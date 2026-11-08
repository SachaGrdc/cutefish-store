#pragma once

// The D-Bus object of an action transaction (ARCHITECTURE §5.3, SERVICE.md
// §4.5): org.cutefish.Store1.Transaction for install, remove and update,
// and for the update-all parent (D-031). It only publishes what the
// transaction manager tells it; Confirm and Cancel are handed back to the
// manager, which decides.
//
// Progress, Phase and the byte counts are grouped: at most 10
// PropertiesChanged per second (A §5.3). State changes go out at once,
// after any progress still waiting.

#include <cutefish/store/backend.h>
#include <cutefish/store/error.h>
#include <cutefish/store/statestore.h>
#include <cutefish/store/transaction.h>

#include <base/dbus.h>
#include <base/event_loop.h>
#include <base/timer.h>

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cutefish::store::service {

class ActionTransaction
{
public:
    struct Identity
    {
        std::string path;
        std::string id;
        /// install, remove, update, update-all.
        std::string kind;
        /// packagekit, flatpak, or multiple (update-all).
        std::string backend;
        std::vector<std::string> appIds;
        std::vector<std::string> offerIds;
        /// The update-all parent; empty for none (published as "/").
        std::string parent = {};
        UnixSeconds created = 0;
    };

    /// `confirm` and `cancel` return the D-Bus error name to answer with, or
    /// an empty string when the request was accepted.
    ActionTransaction(base::EventLoop& loop, base::dbus::Connection* connection, Identity identity,
                      std::function<std::string()> confirm, std::function<std::string()> cancel);

    const std::string& path() const noexcept { return m_identity.path; }
    const std::string& id() const noexcept { return m_identity.id; }
    TransactionState state() const noexcept { return m_state; }
    bool finished() const noexcept { return isTerminal(m_state); }

    void setState(TransactionState state);
    /// The plan to show (`Plan` property), with the reasons of the pending
    /// confirmation; empty map for none.
    void setPlan(base::dbus::VariantMap plan);
    void setDeadline(UnixSeconds deadline); ///< 0: none
    void setCancellable(bool cancellable);
    void setProgress(const Progress& progress);
    /// Moves to a terminal state: PropertiesChanged, then Finished once.
    void finish(TransactionState state, std::optional<Error> error, UnixSeconds now);

private:
    void changed(std::vector<std::string> names);
    void flush();

    base::dbus::Object m_object;
    Identity m_identity;
    std::function<std::string()> m_confirm;
    std::function<std::string()> m_cancel;
    TransactionState m_state = TransactionState::Queued;
    base::dbus::VariantMap m_plan;
    std::optional<Error> m_error;
    UnixSeconds m_deadline = 0;
    UnixSeconds m_finished = 0;
    bool m_cancellable = false;
    std::string m_phase;
    double m_progress = -1;
    std::uint64_t m_downloaded = 0;
    std::uint64_t m_total = 0;
    std::set<std::string> m_pending;
    base::Timer m_throttle;
};

} // namespace cutefish::store::service
