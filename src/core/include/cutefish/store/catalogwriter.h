#pragma once

#include <cutefish/store/application.h>
#include <cutefish/store/identifiers.h>
#include <cutefish/store/offer.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cutefish::store {

/// Version of the catalogue schema, also written as SQLite `user_version`
/// once a catalogue is complete (docs/CATALOG.md).
inline constexpr std::int64_t kCatalogSchemaVersion = 1;

/// A text in one language. `C` is the untranslated text.
struct LocalizedText
{
    std::string locale;
    std::string text;
};

struct LocalizedDescription
{
    std::string locale;
    std::vector<DescriptionBlock> blocks;
};

struct LocalizedKeywords
{
    std::string locale;
    std::vector<std::string> keywords;
};

/// One AppStream component read from one source. It becomes one offer of
/// the application named by its normalised id: components are never merged
/// with each other, only grouped under the same application.
struct ComponentRecord
{
    /// Id as written by the source (may end in `.desktop`); kept as the
    /// offer's provenance and normalised with AppId::fromComponentId.
    std::string componentId;
    OfferSpec offer;
    std::vector<LocalizedText> names; ///< Must contain a `C` entry.
    std::vector<LocalizedText> summaries = {};
    std::vector<LocalizedDescription> descriptions = {};
    std::vector<LocalizedKeywords> keywords = {};
    IconRef icon = {};
    std::vector<std::string> categories = {};
    /// `<provides><id>` values, normalised like component ids. Rule D-012:
    /// one becomes an alias only when exactly one application claims it and
    /// no application has that id; it never merges two applications. A
    /// value equal to the component's own id is ignored.
    std::vector<std::string> providedIds = {};
    std::string developer = {};
    bool verified = false;
};

struct CatalogBuildOptions
{
    /// Written in the catalogue; the service gives each snapshot a new one.
    std::uint64_t generation = 1;
    /// Languages whose texts are indexed for search, besides `C`. Each
    /// `xx_YY` also brings `xx`. Every stored language stays readable; this
    /// only affects what the search index contains.
    std::vector<std::string> searchLocales = {};
    /// Description text kept in the search index, per language and offer.
    std::size_t descriptionIndexBytes = 2000;
};

/// What a build kept and dropped. Every dropped item is counted, never
/// silently ignored.
struct CatalogBuildStats
{
    std::int64_t sources = 0;
    std::int64_t applications = 0;
    std::int64_t offers = 0;
    std::int64_t texts = 0;
    std::int64_t aliases = 0;
    /// Translations dropped because invalid (bad UTF-8, too long...).
    std::int64_t droppedTexts = 0;
    /// Provided ids that were not kept as aliases: equal to the id of an
    /// application, or claimed by several applications (D-012).
    std::int64_t ambiguousAliases = 0;
    /// Provided ids that are not valid application ids.
    std::int64_t invalidAliases = 0;
    std::int64_t droppedCategories = 0;
    std::int64_t droppedIcons = 0;
};

/// Writes a candidate catalogue file from scratch (ARCHITECTURE §6.2). The
/// file only becomes a valid catalogue when finish() succeeds: until then
/// its `user_version` is 0 and validation refuses it. The writer never
/// touches the active catalogue; replaceCatalog() does (catalogfile.h).
class CatalogWriter
{
public:
    /// Creates `candidate`, replacing any previous candidate file.
    static Result<CatalogWriter> create(const std::filesystem::path& candidate, CatalogBuildOptions options = {});

    CatalogWriter(CatalogWriter&&) noexcept;
    CatalogWriter& operator=(CatalogWriter&&) noexcept;
    ~CatalogWriter();

    /// Sources come first; each component names its source.
    Result<void> addSource(const Source& source);

    /// Adds one component as one offer. Errors (the component is skipped,
    /// the build goes on): InvalidArgument for an invalid id, name or
    /// offer, NotFound for an unknown source, Conflict for an offer the
    /// application already has. Invalid translations, categories or icons
    /// are dropped and counted, without rejecting the component.
    Result<void> addComponent(const SourceId& source, ComponentRecord record);

    /// Builds the search index, writes the metadata, commits and closes.
    /// The schema version is written last, in its own transaction.
    Result<CatalogBuildStats> finish();

    const CatalogBuildStats& stats() const noexcept;

private:
    struct Impl;
    explicit CatalogWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};

} // namespace cutefish::store
