#pragma once

// cutefish-store-service (steps E and F): the object /org/cutefish/Store,
// interface org.cutefish.Store1, read-only towards the package managers.
// Exported: the reading methods of SERVICE.md §4.3, Refresh (§3.5), the
// properties of §4.2 and the signals of §4.7 except the image ones.
// GetUpdates, UpdateCount and UpdatesChanged exist only with a backend.
// Install, Remove, Update, UpdateAll, EnableSource, DisableSource and
// GetPermissions are not exported yet (D-022: no method that cannot work;
// the transaction manager is a later step).
//
// The service holds a Backend (backend.h) without knowing which one: main
// creates it. Its installed state and updates are kept by an Inventory.

#include "inventory.h"
#include "lifecycle.h"
#include "options.h"
#include "readpool.h"
#include "refresher.h"
#include "refreshtransaction.h"

#include <cutefish/store/backend.h>
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
    /// `backend` may be null: the service then only reads the catalogue.
    StoreService(base::EventLoop& loop, base::dbus::Connection& connection, ServiceOptions options,
                 std::unique_ptr<Backend> backend = nullptr);
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
    /// The refresh's second half: the backend's catalogue sources, then
    /// the reindexing.
    void indexSources(bool force, RefreshTransaction* transaction);
    bool cancelRefresh();
    bool refreshCancellable() const;
    void setGeneration(std::uint64_t generation);
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
    // Declared after the object and the backend: destroyed before them,
    // since it uses both.
    std::unique_ptr<Backend> m_backend;
    std::unique_ptr<Inventory> m_inventory;

    std::string m_stateName = "starting";
    std::uint64_t m_generation = 0;
    std::optional<UnixSeconds> m_lastRefresh;
    bool m_refreshing = false;
    bool m_initialImport = false;

    std::uint64_t m_nextTransaction = 1;
    std::map<std::string, std::unique_ptr<RefreshTransaction>> m_transactions; // by path
    std::map<std::string, std::unique_ptr<base::Timer>> m_retirements;
    std::string m_currentRefresh; // path of the running refresh's transaction, if any
    /// Where the running refresh is (cancellation goes to the right part).
    enum class RefreshPhase { None, Backend, Sources, Index };
    RefreshPhase m_refreshPhase = RefreshPhase::None;
    std::string m_refreshOperation; // the backend's operation id
    bool m_refreshBackendCancellable = false;
    bool m_refreshCancelled = false;
};

} // namespace cutefish::store::service
