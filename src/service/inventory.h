#pragma once

// What one backend sees of the system, kept by the service (SERVICE.md
// §6.2, step F, read-only integration): whether the backend is usable, the
// installed inventory written to state.sqlite, and the available updates of
// applications (D-010). Generic: it only uses the Backend contract
// (backend.h) and the core's linking functions (applicationlinks.h); it
// knows nothing of PackageKit.
//
// Reconciliations run when the backend becomes usable, when it reports a
// change made outside the store, and when a new catalogue is in service
// (the links change). Two requests during one give one more run, not two.
// The backend answers on the main loop; the catalogue read and the write
// of state.sqlite run on the I/O thread every inventory of the service
// shares (inventoryio.h, H.2).
//
// What is recorded and listed is normalised first (H.2): an item or an
// update whose offer belongs to another backend is left out, and an offer
// listed twice is kept once, so that one bad entry never makes the whole
// inventory fail to be recorded, and nothing is counted twice.

#include "inventoryio.h"

#include <cutefish/store/backend.h>
#include <cutefish/store/result.h>

#include <base/event_loop.h>
#include <base/timer.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cutefish::store::service {

/// The installed items of `kind` as recorded (H.2): items of another
/// backend are left out; an offer listed twice is kept once (the first by
/// its contents, so the choice does not depend on the backend's order).
/// Sorted by offer id.
std::vector<InstalledItem> normalizeInstalled(BackendKind kind, std::vector<InstalledItem> items);

/// The updates of `kind` as listed (H.2): an update whose offer is not an
/// offer of `kind` is left out; an offer listed twice is one update (the
/// first by its contents). Sorted by offer id: one entry per offer.
std::vector<AvailableUpdate> normalizeUpdates(BackendKind kind, std::vector<AvailableUpdate> updates);

/// What one recorded inventory changed (internal to the inventory).
struct InstalledOutcome
{
    bool changed = false;
    std::vector<std::string> apps;
};

class Inventory
{
public:
    /// `io` outlives the inventory.
    Inventory(Backend& backend, std::filesystem::path catalog, InventoryIo& io);
    ~Inventory();

    Inventory(const Inventory&) = delete;
    Inventory& operator=(const Inventory&) = delete;

    /// On the main loop, after the change is visible through the getters:
    /// available(), reason() or recordError().
    std::function<void()> onAvailabilityChanged;
    /// The applications whose installed state changed (added, removed,
    /// another version, another link); may be empty when only packages
    /// without an application changed.
    std::function<void(const std::vector<std::string>& appIds)> onInstalledChanged;
    std::function<void()> onUpdatesChanged;

    /// Asks the backend whether it is usable; when it is, reconciles the
    /// installed state and the updates. At startup, and at each Refresh.
    void check();
    /// A catalogue generation in service (0: none): links again.
    void setGeneration(std::uint64_t generation);

    Backend& backend() noexcept { return m_backend; }
    /// Unknown until the backend has answered once.
    std::optional<bool> available() const noexcept { return m_available; }
    /// Why the backend is not usable (empty when it is).
    const std::string& reason() const noexcept { return m_reason; }
    /// The updates of applications, D-010, normalised (normalizeUpdates);
    /// empty until known and while the backend is not usable.
    const std::vector<AvailableUpdate>& updates() const noexcept { return m_updates; }
    /// Why the last inventory read from the backend could not be recorded
    /// in state.sqlite, once every attempt failed (a permanent error, or a
    /// lock held past every new attempt); empty when the last one was
    /// recorded, or while attempts go on. The recorded state is then older
    /// than the backend's (SERVICE.md §21.5).
    const std::string& recordError() const noexcept { return m_recordError; }

private:
    struct Run
    {
        bool running = false;
        bool again = false;
    };
    void setAvailable(bool available, std::string reason);
    void reconcileInstalled();
    void reconcileUpdates();
    void installedDone();
    void updatesDone();
    void writeInstalled();
    void installedWritten(Result<InstalledOutcome> outcome);
    void retryWrite();
    void setRecordError(std::string error);

    /// An inventory read from the backend, until it is recorded.
    struct Pending
    {
        std::vector<InstalledItem> items;
        std::set<Scope> scopes;
        std::uint64_t generation = 0;
        int attempts = 0;
    };

    Backend& m_backend;
    std::filesystem::path m_catalogPath;
    InventoryIo& m_io;
    std::uint64_t m_generation = 0;
    std::optional<bool> m_available;
    std::string m_reason;
    bool m_checking = false;
    std::vector<AvailableUpdate> m_updates;
    Run m_installedRun;
    Run m_updatesRun;
    std::optional<Pending> m_pending;
    std::string m_recordError;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    // Destroyed first: no new attempt, and a result still on its way is
    // dropped.
    base::Timer m_retry;
    IoJob m_installedJob;
    IoJob m_updatesJob;
};

} // namespace cutefish::store::service
