#pragma once

// The transaction manager (step F2, SERVICE.md §5): Install, Remove, Update
// and UpdateAll as transactions driven on the main loop. Generic: it uses
// the Backend contract (backend.h), the core's Transaction state machine
// (transaction.h) and the journal of state.sqlite (statestore.h); it knows
// nothing of PackageKit or D-Bus details beyond publishing its objects.
//
// One lane per backend (P-009, D-030): at most one transaction plans or
// executes on a backend at a time, in order of arrival; a transaction
// awaiting confirmation gives the lane up, and once confirmed waits its
// turn again, staying in planning. Exclusive work of the host (a repository
// refresh) takes its turn in the same lane.
//
// Flow of a transaction (P-008, D-017): queued → planning (Backend::plan)
// → awaiting-confirmation when the policy asks (Confirm, or cancelled after
// 10 minutes, D-019) → Backend::execute, which simulates again and checks
// the accepted plan before any change: `started` → verifyExecutionPlan →
// running; PlanChanged → awaiting-confirmation with plan-changed, nothing
// executed. Every change is written to the journal (D-017, D-018).

#include "actiontransaction.h"

#include <cutefish/store/backend.h>
#include <cutefish/store/result.h>
#include <cutefish/store/statestore.h>
#include <cutefish/store/transaction.h>

#include <base/dbus.h>
#include <base/event_loop.h>
#include <base/timer.h>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutefish::store::service {

class TransactionManager
{
public:
    struct Options
    {
        /// D-019.
        std::chrono::milliseconds confirmationTimeout = std::chrono::minutes(10);
        /// How long a finished transaction stays on the bus (A §5.3).
        std::chrono::milliseconds retention = std::chrono::minutes(5);
    };

    /// What a transaction acts on: an offer, and its application if known.
    using Target = TransactionOffer;

    TransactionManager(base::EventLoop& loop, base::dbus::Connection& connection, StateStore& journal,
                       Options options);
    ~TransactionManager();

    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    /// A backend the manager may use; one per kind.
    void addBackend(Backend& backend);

    // --- Hooks, on the main loop -------------------------------------------
    /// A new object path for a transaction.
    std::function<std::string()> newPath;
    std::function<void(const std::string& path)> onAdded;
    std::function<void(const std::string& path)> onRemoved;
    /// A transaction that may have changed the system ended: the host
    /// reconciles the installed state (SERVICE.md §8.3).
    std::function<void(BackendKind)> onSystemMayHaveChanged;
    /// Keep the service alive while a transaction is not finished (P-010).
    std::function<void()> onActivityBegin;
    std::function<void()> onActivityEnd;

    // --- Requests -----------------------------------------------------------
    /// Creates and queues a transaction; returns its object path. The caller
    /// has checked the targets against the catalogue or the installed state.
    /// InvalidArgument (a request PlanRequest refuses), Backend/Unavailable
    /// (no such backend), Conflict (an offer already engaged: Busy),
    /// Internal (the journal cannot be written).
    Result<std::string> start(TransactionKind kind, std::vector<Target> targets);
    /// UpdateAll (D-031): a parent `update-all` transaction, and one child
    /// `update` transaction per backend and installation. Conflict when
    /// one of the offers is already engaged; nothing is created then.
    Result<std::string> startUpdateAll(std::vector<Target> updates);
    /// Recovery at startup (SERVICE.md §5.6): the running transactions the
    /// journal kept with a backend handle are re-attached, or interrupted
    /// when the backend no longer knows them.
    void recover(std::vector<TransactionRecord> toReattach);
    /// Runs `work` alone on the backend's lane, after what is in progress;
    /// `work` calls `done` once when it has finished.
    void exclusive(BackendKind backend, std::function<void(std::function<void()> done)> work);

    std::vector<std::string> paths() const;
    /// An offer is engaged by a transaction that is not finished.
    bool engaged(const OfferId& offer) const;

private:
    struct Entry;
    struct Parent;
    struct Job
    {
        std::string entry; ///< empty: exclusive work
        std::function<void(std::function<void()>)> work = {};
    };
    struct Lane
    {
        Backend* backend = nullptr;
        /// An entry id, or kExclusive while exclusive work runs.
        std::string owner;
        std::deque<Job> waiting;
    };

    Result<std::unique_ptr<Entry>> make(TransactionKind kind, std::vector<Target> targets, std::string parent);
    void publish(Entry& e);
    void pump(BackendKind backend);
    void release(Entry& e);
    void plan(Entry& e);
    void execute(Entry& e);
    void awaitConfirmation(Entry& e);
    void stateChanged(Entry& e);
    /// Ends the transaction with this error: cancelled for Cancelled,
    /// interrupted for a lost backend operation, failed otherwise.
    void endWith(Entry& e, const Error& error);
    void finish(Entry& e);
    void save(const Entry& e);
    std::string confirm(const std::string& id);
    std::string cancel(const std::string& id);
    std::string operation(Entry& e, const char* what);
    void retire(const std::string& path, std::function<void()> erase);
    void updateParent(const std::string& parentId);
    Entry* find(const std::string& id);
    UnixSeconds now() const;

    base::EventLoop& m_loop;
    base::dbus::Connection& m_connection;
    StateStore& m_journal;
    Options m_options;
    std::map<BackendKind, Lane> m_lanes;
    std::map<std::string, std::unique_ptr<Entry>> m_entries; // by id
    std::map<std::string, std::unique_ptr<Parent>> m_parents; // by id
    std::map<std::string, std::unique_ptr<base::Timer>> m_retirements; // by path
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace cutefish::store::service
