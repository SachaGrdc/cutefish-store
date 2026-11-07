#pragma once

#include <cutefish/store/catalog.h>
#include <cutefish/store/result.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cutefish::store {

struct CatalogReadOptions
{
    /// Preferred languages, most preferred first (`fr_FR.UTF-8`, `de`).
    /// Each brings its fallbacks (`fr_FR` then `fr`); the untranslated `C`
    /// text always comes last.
    std::vector<std::string> locales = {};
    /// state.sqlite (StateStore), attached read-only to give `installed`
    /// and the `installedOnly` filter (ARCHITECTURE §6.3, SERVICE.md §8.4).
    /// Empty: no installed state; `installedOnly` is then refused. The file
    /// must exist and have the current state schema; it is never modified.
    std::filesystem::path statePath = {};
};

/// The catalogue snapshot (catalog.sqlite), read-only (ARCHITECTURE §6.3).
///
/// The file is opened read-only with SQLite's `immutable` flag: the
/// snapshot is never modified in place, only replaced as a whole by
/// replaceCatalog(). An open SqliteCatalog keeps reading the snapshot it
/// opened, even after a replacement; open a new one to see the new
/// generation (docs/CATALOG.md, "Reconstruction atomique").
///
/// Displayed texts (editorial priority): for each field, the text of the
/// offer whose source has the highest priority among those that provide it
/// in the best available language. Offers come in the same order. This
/// order is not the installation priority: the offer proposed by default is
/// chosen by defaultOffer() (defaultoffer.h, D-009), never by position.
///
/// Installed state (with CatalogReadOptions::statePath): an application is
/// shown installed when state.sqlite lists one of its offers, or, for an
/// installed offer the catalogue does not know, when the inventory links it
/// to the application (its id or an alias). The catalogue's offer wins over
/// the inventory's link when both exist. This is a display cache of the
/// last inventory, never a reason to install or remove anything: the
/// backends decide (SERVICE.md §8). Each call reads one committed state of
/// state.sqlite: one SQLite read transaction per call.
///
/// One instance per thread; it keeps prepared statements and the result
/// list of the last search, so that turning pages does not search again.
class SqliteCatalog final : public Catalog
{
public:
    static Result<SqliteCatalog> open(const std::filesystem::path& path, CatalogReadOptions options = {});

    SqliteCatalog(SqliteCatalog&&) noexcept;
    SqliteCatalog& operator=(SqliteCatalog&&) noexcept;
    ~SqliteCatalog() override;

    std::uint64_t generation() const noexcept override;
    Result<Application> application(const AppId& id) const override;
    /// Ranking in tiers, documented in docs/CATALOG.md: exact name, name
    /// prefix, words (FTS5, bm25), substring of the name (trigrams), and only
    /// when those find nothing, approximate names (trigrams and edit
    /// distance ≤ 2). With state.sqlite attached, `installed` is set and
    /// `installedOnly` filters; without it, `installedOnly` is refused with
    /// NotSupported. `hasUpdate` stays false: there is no update table yet.
    Result<SearchPage> search(const SearchQuery& query) const override;
    Result<std::vector<AppSummary>> summaries(std::span<const AppId> ids) const override;
    /// The sorted list of matching rows is kept, as for a search, so that
    /// turning pages does not sort again; only row numbers are kept, never
    /// the applications themselves.
    Result<SearchPage> browse(const BrowseQuery& query) const override;
    Result<std::vector<CategoryCount>> listCategories() const override;
    Result<std::vector<RecordedSource>> sources() const override;

private:
    struct Impl;
    explicit SqliteCatalog(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};

} // namespace cutefish::store
