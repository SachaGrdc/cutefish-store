#include <cutefish/store/catalogwriter.h>

#include "catalogschema.h"
#include "sqlite.h"
#include "text.h"

#include <map>
#include <set>
#include <system_error>
#include <unordered_map>

namespace cutefish::store {

using detail::Database;
using detail::Statement;
using detail::TextField;

namespace {

constexpr std::size_t kMaxNameBytes = 512;
constexpr std::size_t kMaxSummaryBytes = 1024;
constexpr std::size_t kMaxBlockBytes = 64 * 1024;
constexpr std::size_t kMaxKeywordBytes = 256;

bool isValidLocale(std::string_view locale) noexcept
{
    if (locale.empty() || locale.size() > 64)
        return false;
    for (const char c : locale)
        if (!detail::isAsciiAlnum(c) && c != '_' && c != '@' && c != '-')
            return false;
    return true;
}

// Locales are stored as AppStream gives them, without an encoding:
// "fr_FR.UTF-8@euro" -> "fr_FR@euro", so that the reader's fallback chain
// (fr_FR, fr, C) finds them.
std::string withoutEncoding(std::string_view locale)
{
    const std::size_t dot = locale.find('.');
    if (dot == std::string_view::npos)
        return std::string(locale);
    const std::size_t at = locale.find('@', dot);
    std::string out(locale.substr(0, dot));
    if (at != std::string_view::npos)
        out.append(locale.substr(at));
    return out;
}

char markerOf(DescriptionBlockType type) noexcept
{
    switch (type) {
    case DescriptionBlockType::Paragraph: return detail::kParagraphMarker;
    case DescriptionBlockType::ListItem: return detail::kListItemMarker;
    case DescriptionBlockType::OrderedListItem: return detail::kOrderedItemMarker;
    }
    return detail::kParagraphMarker;
}


} // namespace

struct CatalogWriter::Impl
{
    std::filesystem::path path;
    CatalogBuildOptions options;
    Database db;
    CatalogBuildStats stats;
    bool finished = false;

    std::map<SourceId, std::pair<Source, std::int64_t>> sources; // id -> (source, rowid)
    std::unordered_map<std::string, std::int64_t> apps;           // app_id -> rowid
    std::map<std::string, std::set<std::int64_t>> aliasClaims;    // alias -> claiming apps

    Statement insertSource, insertApp, markVerified, insertOffer, insertText, insertCategory;

    Result<void> prepare()
    {
        struct Item
        {
            Statement* target;
            const char* sql;
        };
        const Item items[] = {
            {&insertSource, "INSERT INTO sources (source_id, backend, scope, kind, origin, label, url, priority, enabled)"
                            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"},
            {&insertApp, "INSERT INTO apps (app_id, verified) VALUES (?, ?)"},
            {&markVerified, "UPDATE apps SET verified = 1 WHERE id = ?"},
            {&insertOffer, "INSERT INTO offers (offer_id, app, source, component_id, format, package, version,"
                           " architecture, developer, icon_kind, icon) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"},
            {&insertText, "INSERT INTO texts (offer, field, locale, value) VALUES (?, ?, ?, ?)"},
            {&insertCategory, "INSERT OR IGNORE INTO app_categories (category, app) VALUES (?, ?)"},
        };
        for (const Item& item : items) {
            auto stmt = Statement::prepare(db, item.sql);
            if (!stmt)
                return std::move(stmt).error();
            *item.target = std::move(stmt).value();
        }
        return {};
    }

    Result<std::int64_t> appRow(const AppId& id, bool verified)
    {
        if (const auto it = apps.find(id.str()); it != apps.end()) {
            if (verified)
                if (auto ok = markVerified.bind(1, it->second).run(); !ok)
                    return std::move(ok).error();
            return it->second;
        }
        if (auto ok = insertApp.bind(1, id.str()).bind(2, std::int64_t(verified)).run(); !ok)
            return std::move(ok).error();
        const std::int64_t row = sqlite3_last_insert_rowid(db.handle());
        apps.emplace(id.str(), row);
        ++stats.applications;
        return row;
    }

    Result<void> addText(std::int64_t offer, TextField field, std::string_view locale, std::string_view value)
    {
        insertText.bind(1, offer).bind(2, static_cast<std::int64_t>(field)).bind(3, locale).bind(4, value);
        if (auto ok = insertText.run(); !ok)
            return std::move(ok).error();
        ++stats.texts;
        return {};
    }
};

namespace {

std::string encodeBlocks(const std::vector<DescriptionBlock>& blocks)
{
    std::string out;
    std::size_t size = 0;
    for (const DescriptionBlock& b : blocks)
        size += b.text.size() + 3;
    out.reserve(size);
    for (const DescriptionBlock& b : blocks) {
        if (!out.empty())
            out += detail::kBlockSeparator;
        out += markerOf(b.type);
        out += detail::kBlockTextStart;
        out += b.text;
    }
    return out;
}

} // namespace

CatalogWriter::CatalogWriter(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}
CatalogWriter::CatalogWriter(CatalogWriter&&) noexcept = default;
CatalogWriter& CatalogWriter::operator=(CatalogWriter&&) noexcept = default;
CatalogWriter::~CatalogWriter() = default;

const CatalogBuildStats& CatalogWriter::stats() const noexcept
{
    return d->stats;
}

Result<CatalogWriter> CatalogWriter::create(const std::filesystem::path& candidate, CatalogBuildOptions options)
{
    std::error_code ec;
    for (const char* suffix : {"", "-journal", "-wal", "-shm"})
        std::filesystem::remove(candidate.string() + suffix, ec);

    auto impl = std::make_unique<Impl>();
    impl->path = candidate;
    impl->options = std::move(options);
    auto db = Database::open("file:" + candidate.string(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (!db)
        return std::move(db).error();
    impl->db = std::move(db).value();
    // The file is a candidate nobody reads yet: no journal and no fsync while
    // building. Durability is restored before the schema version is written
    // (finish()), and the file is fsynced before it replaces anything.
    if (auto ok = impl->db.exec("PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF; PRAGMA page_size = 4096;"
                                "PRAGMA foreign_keys = ON; PRAGMA user_version = 0;");
        !ok)
        return std::move(ok).error();
    if (auto ok = impl->db.exec(detail::kCatalogSchema); !ok)
        return std::move(ok).error();
    if (auto ok = impl->db.exec("BEGIN"); !ok)
        return std::move(ok).error();
    if (auto ok = impl->prepare(); !ok)
        return std::move(ok).error();
    return CatalogWriter(std::move(impl));
}

Result<void> CatalogWriter::addSource(const Source& source)
{
    if (d->finished)
        return Error::conflict("catalogue already finished");
    if (d->sources.contains(source.id()))
        return Error::conflict("source " + source.id().str() + " added twice");
    auto& s = d->insertSource;
    s.bind(1, source.id().str())
            .bind(2, toString(source.backend()))
            .bind(3, toString(source.scope()))
            .bind(4, toString(source.kind()))
            .bind(5, source.origin())
            .bind(6, source.label())
            .bind(7, source.url())
            .bind(8, std::int64_t(source.priority()))
            .bind(9, std::int64_t(source.enabled()));
    if (auto ok = s.run(); !ok)
        return std::move(ok).error();
    d->sources.emplace(source.id(), std::pair{source, sqlite3_last_insert_rowid(d->db.handle())});
    ++d->stats.sources;
    return {};
}

Result<void> CatalogWriter::addComponent(const SourceId& sourceId, ComponentRecord record)
{
    if (d->finished)
        return Error::conflict("catalogue already finished");
    const auto source = d->sources.find(sourceId);
    if (source == d->sources.end())
        return Error::notFound("unknown source " + sourceId.str());

    auto appId = AppId::fromComponentId(record.componentId);
    if (!appId)
        return std::move(appId).error();
    const LocalizedText* untranslatedName = nullptr;
    for (const LocalizedText& n : record.names)
        if (n.locale == "C")
            untranslatedName = &n;
    if (!untranslatedName)
        return Error::invalidArgument("component " + record.componentId + " has no untranslated name");
    if (auto ok = detail::checkText(untranslatedName->text, "name", kMaxNameBytes, true); !ok)
        return std::move(ok).error();
    if (auto ok = detail::checkIcon(record.icon); !ok) {
        record.icon = {};
        ++d->stats.droppedIcons;
    }
    if (!record.developer.empty() && !detail::checkText(record.developer, "developer", kMaxNameBytes, false)) {
        record.developer.clear();
        ++d->stats.droppedTexts;
    }

    const Source& src = source->second.first;
    auto offer = Offer::create(std::move(record.offer), src);
    if (!offer)
        return std::move(offer).error();

    auto app = d->appRow(*appId, record.verified);
    if (!app)
        return std::move(app).error();

    auto& ins = d->insertOffer;
    ins.bind(1, offer->id().str())
            .bind(2, *app)
            .bind(3, source->second.second)
            .bind(4, record.componentId)
            .bind(5, toString(offer->format()))
            .bind(6, offer->package());
    if (offer->version())
        ins.bind(7, offer->version()->str());
    else
        ins.bindNull(7);
    ins.bind(8, offer->architecture())
            .bind(9, record.developer)
            .bind(10, std::int64_t(record.icon.kind))
            .bind(11, record.icon.value);
    if (ins.step() != SQLITE_DONE) {
        const int rc = sqlite3_extended_errcode(d->db.handle());
        Error error = rc == SQLITE_CONSTRAINT_UNIQUE
                ? Error::conflict("application " + appId->str() + " already has offer " + offer->id().str())
                : d->db.error("insert offer");
        ins.reset();
        return error;
    }
    ins.reset();
    const std::int64_t offerRow = sqlite3_last_insert_rowid(d->db.handle());
    ++d->stats.offers;

    auto addAll = [&](const std::vector<LocalizedText>& texts, TextField field, std::size_t max) -> Result<void> {
        std::set<std::string> seen;
        for (const LocalizedText& t : texts) {
            std::string locale = withoutEncoding(t.locale);
            if (!isValidLocale(locale) || !detail::checkText(t.text, "text", max, true) || !seen.insert(locale).second) {
                ++d->stats.droppedTexts;
                continue;
            }
            if (auto ok = d->addText(offerRow, field, locale, t.text); !ok)
                return ok;
        }
        return {};
    };
    if (auto ok = addAll(record.names, TextField::Name, kMaxNameBytes); !ok)
        return ok;
    if (auto ok = addAll(record.summaries, TextField::Summary, kMaxSummaryBytes); !ok)
        return ok;

    std::set<std::string> seenDescriptions;
    for (const LocalizedDescription& desc : record.descriptions) {
        const std::string locale = withoutEncoding(desc.locale);
        bool valid = isValidLocale(locale) && !desc.blocks.empty() && !seenDescriptions.contains(locale);
        for (const DescriptionBlock& b : desc.blocks)
            valid = valid && detail::checkText(b.text, "description", kMaxBlockBytes, true, true).hasValue();
        if (!valid) {
            ++d->stats.droppedTexts;
            continue;
        }
        seenDescriptions.insert(locale);
        const std::string encoded = encodeBlocks(desc.blocks);
        if (auto ok = d->addText(offerRow, TextField::Description, locale, encoded); !ok)
            return ok;
    }

    std::set<std::string> seenKeywords;
    for (const LocalizedKeywords& kw : record.keywords) {
        const std::string locale = withoutEncoding(kw.locale);
        std::string joined;
        for (const std::string& k : kw.keywords) {
            if (!detail::checkText(k, "keyword", kMaxKeywordBytes, true)) {
                ++d->stats.droppedTexts;
                continue;
            }
            if (!joined.empty())
                joined += detail::kKeywordSeparator;
            joined += k;
        }
        if (joined.empty() || !isValidLocale(locale) || !seenKeywords.insert(locale).second)
            continue;
        if (auto ok = d->addText(offerRow, TextField::Keywords, locale, joined); !ok)
            return ok;
    }

    for (const std::string& category : record.categories) {
        if (!detail::checkOpaqueIdentifier(category, "category")) {
            ++d->stats.droppedCategories;
            continue;
        }
        if (auto ok = d->insertCategory.bind(1, category).bind(2, *app).run(); !ok)
            return ok;
    }

    for (const std::string& provided : record.providedIds) {
        auto alias = AppId::fromComponentId(provided);
        if (!alias || *alias == *appId)
            continue;
        d->aliasClaims[alias->str()].insert(*app);
    }
    return {};
}

Result<CatalogBuildStats> CatalogWriter::finish()
{
    if (d->finished)
        return Error::conflict("catalogue already finished");
    Database& db = d->db;

    // Aliases: kept only when one application claims them and no
    // application has that id (ARCHITECTURE §6.4: never merge on a claim).
    {
        auto insertAlias = Statement::prepare(db, "INSERT INTO aliases (alias, app) VALUES (?, ?)");
        if (!insertAlias)
            return std::move(insertAlias).error();
        for (const auto& [alias, claimants] : d->aliasClaims) {
            if (claimants.size() != 1 || d->apps.contains(alias)) {
                ++d->stats.ambiguousAliases;
                continue;
            }
            if (auto ok = insertAlias->bind(1, alias).bind(2, *claimants.begin()).run(); !ok)
                return std::move(ok).error();
            ++d->stats.aliases;
        }
    }

    // Search index, one row per application (rowid = apps.id), built from
    // the texts of all its offers in the indexed languages.
    if (auto ok = db.exec("CREATE TEMP TABLE fts_locales (locale TEXT PRIMARY KEY) WITHOUT ROWID;"
                          "INSERT INTO fts_locales VALUES ('C');");
        !ok)
        return std::move(ok).error();
    {
        auto insertLocale = Statement::prepare(db, "INSERT OR IGNORE INTO fts_locales VALUES (?)");
        if (!insertLocale)
            return std::move(insertLocale).error();
        for (const std::string& locale : d->options.searchLocales) {
            std::string_view l = locale;
            if (const auto dot = l.find('.'); dot != std::string_view::npos)
                l = l.substr(0, dot);
            if (auto ok = insertLocale->bind(1, l).run(); !ok)
                return std::move(ok).error();
            if (const auto sep = l.find_first_of("_@"); sep != std::string_view::npos)
                if (auto ok = insertLocale->bind(1, l.substr(0, sep)).run(); !ok)
                    return std::move(ok).error();
        }
    }
    {
        auto fill = Statement::prepare(db, R"SQL(
INSERT INTO search (rowid, name, keywords, summary, description, developer)
SELECT a.id,
    (SELECT group_concat(DISTINCT t.value) FROM texts t JOIN offers o ON o.id = t.offer
      WHERE o.app = a.id AND t.field = 0 AND t.locale IN (SELECT locale FROM fts_locales)),
    (SELECT group_concat(DISTINCT replace(t.value, char(10), ' ')) FROM texts t JOIN offers o ON o.id = t.offer
      WHERE o.app = a.id AND t.field = 3 AND t.locale IN (SELECT locale FROM fts_locales)),
    (SELECT group_concat(DISTINCT t.value) FROM texts t JOIN offers o ON o.id = t.offer
      WHERE o.app = a.id AND t.field = 1 AND t.locale IN (SELECT locale FROM fts_locales)),
    (SELECT group_concat(DISTINCT substr(replace(replace(t.value, char(30), ' '), char(31), ' '), 1, ?1))
      FROM texts t JOIN offers o ON o.id = t.offer
      WHERE o.app = a.id AND t.field = 2 AND t.locale IN (SELECT locale FROM fts_locales)),
    (SELECT group_concat(DISTINCT o.developer) FROM offers o WHERE o.app = a.id AND o.developer <> '')
FROM apps a
)SQL");
        if (!fill)
            return std::move(fill).error();
        if (auto ok = fill->bind(1, static_cast<std::int64_t>(d->options.descriptionIndexBytes)).run(); !ok)
            return std::move(ok).error();
    }
    if (auto ok = db.exec(R"SQL(
INSERT INTO search_names (rowid, name)
SELECT a.id, (SELECT group_concat(DISTINCT t.value) FROM texts t JOIN offers o ON o.id = t.offer
              WHERE o.app = a.id AND t.field = 0 AND t.locale IN (SELECT locale FROM fts_locales))
FROM apps a;
INSERT INTO search (search) VALUES ('optimize');
INSERT INTO search_names (search_names) VALUES ('optimize');
DROP TABLE fts_locales;
)SQL");
        !ok)
        return std::move(ok).error();

    // Metadata, including the number of offers per source for the
    // plausibility check of a replacement (ARCHITECTURE §6.2).
    {
        auto meta = Statement::prepare(db, "INSERT INTO meta (key, value) VALUES (?, ?)");
        if (!meta)
            return std::move(meta).error();
        auto put = [&](std::string_view key, std::string_view value) { return meta->bind(1, key).bind(2, value).run(); };
        const std::string generation = std::to_string(d->options.generation);
        const std::string schema = std::to_string(kCatalogSchemaVersion);
        std::string locales;
        for (const std::string& l : d->options.searchLocales)
            locales += (locales.empty() ? "" : ",") + l;
        for (auto ok : {put("schema", schema), put("generation", generation), put("search_locales", locales)})
            if (!ok)
                return std::move(ok).error();
        auto counts = Statement::prepare(db, "SELECT s.source_id, count(o.id) FROM sources s"
                                             " LEFT JOIN offers o ON o.source = s.id GROUP BY s.id");
        if (!counts)
            return std::move(counts).error();
        while (counts->step() == SQLITE_ROW) {
            const std::string key = "offers:" + std::string(counts->columnText(0));
            const std::string value = std::to_string(counts->columnInt(1));
            if (auto ok = put(key, value); !ok)
                return std::move(ok).error();
        }
    }

    if (auto ok = db.exec("COMMIT; ANALYZE;"); !ok)
        return std::move(ok).error();
    // From here on, a rollback journal and fsync: the schema version, which
    // marks the file as complete, is written in a transaction of its own,
    // after everything else has reached the disk.
    if (auto ok = db.exec("PRAGMA journal_mode = DELETE; PRAGMA synchronous = FULL;"); !ok)
        return std::move(ok).error();
    const std::string version = "PRAGMA user_version = " + std::to_string(kCatalogSchemaVersion);
    if (auto ok = db.exec(version.c_str()); !ok)
        return std::move(ok).error();

    d->insertSource = {};
    d->insertApp = {};
    d->markVerified = {};
    d->insertOffer = {};
    d->insertText = {};
    d->insertCategory = {};
    d->db.close();
    d->finished = true;
    return d->stats;
}

} // namespace cutefish::store
