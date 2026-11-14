#pragma once

// The transactions that change a source (H.5, SERVICE.md §25): kinds
// `enable-source`, `disable-source` and `remove-source`, published as
// org.cutefish.Store1.Transaction objects (ActionTransaction) like the
// others, but not written to the journal (D-043). Generic: it uses the
// Backend contract only.
//
// Flow: enabling waits for Confirm first (D-042), with the delay of D-019
// (awaiting-confirmation → cancelled when it expires); then, like disabling
// and removing, queued on the backend's lane (the same as its transactions
// and refreshes, P-009, D-034) → running → succeeded or failed. Cancel is
// immediate before running; a running change is not cancellable. A request
// the host refuses as untrusted (D-044) gets its object, then fails at
// once, without asking anything.

#include "actiontransaction.h"

#include <cutefish/store/backend.h>
#include <cutefish/store/result.h>
#include <cutefish/store/statestore.h>

#include <base/dbus.h>
#include <base/event_loop.h>
#include <base/timer.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutefish::store::service {

class SourceChanges
{
public:
    struct Options
    {
        /// D-019.
        std::chrono::milliseconds confirmationTimeout = std::chrono::minutes(10);
        /// How long a finished transaction stays on the bus (A §5.3).
        std::chrono::milliseconds retention = std::chrono::minutes(5);
    };

    SourceChanges(base::EventLoop& loop, base::dbus::Connection& connection, Options options);
    ~SourceChanges();

    SourceChanges(const SourceChanges&) = delete;
    SourceChanges& operator=(const SourceChanges&) = delete;

    // --- Hooks, on the main loop -------------------------------------------
    std::function<std::string()> newPath;
    std::function<void(const std::string& path)> onAdded;
    std::function<void(const std::string& path)> onRemoved;
    /// Runs `work` alone on the backend's lane (TransactionManager::exclusive);
    /// `work` calls `done` once it has finished.
    std::function<void(BackendKind, std::function<void(std::function<void()>)>)> exclusive;
    /// The backend ran a change, whatever its outcome: the host reads the
    /// sources again and reindexes the local metadata.
    std::function<void(BackendKind)> onSourcesMayHaveChanged;
    /// Keep the service alive while a change is not finished (P-010).
    std::function<void()> onActivityBegin;
    std::function<void()> onActivityEnd;

    /// Creates the transaction and returns its path. `source` is the
    /// source as the backend last listed it (shown in the plan); the caller
    /// checked the request (sourceRequestRefusal). `refusal`: the
    /// transaction fails with it at once (D-044).
    Result<std::string> start(Backend& backend, const BackendSource& source, SourceRequest::Action action,
                              std::optional<Error> refusal = std::nullopt);

    std::vector<std::string> paths() const;
    /// A change of this source is not finished.
    bool engaged(const SourceId& source) const;

private:
    struct Entry;
    void enqueue(Entry& e);
    void run(const std::string& id, std::function<void()> laneDone);
    void finish(Entry& e, TransactionState state, std::optional<Error> error);
    std::string confirm(const std::string& id);
    std::string cancel(const std::string& id);
    Entry* find(const std::string& id);
    UnixSeconds now() const;

    base::EventLoop& m_loop;
    base::dbus::Connection& m_connection;
    Options m_options;
    std::map<std::string, std::unique_ptr<Entry>> m_entries; // by id
    std::map<std::string, std::unique_ptr<base::Timer>> m_retirements; // by id
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace cutefish::store::service
