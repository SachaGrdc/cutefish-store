#include "importer.h"

#include "markup.h"

#include <appstream.h>
#include <libxml/xmlreader.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include <sys/resource.h>

namespace cutefish::store::indexer {

namespace {

constexpr const char* kVerifiedKey = "flathub::verification::verified";
constexpr std::size_t kExamplesPerSource = 8;

std::string str(const char* s)
{
    return s ? std::string(s) : std::string();
}

template <typename T>
struct GObjectPtr
{
    T* p = nullptr;
    explicit GObjectPtr(T* object) : p(object) {}
    ~GObjectPtr()
    {
        if (p)
            g_object_unref(p);
    }
    GObjectPtr(GObjectPtr&& other) noexcept : p(std::exchange(other.p, nullptr)) {}
    GObjectPtr(const GObjectPtr&) = delete;
    GObjectPtr& operator=(const GObjectPtr&) = delete;
    GObjectPtr& operator=(GObjectPtr&&) = delete;
};

bool isYaml(const std::filesystem::path& file)
{
    std::filesystem::path p = file;
    if (p.extension() == ".gz")
        p = p.stem();
    return p.extension() == ".yml" || p.extension() == ".yaml";
}

std::vector<LocalizedText> textTable(GHashTable* table)
{
    std::vector<LocalizedText> out;
    if (!table)
        return out;
    GHashTableIter it;
    gpointer key;
    gpointer value;
    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        std::string text = normalizeSpace(static_cast<const char*>(value));
        if (!text.empty())
            out.push_back({static_cast<const char*>(key), std::move(text)});
    }
    // Hash order is not stable: sort, so that builds are reproducible.
    std::sort(out.begin(), out.end(), [](const LocalizedText& a, const LocalizedText& b) { return a.locale < b.locale; });
    return out;
}

std::vector<LocalizedKeywords> keywordTable(GHashTable* table)
{
    std::vector<LocalizedKeywords> out;
    if (!table)
        return out;
    GHashTableIter it;
    gpointer key;
    gpointer value;
    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        auto* array = static_cast<GPtrArray*>(value);
        LocalizedKeywords k{static_cast<const char*>(key), {}};
        for (guint i = 0; array && i < array->len; ++i) {
            std::string word = normalizeSpace(static_cast<const char*>(g_ptr_array_index(array, i)));
            if (!word.empty())
                k.keywords.push_back(std::move(word));
        }
        if (!k.keywords.empty())
            out.push_back(std::move(k));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.locale < b.locale; });
    return out;
}

// Best icon: a cached icon present on disk (64 then 128 px), then a remote
// https icon, then a stock name.
IconRef chooseIcon(AsComponent* cpt, const std::filesystem::path& iconRoot)
{
    GPtrArray* icons = as_component_get_icons(cpt);
    IconRef remote;
    IconRef stock;
    IconRef cached;
    int cachedScore = 0;
    for (guint i = 0; icons && i < icons->len; ++i) {
        auto* icon = static_cast<AsIcon*>(g_ptr_array_index(icons, i));
        switch (as_icon_get_kind(icon)) {
        case AS_ICON_KIND_CACHED: {
            if (iconRoot.empty() || !as_icon_get_name(icon))
                break;
            const guint width = as_icon_get_width(icon);
            const guint height = as_icon_get_height(icon);
            const guint scale = std::max(1u, as_icon_get_scale(icon));
            const int score = scale == 1 && width == 64 ? 3 : scale == 1 && width == 128 ? 2 : 1;
            if (score <= cachedScore)
                break;
            std::string size = std::to_string(width) + "x" + std::to_string(height);
            if (scale > 1)
                size += "@" + std::to_string(scale);
            const std::filesystem::path path = iconRoot / size / as_icon_get_name(icon);
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec)) {
                cached = {IconKind::LocalPath, path.string()};
                cachedScore = score;
            }
            break;
        }
        case AS_ICON_KIND_REMOTE: {
            const std::string url = str(as_icon_get_url(icon));
            if (remote.kind == IconKind::None && url.starts_with("https://"))
                remote = {IconKind::RemoteUrl, url};
            break;
        }
        case AS_ICON_KIND_STOCK:
            if (stock.kind == IconKind::None && as_icon_get_name(icon))
                stock = {IconKind::StockName, as_icon_get_name(icon)};
            break;
        default:
            break;
        }
    }
    if (cached.kind != IconKind::None)
        return cached;
    if (remote.kind != IconKind::None)
        return remote;
    return stock;
}

struct Extracted
{
    std::optional<ComponentRecord> record;
    std::string skipReason;
};

class Extractor
{
public:
    Extractor(const SourceConfig& config, const ImportOptions& options) : m_config(config), m_options(options) {}

    Extracted extract(AsComponent* cpt) const
    {
        if (as_component_get_kind(cpt) != AS_COMPONENT_KIND_DESKTOP_APP)
            return {std::nullopt, "not a desktop application"};
        const std::string id = str(as_component_get_id(cpt));
        if (id.empty())
            return {std::nullopt, "no id"};

        // The plain getters follow the process language when the metadata
        // locale is ALL (prototype 3): read the untranslated text explicitly.
        as_component_set_context_locale(cpt, "C");

        OfferSpec offer;
        if (m_config.source.backend() == BackendKind::PackageKit) {
            // Transfer none, despite the non-const return type.
            offer.format = PackageFormat::Deb;
            offer.package = str(as_component_get_pkgname(cpt));
            if (offer.package.empty())
                return {std::nullopt, "no package name"};
            // DEP-11 gives no package version: PackageKit provides it.
        } else {
            AsBundle* bundle = as_component_get_bundle(cpt, AS_BUNDLE_KIND_FLATPAK);
            offer.format = PackageFormat::Flatpak;
            offer.package = bundle ? str(as_bundle_get_id(bundle)) : std::string();
            if (offer.package.empty())
                return {std::nullopt, "no Flatpak bundle"};
            AsReleaseList* releases = as_component_get_releases_plain(cpt);
            if (releases && as_release_list_get_size(releases) > 0)
                if (auto version = Version::upstream(str(as_release_get_version(as_release_list_index_safe(releases, 0)))))
                    offer.version = std::move(version).value();
        }

        ComponentRecord r{.componentId = id, .offer = std::move(offer), .names = textTable(as_component_get_name_table(cpt))};
        r.summaries = textTable(as_component_get_summary_table(cpt));
        r.keywords = keywordTable(as_component_get_keywords_table(cpt));
        r.icon = chooseIcon(cpt, m_config.icons);
        if (GPtrArray* categories = as_component_get_categories(cpt))
            for (guint i = 0; i < categories->len; ++i)
                r.categories.emplace_back(static_cast<const char*>(g_ptr_array_index(categories, i)));
        if (AsProvided* provided = as_component_get_provided_for_kind(cpt, AS_PROVIDED_KIND_ID))
            if (GPtrArray* items = as_provided_get_items(provided))
                for (guint i = 0; i < items->len; ++i)
                    r.providedIds.emplace_back(static_cast<const char*>(g_ptr_array_index(items, i)));
        if (AsDeveloper* developer = as_component_get_developer(cpt))
            r.developer = normalizeSpace(str(as_developer_get_name(developer)));
        r.verified = str(as_component_get_custom_value(cpt, kVerifiedKey)) == "true";

        // Descriptions: AppStream 1.0 has no table for them; each language
        // is read by switching the component's context locale. Translations
        // equal to the untranslated text are not stored.
        const std::string untranslated = str(as_component_get_description(cpt));
        if (!untranslated.empty())
            r.descriptions.push_back({"C", descriptionBlocks(untranslated)});
        for (const std::string& locale : descriptionLocales(r)) {
            as_component_set_context_locale(cpt, locale.c_str());
            const std::string translated = str(as_component_get_description(cpt));
            if (!translated.empty() && translated != untranslated)
                r.descriptions.push_back({locale, descriptionBlocks(translated)});
        }
        as_component_set_context_locale(cpt, "C");
        std::erase_if(r.descriptions, [](const LocalizedDescription& d) { return d.blocks.empty(); });
        return {std::move(r), {}};
    }

private:
    std::vector<std::string> descriptionLocales(const ComponentRecord& r) const
    {
        if (!m_options.allDescriptionLocales)
            return m_options.descriptionLocales;
        std::set<std::string> locales;
        for (const auto& t : r.names)
            locales.insert(t.locale);
        for (const auto& t : r.summaries)
            locales.insert(t.locale);
        for (const auto& k : r.keywords)
            locales.insert(k.locale);
        locales.erase("C");
        return {locales.begin(), locales.end()};
    }

    const SourceConfig& m_config;
    const ImportOptions& m_options;
};

class SourceImport
{
public:
    SourceImport(CatalogWriter& writer, const SourceConfig& config, const ImportOptions& options)
        : m_writer(writer), m_config(config), m_extractor(config, options)
    {
        m_report.sourceId = config.source.id().str();
    }

    void skip(const std::string& reason, const std::string& id)
    {
        ++m_report.skipped[reason];
        if (m_report.examples.size() < kExamplesPerSource)
            m_report.examples.push_back(reason + ": " + id);
    }

    void take(AsComponentBox* box)
    {
        for (guint i = 0; i < as_component_box_get_size(box); ++i) {
            AsComponent* cpt = as_component_box_index_safe(box, i);
            ++m_report.components;
            Extracted e = m_extractor.extract(cpt);
            if (!e.record) {
                skip(e.skipReason, str(as_component_get_id(cpt)));
                continue;
            }
            const std::string id = e.record->componentId;
            if (auto ok = m_writer.addComponent(m_config.source.id(), std::move(*e.record)); !ok)
                skip(std::string(toString(ok.error().code())), id + " (" + ok.error().message() + ")");
            else
                ++m_report.imported;
        }
    }

    void fail(std::string error)
    {
        m_report.readable = false;
        m_report.error = std::move(error);
    }

    SourceReport finish(double seconds)
    {
        m_report.seconds = seconds;
        return std::move(m_report);
    }

private:
    CatalogWriter& m_writer;
    const SourceConfig& m_config;
    Extractor m_extractor;
    SourceReport m_report;
};

GObjectPtr<AsMetadata> newMetadata()
{
    GObjectPtr<AsMetadata> metadata(as_metadata_new());
    as_metadata_set_format_style(metadata.p, AS_FORMAT_STYLE_CATALOG);
    as_metadata_set_locale(metadata.p, "ALL");
    return metadata;
}

void parseWhole(const SourceConfig& config, SourceImport& import)
{
    auto metadata = newMetadata();
    GObjectPtr<GFile> file(g_file_new_for_path(config.file.c_str()));
    GError* error = nullptr;
    if (!as_metadata_parse_file(metadata.p, file.p, isYaml(config.file) ? AS_FORMAT_KIND_YAML : AS_FORMAT_KIND_XML,
                                &error)) {
        import.fail(error ? error->message : "cannot parse");
        g_clear_error(&error);
        return;
    }
    import.take(as_metadata_get_components(metadata.p));
}

std::string escapeAttribute(std::string_view value)
{
    std::string out;
    for (const char c : value) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// Streams an AppStream XML catalogue: each <component> is serialised by
// libxml2's reader and parsed by AsMetadata in batches, under a copy of the
// root element and its attributes (origin, media base URL...). Only one
// batch is in memory at a time.
void parseXmlInBatches(const SourceConfig& config, std::size_t batchSize, SourceImport& import)
{
    const std::unique_ptr<xmlTextReader, decltype(&xmlFreeTextReader)> reader(
            xmlReaderForFile(config.file.c_str(), nullptr, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
            &xmlFreeTextReader);
    if (!reader) {
        import.fail("cannot open " + config.file.string());
        return;
    }
    std::string root;
    std::string batch;
    std::size_t inBatch = 0;
    auto metadata = newMetadata();
    bool failed = false;
    auto flush = [&] {
        if (inBatch == 0)
            return;
        const std::string document = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + root + batch + "</components>\n";
        GError* error = nullptr;
        if (!as_metadata_parse_data(metadata.p, document.c_str(), static_cast<gssize>(document.size()),
                                    AS_FORMAT_KIND_XML, &error)) {
            import.fail(error ? error->message : "cannot parse a batch");
            g_clear_error(&error);
            failed = true;
        } else {
            import.take(as_metadata_get_components(metadata.p));
        }
        as_metadata_clear_components(metadata.p);
        batch.clear();
        inBatch = 0;
    };

    int rc = xmlTextReaderRead(reader.get());
    while (rc == 1 && !failed) {
        const int type = xmlTextReaderNodeType(reader.get());
        const int depth = xmlTextReaderDepth(reader.get());
        const auto* name = reinterpret_cast<const char*>(xmlTextReaderConstLocalName(reader.get()));
        if (type == XML_READER_TYPE_ELEMENT && depth == 0) {
            if (!name || std::string_view(name) != "components") {
                import.fail("not an AppStream catalogue (root element)");
                return;
            }
            root = "<components";
            while (xmlTextReaderMoveToNextAttribute(reader.get()) == 1) {
                root += ' ';
                root += reinterpret_cast<const char*>(xmlTextReaderConstName(reader.get()));
                root += "=\"";
                root += escapeAttribute(reinterpret_cast<const char*>(xmlTextReaderConstValue(reader.get())));
                root += '"';
            }
            root += ">\n";
            xmlTextReaderMoveToElement(reader.get());
            rc = xmlTextReaderRead(reader.get());
        } else if (type == XML_READER_TYPE_ELEMENT && depth == 1 && name && std::string_view(name) == "component") {
            xmlChar* outer = xmlTextReaderReadOuterXml(reader.get());
            if (outer) {
                batch += reinterpret_cast<const char*>(outer);
                batch += '\n';
                xmlFree(outer);
                if (++inBatch >= batchSize)
                    flush();
            }
            rc = xmlTextReaderNext(reader.get());
        } else {
            rc = xmlTextReaderRead(reader.get());
        }
    }
    if (rc < 0 && !failed) {
        // Truncated or malformed file: what was read is kept and reported;
        // the replacement checks decide whether the candidate is plausible.
        import.fail("XML error in " + config.file.string());
        failed = true;
    }
    if (!failed)
        flush();
}

// Streams a DEP-11 file: a header document, then one document per
// component, each introduced by a `---` line. Top-level keys sit in column
// 0 and block scalars are indented, so a `---` line in column 0 is always a
// document separator. Each batch is the header plus N components.
void parseYamlInBatches(const SourceConfig& config, std::size_t batchSize, SourceImport& import)
{
    GObjectPtr<GFile> file(g_file_new_for_path(config.file.c_str()));
    GError* error = nullptr;
    GObjectPtr<GInputStream> raw(G_INPUT_STREAM(g_file_read(file.p, nullptr, &error)));
    if (!raw.p) {
        import.fail(error ? error->message : "cannot open " + config.file.string());
        g_clear_error(&error);
        return;
    }
    GInputStream* source = raw.p;
    GObjectPtr<GZlibDecompressor> gunzip(nullptr);
    GObjectPtr<GInputStream> converted(nullptr);
    if (config.file.extension() == ".gz") {
        gunzip.p = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);
        converted.p = g_converter_input_stream_new(raw.p, G_CONVERTER(gunzip.p));
        source = converted.p;
    }
    GObjectPtr<GDataInputStream> lines(g_data_input_stream_new(source));

    auto metadata = newMetadata();
    std::string header;
    std::string batch;
    std::string document;
    std::size_t inBatch = 0;
    int documents = 0;
    bool failed = false;
    auto flush = [&] {
        if (inBatch == 0 || failed)
            return;
        const std::string data = header + batch;
        GError* parseError = nullptr;
        if (!as_metadata_parse_data(metadata.p, data.c_str(), static_cast<gssize>(data.size()), AS_FORMAT_KIND_YAML,
                                    &parseError)) {
            import.fail(parseError ? parseError->message : "cannot parse a batch");
            g_clear_error(&parseError);
            failed = true;
        } else {
            import.take(as_metadata_get_components(metadata.p));
        }
        as_metadata_clear_components(metadata.p);
        batch.clear();
        inBatch = 0;
    };
    auto endDocument = [&] {
        if (documents == 1)
            header = "---\n" + document;
        else if (documents > 1) {
            batch += "---\n";
            batch += document;
            if (++inBatch >= batchSize)
                flush();
        }
        document.clear();
    };
    for (;;) {
        gsize length = 0;
        char* line = g_data_input_stream_read_line_utf8(lines.p, &length, nullptr, &error);
        if (!line)
            break;
        if (std::string_view(line, length) == "---") {
            endDocument();
            ++documents;
        } else if (documents > 0) {
            document.append(line, length);
            document += '\n';
        }
        g_free(line);
        if (failed)
            break;
    }
    if (error) {
        // Truncated archive or invalid UTF-8: what was read is kept and
        // reported; the replacement checks judge the candidate.
        import.fail(std::string("read error: ") + error->message);
        g_clear_error(&error);
        return;
    }
    endDocument();
    flush();
}

} // namespace

long peakRssKiB()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss;
}

std::vector<SourceReport> importSources(CatalogWriter& writer, const std::vector<SourceConfig>& sources,
                                        const ImportOptions& options)
{
    std::vector<SourceReport> reports;
    for (const SourceConfig& config : sources) {
        const auto start = std::chrono::steady_clock::now();
        SourceImport import(writer, config, options);
        std::error_code ec;
        if (auto ok = writer.addSource(config.source); !ok) {
            import.fail(ok.error().message());
        } else if (!std::filesystem::is_regular_file(config.file, ec)) {
            import.fail("no such file " + config.file.string());
        } else if (options.batchSize > 0 && isYaml(config.file)) {
            parseYamlInBatches(config, options.batchSize, import);
        } else if (options.batchSize > 0) {
            parseXmlInBatches(config, options.batchSize, import);
        } else {
            parseWhole(config, import);
        }
        reports.push_back(import.finish(
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()));
    }
    return reports;
}

} // namespace cutefish::store::indexer
