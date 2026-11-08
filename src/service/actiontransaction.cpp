#include "actiontransaction.h"

#include "marshal.h"

#include <chrono>
#include <cstdint>

namespace cutefish::store::service {

namespace {

/// 10 PropertiesChanged per second at most (ARCHITECTURE §5.3).
constexpr auto kThrottle = std::chrono::milliseconds(100);

} // namespace

ActionTransaction::ActionTransaction(base::EventLoop& loop, base::dbus::Connection* connection, Identity identity,
                                     std::function<std::string()> confirm, std::function<std::string()> cancel)
    : m_object(connection, identity.path, "org.cutefish.Store1.Transaction"), m_identity(std::move(identity)),
      m_confirm(std::move(confirm)), m_cancel(std::move(cancel)), m_throttle(&loop)
{
    using namespace base::dbus;
    m_throttle.setSingleShot(true);
    m_throttle.onTimeout = [this] { flush(); };

    m_object.addProperty("Id", [this] { return m_identity.id; });
    m_object.addProperty("Kind", [this] { return m_identity.kind; });
    m_object.addProperty("Backend", [this] { return m_identity.backend; });
    m_object.addProperty("AppIds", [this] { return m_identity.appIds; });
    m_object.addProperty("OfferIds", [this] { return m_identity.offerIds; });
    m_object.addProperty("Parent", [this] { return ObjectPath{m_identity.parent.empty() ? "/" : m_identity.parent}; });
    m_object.addProperty("State", [this] { return std::string(toString(m_state)); });
    m_object.addProperty("Phase", [this] { return m_state == TransactionState::Running ? m_phase : std::string(); });
    m_object.addProperty("Progress", [this] { return m_progress; });
    m_object.addProperty("DownloadedBytes", [this] { return m_downloaded; });
    m_object.addProperty("TotalBytes", [this] { return m_total; });
    m_object.addProperty("Cancellable", [this] { return !finished() && m_cancellable; });
    m_object.addProperty("Plan", [this] { return m_plan; });
    m_object.addProperty("ConfirmationDeadline", [this] { return static_cast<std::uint64_t>(m_deadline); });
    m_object.addProperty("Error", [this] { return m_error ? errorMap(*m_error) : VariantMap{}; });
    m_object.addProperty("Created", [this] { return static_cast<std::uint64_t>(m_identity.created); });
    m_object.addProperty("Finished", [this] { return static_cast<std::uint64_t>(m_finished); });

    m_object.addMethod("Confirm", "", "", [this](MethodCall& call) {
        const std::string error = m_confirm ? m_confirm() : std::string(errors::kNotAllowed);
        if (!error.empty())
            return call.replyError(error, "the transaction is not awaiting confirmation");
        call.reply();
    });
    m_object.addMethod("Cancel", "", "", [this](MethodCall& call) {
        const std::string error = m_cancel ? m_cancel() : std::string(errors::kNotAllowed);
        if (!error.empty())
            return call.replyError(error, "the transaction cannot be cancelled now");
        call.reply();
    });
    m_object.addSignal("Finished", "sss", {"state", "error_code", "message"});
    m_object.publish();
}

void ActionTransaction::changed(std::vector<std::string> names)
{
    for (std::string& n : names)
        m_pending.insert(std::move(n));
    if (!m_throttle.isActive())
        m_throttle.start(kThrottle);
}

void ActionTransaction::flush()
{
    if (m_pending.empty())
        return;
    std::vector<std::string> names(m_pending.begin(), m_pending.end());
    m_pending.clear();
    m_object.emitPropertiesChanged(names);
}

void ActionTransaction::setState(TransactionState state)
{
    if (state == m_state)
        return;
    m_state = state;
    if (state != TransactionState::Running)
        m_phase.clear();
    // Progress first, so that a client never sees it after the new state.
    m_throttle.stop();
    m_pending.insert({"State", "Phase", "Cancellable"});
    flush();
}

void ActionTransaction::setPlan(base::dbus::VariantMap plan)
{
    m_plan = std::move(plan);
    m_throttle.stop();
    m_pending.insert("Plan");
    flush();
}

void ActionTransaction::setDeadline(UnixSeconds deadline)
{
    if (deadline == m_deadline)
        return;
    m_deadline = deadline;
    m_throttle.stop();
    m_pending.insert("ConfirmationDeadline");
    flush();
}

void ActionTransaction::setCancellable(bool cancellable)
{
    if (cancellable == m_cancellable)
        return;
    m_cancellable = cancellable;
    changed({"Cancellable"});
}

void ActionTransaction::setProgress(const Progress& p)
{
    std::vector<std::string> names;
    const std::string phase = p.phase ? std::string(toString(*p.phase)) : m_phase;
    if (phase != m_phase) {
        m_phase = phase;
        names.push_back("Phase");
    }
    if (p.fraction != m_progress) {
        m_progress = p.fraction;
        names.push_back("Progress");
    }
    if (p.downloadedBytes != m_downloaded) {
        m_downloaded = p.downloadedBytes;
        names.push_back("DownloadedBytes");
    }
    if (p.totalBytes != m_total) {
        m_total = p.totalBytes;
        names.push_back("TotalBytes");
    }
    if (p.cancellable != m_cancellable) {
        m_cancellable = p.cancellable;
        names.push_back("Cancellable");
    }
    if (!names.empty())
        changed(std::move(names));
}

void ActionTransaction::finish(TransactionState state, std::optional<Error> error, UnixSeconds now)
{
    if (finished())
        return;
    m_state = state;
    m_error = std::move(error);
    m_finished = now;
    m_deadline = 0;
    m_cancellable = false;
    m_phase.clear();
    m_throttle.stop();
    m_pending.insert({"State", "Phase", "Error", "Finished", "Cancellable", "ConfirmationDeadline"});
    flush();
    const base::dbus::VariantMap e = m_error ? errorMap(*m_error) : base::dbus::VariantMap{};
    const std::string code = m_error ? *e.at("code").get<std::string>() : std::string();
    const std::string message = m_error ? m_error->message() : std::string();
    m_object.emitSignal("Finished", std::string(toString(m_state)), code, message);
}

} // namespace cutefish::store::service
