#pragma once

// The transaction object of a Refresh (ARCHITECTURE §5.3, owner's decision
// for service v0.1): org.cutefish.Store1.Transaction with kind `refresh`.
// It is not written to the journal, whose transactions are install, remove
// and update (D-018). States: running, then succeeded, failed or cancelled.

#include <cutefish/store/error.h>
#include <cutefish/store/statestore.h>
#include <cutefish/store/transaction.h>

#include <base/dbus.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace cutefish::store::service {

class RefreshTransaction
{
public:
    /// `cancel` returns false when the refresh can no longer be cancelled.
    RefreshTransaction(base::dbus::Connection* connection, std::string path, std::string id, UnixSeconds created,
                       std::function<bool()> cancel, std::function<bool()> cancellable);

    const std::string& path() const noexcept { return m_object.path(); }
    TransactionState state() const noexcept { return m_state; }
    bool finished() const noexcept { return isTerminal(m_state); }

    /// Moves to a terminal state, emits PropertiesChanged and Finished.
    void finish(TransactionState state, std::optional<Error> error, UnixSeconds now);
    /// Cancellable may have changed.
    void updateCancellable();

private:
    base::dbus::Object m_object;
    std::string m_id;
    UnixSeconds m_created;
    UnixSeconds m_finished = 0;
    TransactionState m_state = TransactionState::Running;
    std::optional<Error> m_error;
    std::function<bool()> m_cancel;
    std::function<bool()> m_cancellable;
};

} // namespace cutefish::store::service
