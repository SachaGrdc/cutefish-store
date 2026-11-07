#include <cutefish/store/sqlitecatalog.h>

#include <cutefish/store/catalogwriter.h>

#include "catalogschema.h"
#include "sqlite.h"
#include "sqliteuri.h"
#include "text.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace cutefish::store {

using detail::Database;
using detail::Statement;

namespace {

// "fr_FR.UTF-8" -> fr_FR, fr ; "sr_RS@latin" -> sr_RS@latin, sr@latin, sr_RS, sr.
void appendLocaleChain(std::string_view locale, std::vector<std::string>& out)
{
    auto add = [&](std::string value) {
        if (!value.empty() && value != "C" && std::find(out.begin(), out.end(), value) == out.end())
            out.push_back(std::move(value));
    };
    std::string_view modifier;
    if (const auto at = locale.find('@'); at != std::string_view::npos) {
        modifier = locale.substr(at);
        locale = locale.substr(0, at);
    }
    if (const auto dot = locale.find('.'); dot != std::string_view::npos)
        locale = locale.substr(0, dot);
    const std::string_view language = locale.substr(0, locale.find('_'));
    if (!modifier.empty()) {
        add(std::string(locale) + std::string(modifier));
        add(std::string(language) + std::string(modifier));
    }
    add(std::string(locale));
    add(std::string(language));
}

bool isTokenByte(unsigned char c) noexcept
{
    return c >= 0x80 || detail::isAsciiAlnum(static_cast<char>(c));
}

// User text -> FTS5 query: words split like the unicode61 tokenizer splits
// ASCII, each quoted and used as a prefix, implicitly ANDed; at most 8.
// The text is never used as FTS5 syntax (ARCHITECTURE §6.3).
std::string wordQuery(std::string_view text)
{
    std::string query;
    int terms = 0;
    std::size_t i = 0;
    while (i < text.size() && terms < 8) {
        while (i < text.size() && !isTokenByte(static_cast<unsigned char>(text[i])))
            ++i;
        const std::size_t start = i;
        while (i < text.size() && isTokenByte(static_cast<unsigned char>(text[i])))
            ++i;
        if (i > start) {
            if (!query.empty())
                query += ' ';
            query += '"';
            query.append(text.substr(start, i - start));
            query += "\"*";
            ++terms;
        }
    }
    return query;
}

std::vector<std::string_view> codepoints(std::string_view text)
{
    std::vector<std::string_view> out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t length = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        out.push_back(text.substr(i, std::min(length, text.size() - i)));
        i += length;
    }
    return out;
}

std::string ftsQuoted(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"')
            out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::string asciiLower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::size_t editDistance(std::string_view a, std::string_view b)
{
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
        row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t previous = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t current = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, previous + (a[i - 1] == b[j - 1] ? 0 : 1)});
            previous = current;
        }
    }
    return row[b.size()];
}

std::vector<DescriptionBlock> decodeBlocks(std::string_view value)
{
    std::vector<DescriptionBlock> blocks;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t end = value.find(detail::kBlockSeparator, start);
        if (end == std::string_view::npos)
            end = value.size();
        const std::string_view block = value.substr(start, end - start);
        if (block.size() >= 2 && block[1] == detail::kBlockTextStart) {
            const DescriptionBlockType type = block[0] == detail::kListItemMarker ? DescriptionBlockType::ListItem
                    : block[0] == detail::kOrderedItemMarker                      ? DescriptionBlockType::OrderedListItem
                                                                                  : DescriptionBlockType::Paragraph;
            if (block.size() > 2)
                blocks.push_back({type, std::string(block.substr(2))});
        }
        start = end + 1;
    }
    return blocks;
}

struct OfferRow
{
    std::int64_t row;
    std::int64_t sourceRow;
    std::string offerId;
    std::string format;
    std::string package;
    std::optional<std::string> version;
    std::string architecture;
    std::string developer;
    IconRef icon;
};

} // namespace

struct SqliteCatalog::Impl
{
    Database db;
    std::uint64_t generation = 0;
    std::vector<std::string> locales; // preference chain, then "C"
    std::unordered_map<std::int64_t, Source> sources;

    mutable Statement appById, appByAlias, appByRow, offersOfApp, displayTexts, allTexts, categoriesOfApp,
            aliasesOfApp, exactNames, prefixNames, words, substrings, fuzzyCandidates, namesOfApp, inCategory,
            hasBackend;

    // Result list of the last search (application rows, ranked), reused when
    // only the page changes.
    mutable std::string lastKey;
    mutable std::vector<std::int64_t> lastResults;

    std::size_t localeRank(std::string_view locale) const noexcept
    {
        for (std::size_t i = 0; i < locales.size(); ++i)
            if (locales[i] == locale)
                return i;
        return locales.size();
    }

    std::string localeList() const
    {
        std::string list;
        for (std::size_t i = 0; i < locales.size(); ++i)
            list += i ? ", ?" : "?";
        return list;
    }

    void bindLocales(Statement& s, int first) const
    {
        for (std::size_t i = 0; i < locales.size(); ++i)
            s.bind(first + static_cast<int>(i), locales[i]);
    }

    Result<void> loadSources()
    {
        auto s = Statement::prepare(db, "SELECT id, source_id, backend, scope, kind, origin, label, url, priority,"
                                        " enabled FROM sources");
        if (!s)
            return std::move(s).error();
        while (s->step() == SQLITE_ROW) {
            auto id = SourceId::parse(s->columnText(1));
            const auto backend = parseBackendKind(s->columnText(2));
            const auto scope = parseScope(s->columnText(3));
            const auto kind = parseSourceKind(s->columnText(4));
            if (!id || !backend || !scope || !kind)
                return Error::internal("invalid source row in catalogue");
            auto source = Source::create({.id = std::move(id).value(),
                                          .backend = *backend,
                                          .scope = *scope,
                                          .kind = *kind,
                                          .origin = std::string(s->columnText(5)),
                                          .label = std::string(s->columnText(6)),
                                          .url = std::string(s->columnText(7)),
                                          .enabled = s->columnInt(9) != 0,
                                          .priority = static_cast<std::int32_t>(s->columnInt(8))});
            if (!source)
                return Error::internal("invalid source in catalogue: " + source.error().message());
            sources.emplace(s->columnInt(0), std::move(source).value());
        }
        return {};
    }

    Result<void> prepare()
    {
        const std::string inLocales = "(" + localeList() + ")";
        const std::string display = "SELECT t.offer, t.field, t.locale, t.value FROM texts t JOIN offers o ON o.id = t.offer"
                                    " WHERE o.app = ?1 AND t.field IN (0, 1) AND t.locale IN " + inLocales;
        const std::string all = "SELECT t.offer, t.field, t.locale, t.value FROM texts t JOIN offers o ON o.id = t.offer"
                                " WHERE o.app = ?1 AND t.field IN (0, 1, 2) AND t.locale IN " + inLocales;
        const std::string names = "SELECT t.value FROM texts t JOIN offers o ON o.id = t.offer"
                                  " WHERE o.app = ?1 AND t.field = 0 AND t.locale IN " + inLocales;
        struct Item
        {
            Statement* target;
            std::string sql;
        };
        const Item items[] = {
            {&appById, "SELECT id, app_id, verified FROM apps WHERE app_id = ?"},
            {&appByAlias, "SELECT a.id, a.app_id, a.verified FROM aliases x JOIN apps a ON a.id = x.app WHERE x.alias = ?"},
            {&appByRow, "SELECT id, app_id, verified FROM apps WHERE id = ?"},
            {&offersOfApp, "SELECT id, source, offer_id, format, package, version, architecture, developer, icon_kind, icon"
                           " FROM offers WHERE app = ?"},
            {&displayTexts, display},
            {&allTexts, all},
            {&categoriesOfApp, "SELECT category FROM app_categories WHERE app = ? ORDER BY category"},
            {&aliasesOfApp, "SELECT alias FROM aliases WHERE app = ? ORDER BY alias"},
            {&exactNames, "SELECT o.app FROM texts t JOIN offers o ON o.id = t.offer JOIN apps a ON a.id = o.app"
                          " WHERE t.field = 0 AND t.value = ?1 COLLATE NOCASE"
                          " GROUP BY o.app ORDER BY min(length(t.value)), a.app_id"},
            {&prefixNames, "SELECT o.app FROM texts t JOIN offers o ON o.id = t.offer JOIN apps a ON a.id = o.app"
                           " WHERE t.field = 0 AND t.value LIKE ?1 ESCAPE '\\'"
                           " GROUP BY o.app ORDER BY min(length(t.value)), a.app_id"},
            {&words, "SELECT search.rowid FROM search JOIN apps a ON a.id = search.rowid WHERE search MATCH ?1"
                     " ORDER BY bm25(search, 10.0, 5.0, 2.0, 0.5, 1.0), a.app_id"},
            {&substrings, "SELECT search_names.rowid FROM search_names JOIN apps a ON a.id = search_names.rowid"
                          " WHERE search_names MATCH ?1 ORDER BY bm25(search_names), a.app_id"},
            {&fuzzyCandidates, "SELECT rowid FROM search_names WHERE search_names MATCH ?1"
                               " ORDER BY bm25(search_names) LIMIT 200"},
            {&namesOfApp, names},
            {&inCategory, "SELECT 1 FROM app_categories WHERE category = ?1 AND app = ?2"},
            {&hasBackend, "SELECT 1 FROM offers o JOIN sources s ON s.id = o.source WHERE o.app = ?1 AND s.backend = ?2"
                          " LIMIT 1"},
        };
        for (const Item& item : items) {
            auto stmt = Statement::prepare(db, item.sql.c_str());
            if (!stmt)
                return std::move(stmt).error();
            *item.target = std::move(stmt).value();
        }
        return {};
    }

    struct AppRow
    {
        std::int64_t row;
        std::string appId;
        bool verified;
    };

    std::optional<AppRow> readApp(Statement& s) const
    {
        std::optional<AppRow> out;
        if (s.step() == SQLITE_ROW)
            out = AppRow{s.columnInt(0), std::string(s.columnText(1)), s.columnInt(2) != 0};
        s.reset();
        return out;
    }

    std::optional<AppRow> findApp(const AppId& id) const
    {
        appById.bind(1, id.str());
        if (auto row = readApp(appById))
            return row;
        appByAlias.bind(1, id.str());
        return readApp(appByAlias);
    }

    // Offers sorted by source priority (highest first), then offer id.
    std::vector<OfferRow> offers(std::int64_t app) const
    {
        std::vector<OfferRow> out;
        offersOfApp.bind(1, app);
        while (offersOfApp.step() == SQLITE_ROW) {
            OfferRow o{.row = offersOfApp.columnInt(0),
                       .sourceRow = offersOfApp.columnInt(1),
                       .offerId = std::string(offersOfApp.columnText(2)),
                       .format = std::string(offersOfApp.columnText(3)),
                       .package = std::string(offersOfApp.columnText(4)),
                       .version = std::nullopt,
                       .architecture = std::string(offersOfApp.columnText(6)),
                       .developer = std::string(offersOfApp.columnText(7)),
                       .icon = {static_cast<IconKind>(offersOfApp.columnInt(8)), std::string(offersOfApp.columnText(9))}};
            if (!offersOfApp.columnIsNull(5))
                o.version = std::string(offersOfApp.columnText(5));
            out.push_back(std::move(o));
        }
        offersOfApp.reset();
        std::sort(out.begin(), out.end(), [&](const OfferRow& a, const OfferRow& b) {
            const auto pa = sourcePriority(a.sourceRow);
            const auto pb = sourcePriority(b.sourceRow);
            return pa != pb ? pa > pb : a.offerId < b.offerId;
        });
        return out;
    }

    std::int64_t sourcePriority(std::int64_t sourceRow) const
    {
        const auto it = sources.find(sourceRow);
        return it == sources.end() ? std::numeric_limits<std::int32_t>::min() : it->second.priority();
    }

    // Best text per field: best language first, then the offer order.
    struct Texts
    {
        std::string name, summary, description;
    };

    Texts chooseTexts(std::int64_t app, const std::vector<OfferRow>& offerRows, bool withDescription) const
    {
        std::unordered_map<std::int64_t, std::size_t> offerRank;
        for (std::size_t i = 0; i < offerRows.size(); ++i)
            offerRank.emplace(offerRows[i].row, i);
        struct Best
        {
            std::size_t locale = std::numeric_limits<std::size_t>::max();
            std::size_t offer = std::numeric_limits<std::size_t>::max();
            std::string value;
        } best[3];
        Statement& s = withDescription ? allTexts : displayTexts;
        s.bind(1, app);
        bindLocales(s, 2);
        while (s.step() == SQLITE_ROW) {
            const auto field = static_cast<std::size_t>(s.columnInt(1));
            if (field > 2)
                continue;
            const std::size_t locale = localeRank(s.columnText(2));
            const auto it = offerRank.find(s.columnInt(0));
            const std::size_t offer = it == offerRank.end() ? offerRows.size() : it->second;
            Best& b = best[field];
            if (locale < b.locale || (locale == b.locale && offer < b.offer)) {
                b.locale = locale;
                b.offer = offer;
                b.value = std::string(s.columnText(3));
            }
        }
        s.reset();
        return {std::move(best[0].value), std::move(best[1].value), std::move(best[2].value)};
    }

    Result<AppSummary> summary(std::int64_t app) const
    {
        appByRow.bind(1, app);
        const auto row = readApp(appByRow);
        if (!row)
            return Error::notFound("no application row " + std::to_string(app));
        auto id = AppId::parse(row->appId);
        if (!id)
            return Error::internal("invalid application id in catalogue: " + row->appId);
        const std::vector<OfferRow> offerRows = offers(app);
        Texts texts = chooseTexts(app, offerRows, false);
        AppSummary s{.id = std::move(id).value(), .name = std::move(texts.name), .summary = std::move(texts.summary)};
        s.verified = row->verified;
        for (const OfferRow& o : offerRows) {
            if (s.icon.kind == IconKind::None && o.icon.kind != IconKind::None)
                s.icon = o.icon;
            if (s.developer.empty())
                s.developer = o.developer;
            if (const auto it = sources.find(o.sourceRow); it != sources.end())
                s.backends.insert(it->second.backend());
        }
        return s;
    }

    std::vector<std::int64_t> collect(Statement& s) const
    {
        std::vector<std::int64_t> out;
        while (s.step() == SQLITE_ROW)
            out.push_back(s.columnInt(0));
        s.reset();
        return out;
    }

    std::vector<std::int64_t> rank(const std::string& text) const
    {
        std::vector<std::int64_t> results;
        std::unordered_set<std::int64_t> seen;
        auto append = [&](const std::vector<std::int64_t>& tier) {
            for (const std::int64_t id : tier)
                if (seen.insert(id).second)
                    results.push_back(id);
        };

        // 1. Exact name (ASCII case-insensitive).
        exactNames.bind(1, text);
        append(collect(exactNames));

        // 2. Name prefix.
        std::string like;
        like.reserve(text.size() + 2);
        for (const char c : text) {
            if (c == '%' || c == '_' || c == '\\')
                like += '\\';
            like += c;
        }
        like += '%';
        prefixNames.bind(1, like);
        append(collect(prefixNames));

        // 3. Words, accent- and case-insensitive (unicode61), bm25 weighted.
        if (const std::string q = wordQuery(text); !q.empty()) {
            words.bind(1, q);
            append(collect(words));
        }

        // 4. Substring of a name (trigrams), from three characters.
        const auto chars = codepoints(text);
        if (chars.size() >= 3) {
            const std::string q = ftsQuoted(text);
            substrings.bind(1, q);
            append(collect(substrings));
        }

        // 5. Approximate names, only when nothing else matched.
        if (results.empty() && chars.size() >= 3)
            append(fuzzy(text));
        return results;
    }

    std::vector<std::int64_t> fuzzy(const std::string& text) const
    {
        const std::string lowered = asciiLower(text);
        const auto chars = codepoints(lowered);
        std::string q;
        for (std::size_t i = 0; i + 3 <= chars.size() && i < 40; ++i) {
            const std::string_view tri(chars[i].data(), chars[i + 2].data() + chars[i + 2].size() - chars[i].data());
            if (!q.empty())
                q += " OR ";
            q += ftsQuoted(tri);
        }
        fuzzyCandidates.bind(1, q);
        const std::vector<std::int64_t> candidates = collect(fuzzyCandidates);
        struct Scored
        {
            std::size_t distance;
            std::string appId;
            std::int64_t row;
        };
        std::vector<Scored> kept;
        for (const std::int64_t app : candidates) {
            std::size_t best = std::numeric_limits<std::size_t>::max();
            namesOfApp.bind(1, app);
            bindLocales(namesOfApp, 2);
            while (namesOfApp.step() == SQLITE_ROW) {
                const std::string name = asciiLower(namesOfApp.columnText(0));
                const std::string_view head = std::string_view(name).substr(0, lowered.size() + 2);
                best = std::min({best, editDistance(lowered, name), editDistance(lowered, head)});
            }
            namesOfApp.reset();
            if (best <= 2) {
                appByRow.bind(1, app);
                if (auto row = readApp(appByRow))
                    kept.push_back({best, row->appId, app});
            }
        }
        std::sort(kept.begin(), kept.end(), [](const Scored& a, const Scored& b) {
            return a.distance != b.distance ? a.distance < b.distance : a.appId < b.appId;
        });
        std::vector<std::int64_t> out;
        out.reserve(kept.size());
        for (const Scored& s : kept)
            out.push_back(s.row);
        return out;
    }

    bool accept(std::int64_t app, const SearchOptions& o) const
    {
        if (!o.category.empty()) {
            inCategory.bind(1, o.category).bind(2, app);
            const bool ok = inCategory.step() == SQLITE_ROW;
            inCategory.reset();
            if (!ok)
                return false;
        }
        if (o.backend) {
            hasBackend.bind(1, app).bind(2, toString(*o.backend));
            const bool ok = hasBackend.step() == SQLITE_ROW;
            hasBackend.reset();
            if (!ok)
                return false;
        }
        return true;
    }
};

SqliteCatalog::SqliteCatalog(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}
SqliteCatalog::SqliteCatalog(SqliteCatalog&&) noexcept = default;
SqliteCatalog& SqliteCatalog::operator=(SqliteCatalog&&) noexcept = default;
SqliteCatalog::~SqliteCatalog() = default;

Result<SqliteCatalog> SqliteCatalog::open(const std::filesystem::path& path, CatalogReadOptions options)
{
    auto impl = std::make_unique<Impl>();
    auto db = Database::open(detail::readOnlyUri(path, true), SQLITE_OPEN_READONLY);
    if (!db)
        return std::move(db).error();
    impl->db = std::move(db).value();
    auto version = impl->db.queryInt("PRAGMA user_version");
    if (!version)
        return Error::invalidArgument("not a catalogue: " + version.error().message());
    if (*version != kCatalogSchemaVersion)
        return Error::invalidArgument("catalogue schema version " + std::to_string(*version) + ", expected " +
                                      std::to_string(kCatalogSchemaVersion));
    auto generation = Statement::prepare(impl->db, "SELECT value FROM meta WHERE key = 'generation'");
    if (!generation)
        return std::move(generation).error();
    if (generation->step() == SQLITE_ROW)
        impl->generation = std::stoull(std::string(generation->columnText(0)));
    for (const std::string& l : options.locales)
        appendLocaleChain(l, impl->locales);
    impl->locales.push_back("C");
    if (auto ok = impl->loadSources(); !ok)
        return std::move(ok).error();
    if (auto ok = impl->prepare(); !ok)
        return std::move(ok).error();
    return SqliteCatalog(std::move(impl));
}

std::uint64_t SqliteCatalog::generation() const noexcept
{
    return d->generation;
}

Result<Application> SqliteCatalog::application(const AppId& id) const
{
    const auto row = d->findApp(id);
    if (!row)
        return Error::notFound("no application " + id.str());
    auto appId = AppId::parse(row->appId);
    if (!appId)
        return Error::internal("invalid application id in catalogue: " + row->appId);

    const std::vector<OfferRow> offerRows = d->offers(row->row);
    Impl::Texts texts = d->chooseTexts(row->row, offerRows, true);
    ApplicationSpec spec{.id = std::move(appId).value(),
                         .name = std::move(texts.name),
                         .summary = std::move(texts.summary),
                         .description = decodeBlocks(texts.description)};
    for (const OfferRow& o : offerRows) {
        if (spec.icon.kind == IconKind::None && o.icon.kind != IconKind::None)
            spec.icon = o.icon;
        const auto source = d->sources.find(o.sourceRow);
        const auto format = parsePackageFormat(o.format);
        if (source == d->sources.end() || !format)
            return Error::internal("invalid offer row " + o.offerId);
        std::optional<Version> version;
        if (o.version) {
            auto v = *format == PackageFormat::Deb ? Version::debian(*o.version) : Version::upstream(*o.version);
            if (!v)
                return Error::internal("invalid version in offer " + o.offerId);
            version = std::move(v).value();
        }
        auto offer = Offer::create({.format = *format, .package = o.package, .version = std::move(version),
                                    .architecture = o.architecture},
                                   source->second);
        if (!offer)
            return Error::internal("invalid offer " + o.offerId + ": " + offer.error().message());
        spec.offers.push_back(std::move(offer).value());
    }
    d->categoriesOfApp.bind(1, row->row);
    while (d->categoriesOfApp.step() == SQLITE_ROW)
        spec.categories.emplace_back(d->categoriesOfApp.columnText(0));
    d->categoriesOfApp.reset();
    d->aliasesOfApp.bind(1, row->row);
    while (d->aliasesOfApp.step() == SQLITE_ROW)
        if (auto alias = AppId::parse(d->aliasesOfApp.columnText(0)))
            spec.aliases.push_back(std::move(alias).value());
    d->aliasesOfApp.reset();

    auto app = Application::create(std::move(spec));
    if (!app)
        return Error::internal("invalid application " + id.str() + " in catalogue: " + app.error().message());
    return app;
}

Result<SearchPage> SqliteCatalog::search(const SearchQuery& query) const
{
    const SearchOptions& o = query.options();
    if (o.installedOnly)
        return Error::notSupported("installed-only search needs state.sqlite");
    SearchPage page{.generation = d->generation};
    if (query.text().empty())
        return page;

    std::string key = query.text();
    key += '\0';
    key += o.category;
    key += '\0';
    key += o.backend ? toString(*o.backend) : "";
    if (key != d->lastKey) {
        std::vector<std::int64_t> ranked = d->rank(query.text());
        if (!o.category.empty() || o.backend)
            std::erase_if(ranked, [&](std::int64_t app) { return !d->accept(app, o); });
        d->lastResults = std::move(ranked);
        d->lastKey = std::move(key);
    }
    const std::vector<std::int64_t>& results = d->lastResults;
    page.total = static_cast<std::uint32_t>(std::min<std::size_t>(results.size(), UINT32_MAX));
    const std::size_t begin = std::min<std::size_t>(o.offset, results.size());
    const std::size_t end = std::min<std::size_t>(begin + o.limit, results.size());
    page.items.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        auto s = d->summary(results[i]);
        if (!s)
            return std::move(s).error();
        page.items.push_back(std::move(s).value());
    }
    return page;
}

Result<std::vector<RecordedSource>> SqliteCatalog::sources() const
{
    auto stmt = Statement::prepare(
            d->db, "SELECT s.id, coalesce(CAST(c.value AS INTEGER), 0), f.value, json_valid(f.value),"
                   " json_extract(f.value, '$.path'), json_type(f.value, '$.size'), json_extract(f.value, '$.size'),"
                   " json_type(f.value, '$.mtimeNs'), json_extract(f.value, '$.mtimeNs'),"
                   " json_extract(f.value, '$.sha256'), json_extract(f.value, '$.icons')"
                   " FROM sources s"
                   " LEFT JOIN meta c ON c.key = 'offers:' || s.source_id"
                   " LEFT JOIN meta f ON f.key = 'fingerprint:' || s.source_id"
                   " ORDER BY s.source_id");
    if (!stmt)
        return std::move(stmt).error();
    std::vector<RecordedSource> out;
    int rc;
    while ((rc = stmt->step()) == SQLITE_ROW) {
        const auto it = d->sources.find(stmt->columnInt(0));
        if (it == d->sources.end())
            return Error::internal("source row missing from the catalogue");
        RecordedSource r{.source = it->second, .fingerprint = std::nullopt, .offers = stmt->columnInt(1)};
        if (!stmt->columnIsNull(2)) {
            // A malformed fingerprint is reported as absent: the source then
            // counts as modified and is imported again, which is safe.
            const bool usable = stmt->columnInt(3) == 1 && stmt->columnText(5) == "integer"
                    && stmt->columnText(7) == "integer" && stmt->columnInt(6) >= 0
                    && stmt->columnText(9).size() == 64 && !stmt->columnText(4).empty();
            if (usable)
                r.fingerprint = SourceFingerprint{.path = std::string(stmt->columnText(4)),
                                                  .size = static_cast<std::uint64_t>(stmt->columnInt(6)),
                                                  .mtimeNs = stmt->columnInt(8),
                                                  .sha256 = std::string(stmt->columnText(9)),
                                                  .iconsPath = std::string(stmt->columnText(10))};
        }
        out.push_back(std::move(r));
    }
    if (rc != SQLITE_DONE)
        return d->db.error("reading the catalogue sources");
    return out;
}

Result<std::vector<AppSummary>> SqliteCatalog::summaries(std::span<const AppId> ids) const
{
    std::vector<AppSummary> out;
    out.reserve(ids.size());
    for (const AppId& id : ids) {
        d->appById.bind(1, id.str());
        const auto row = d->readApp(d->appById);
        if (!row)
            continue;
        auto s = d->summary(row->row);
        if (!s)
            return std::move(s).error();
        out.push_back(std::move(s).value());
    }
    return out;
}

} // namespace cutefish::store
