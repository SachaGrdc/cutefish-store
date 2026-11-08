#pragma once

// cutefish-store-service v0.1 (step E): the object /org/cutefish/Store,
// interface org.cutefish.Store1, read-only towards the package managers.
// Exported: the reading methods of SERVICE.md §4.3 that work without the
// backends, Refresh (§3.5, rereading the local metadata), the properties
// of §4.2 except UpdateCount, and the signals of §4.7 except the update and
// image ones. Install, Remove, Update, UpdateAll, EnableSource,
// DisableSource, GetUpdates and GetPermissions come with the backends
// (owner's decision: no method that cannot work).

#include "lifecycle.h"
#include "options.h"
#include "readpool.h"
#include "refresher.h"
#include "refreshtransaction.h"

#include <cutefish/store/result.h>
#include <cutefish/store/statestore.h>

#include <base/dbus.h>
#include <base/event_loop.h>
#include <base/timer.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace cutefish::store::service {

class StoreService
{
public:
    StoreService(base::EventLoop& loop, base::dbus::Connection& connection, ServiceOptions options);
    ~StoreService();

    /// Opens state.sqlite (recovery of the journal), publishes the object,
    /// then takes the bus name without queueing (single instance, A §9.6).
    /// Conflict when another instance owns the name.
    Result<void> start();

    /// Called once the service is idle (P-010): the name is released.
    std::function<void()> onIdle;

private:
    void exportObject();
    void setState(std::string state);
    void loadGeneration();
    void startRefresh(bool force, RefreshTransaction* transaction);
    void refreshDone(Result<RefreshOutcome> result, RefreshTransaction* transaction);
    std::string newTransaction();
    void retire(const std::string& path);
    UnixSeconds now() const;
    /// Common prologue of a method: note the caller, and refuse when no
    /// catalogue is in service. True when the call may go on.
    bool admit(base::dbus::MethodCall& call, bool needsCatalog);

    base::EventLoop& m_loop;
    base::dbus::Connection& m_connection;
    ServiceOptions m_options;
    std::optional<StateStore> m_state;
    ReadPool m_reads;
    Refresher m_refresher;
    Lifecycle m_lifecycle;
    std::unique_ptr<base::dbus::Object> m_object;

    std::string m_stateName = "starting";
    std::uint64_t m_generation = 0;
    std::optional<UnixSeconds> m_lastRefresh;
    bool m_refreshing = false;
    bool m_initialImport = false;

    std::uint64_t m_nextTransaction = 1;
    std::map<std::string, std::unique_ptr<RefreshTransaction>> m_transactions; // by path
    std::map<std::string, std::unique_ptr<base::Timer>> m_retirements;
    std::string m_currentRefresh; // path of the running refresh's transaction, if any
};

} // namespace cutefish::store::service
