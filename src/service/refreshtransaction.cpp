#include "refreshtransaction.h"

#include "marshal.h"

#include <cstdint>
#include <vector>

namespace cutefish::store::service {

RefreshTransaction::RefreshTransaction(base::dbus::Connection* connection, std::string path, std::string id,
                                       UnixSeconds created, std::function<bool()> cancel,
                                       std::function<bool()> cancellable)
    : m_object(connection, std::move(path), "org.cutefish.Store1.Transaction"), m_id(std::move(id)),
      m_created(created), m_cancel(std::move(cancel)), m_cancellable(std::move(cancellable))
{
    using namespace base::dbus;
    m_object.addProperty("Id", [this] { return m_id; });
    m_object.addProperty("Kind", [] { return std::string("refresh"); });
    m_object.addProperty("Backend", [] { return std::string("multiple"); });
    m_object.addProperty("AppIds", [] { return std::vector<std::string>{}; });
    m_object.addProperty("OfferIds", [] { return std::vector<std::string>{}; });
    m_object.addProperty("State", [this] { return std::string(toString(m_state)); });
    m_object.addProperty("Phase", [] { return std::string(); });
    m_object.addProperty("Progress", [] { return -1.0; });
    m_object.addProperty("DownloadedBytes", [] { return std::uint64_t(0); });
    m_object.addProperty("TotalBytes", [] { return std::uint64_t(0); });
    m_object.addProperty("Cancellable", [this] { return !finished() && m_cancellable && m_cancellable(); });
    m_object.addProperty("Plan", [] { return VariantMap{}; });
    m_object.addProperty("Error", [this] { return m_error ? errorMap(*m_error) : VariantMap{}; });
    m_object.addProperty("Created", [this] { return static_cast<std::uint64_t>(m_created); });
    m_object.addProperty("Finished", [this] { return static_cast<std::uint64_t>(m_finished); });

    m_object.addMethod("Confirm", "", "", [](MethodCall& call) {
        // A refresh has no plan to confirm (ARCHITECTURE §5.3).
        call.replyError(errors::kNotAllowed, "nothing to confirm");
    });
    m_object.addMethod("Cancel", "", "", [this](MethodCall& call) {
        if (finished() || !m_cancel || !m_cancel())
            return call.replyError(errors::kNotAllowed, "the refresh cannot be cancelled now");
        call.reply();
    });
    m_object.addSignal("Finished", "sss", {"state", "error_code", "message"});
    m_object.publish();
}

void RefreshTransaction::finish(TransactionState state, std::optional<Error> error, UnixSeconds now)
{
    if (finished())
        return;
    m_state = state;
    m_error = std::move(error);
    m_finished = now;
    m_object.emitPropertiesChanged({"State", "Error", "Finished", "Cancellable"});
    const base::dbus::VariantMap e = m_error ? errorMap(*m_error) : base::dbus::VariantMap{};
    const std::string code = m_error ? *e.at("code").get<std::string>() : std::string();
    const std::string message = m_error ? m_error->message() : std::string();
    m_object.emitSignal("Finished", std::string(toString(m_state)), code, message);
}

void RefreshTransaction::updateCancellable()
{
    m_object.emitPropertiesChanged({"Cancellable"});
}

} // namespace cutefish::store::service
