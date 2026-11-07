#pragma once

#include <cutefish/store/application.h>
#include <cutefish/store/identifiers.h>
#include <cutefish/store/result.h>
#include <cutefish/store/search.h>
#include <cutefish/store/sourcefingerprint.h>

#include <cstdint>
#include <span>
#include <vector>

namespace cutefish::store {

/// Read access to the catalogue: applications, their offers, search.
///
/// The production implementation will read the catalogue snapshot
/// (catalog.sqlite, ARCHITECTURE §6.3); the interface lets the service and
/// the tests use another one. Calls are synchronous and may take a few
/// milliseconds: the host runs them on its reader threads, never on its main
/// thread. An instance is used by one thread at a time (one SQLite
/// connection per reader thread, §8.4).
class Catalog
{
public:
    virtual ~Catalog() = default;

    /// Generation of the snapshot being served; 0 when there is none yet.
    /// Every result below comes from this generation.
    virtual std::uint64_t generation() const noexcept = 0;

    /// The application whose id, or one of whose aliases, is `id`.
    /// ErrorCode::NotFound if there is none.
    virtual Result<Application> application(const AppId& id) const = 0;

    /// One page of results for a validated query.
    virtual Result<SearchPage> search(const SearchQuery& query) const = 0;

    /// Summaries for the given ids, in the same order; unknown ids are left
    /// out. The caller bounds the number of ids (200 over D-Bus, §5.2).
    virtual Result<std::vector<AppSummary>> summaries(std::span<const AppId> ids) const = 0;

    /// The sources this snapshot was built from, sorted by id, with the
    /// fingerprint of what was imported and the number of offers.
    virtual Result<std::vector<RecordedSource>> sources() const = 0;

protected:
    Catalog() = default;
    Catalog(const Catalog&) = default;
    Catalog& operator=(const Catalog&) = default;
};

} // namespace cutefish::store
