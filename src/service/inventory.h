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
// of state.sqlite run on a worker thread of their own (a connection of
// their own, SQLite's busy timeout between it and the main one).

#include <cutefish/store/backend.h>
#include <cutefish/store/result.h>

#include <base/event_loop.h>
#include <base/thread_pool.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutefish::store::service {

class Inventory
{
public:
    Inventory(Backend& backend, std::filesystem::path catalog, std::filesystem::path state);
    ~Inventory();

    Inventory(const Inventory&) = delete;
    Inventory& operator=(const Inventory&) = delete;

    /// On the main loop, after the change is visible through the getters.
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
    /// The updates of applications, D-010; empty until known.
    const std::vector<AvailableUpdate>& updates() const noexcept { return m_updates; }

private:
    struct Run
    {
        bool running = false;
        bool again = false;
    };
    struct Disk;

    void setAvailable(bool available, std::string reason);
    void reconcileInstalled();
    void reconcileUpdates();
    void installedDone();
    void updatesDone();

    Backend& m_backend;
    std::filesystem::path m_catalogPath;
    std::filesystem::path m_statePath;
    std::uint64_t m_generation = 0;
    std::optional<bool> m_available;
    std::string m_reason;
    bool m_checking = false;
    std::vector<AvailableUpdate> m_updates;
    Run m_installedRun;
    Run m_updatesRun;
    /// Owned by the worker thread once created there.
    std::shared_ptr<Disk> m_disk;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    base::ThreadPool m_io{1};
    base::Job m_installedJob;
    base::Job m_updatesJob;
};

} // namespace cutefish::store::service
