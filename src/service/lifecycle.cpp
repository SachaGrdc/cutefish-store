#include "lifecycle.h"

namespace cutefish::store::service {

Lifecycle::Lifecycle(base::EventLoop& loop, base::dbus::Connection& connection, std::chrono::milliseconds idleTimeout,
                     std::function<void()> onIdle)
    : m_loop(loop), m_connection(connection), m_idleTimeout(idleTimeout), m_onIdle(std::move(onIdle)), m_timer(&loop)
{
    m_timer.setSingleShot(true);
    m_timer.onTimeout = [this] {
        if (m_clients.empty() && m_activities == 0 && m_onIdle)
            m_onIdle();
    };
}

void Lifecycle::noteCaller(const std::string& sender)
{
    if (sender.empty() || m_clients.contains(sender))
        return;
    // Inserted first: the watch may call back at once.
    auto& watch = m_clients[sender];
    watch = m_connection.watchName(sender, [this, sender](const std::string& owner) {
        if (owner.empty())
            // Not from inside the watch's own callback: it is destroyed.
            m_loop.post([this, sender] { clientGone(sender); });
    });
    evaluate();
}

void Lifecycle::clientGone(const std::string& name)
{
    m_clients.erase(name);
    evaluate();
}

void Lifecycle::beginActivity()
{
    ++m_activities;
    evaluate();
}

void Lifecycle::endActivity()
{
    if (m_activities > 0)
        --m_activities;
    evaluate();
}

void Lifecycle::evaluate()
{
    if (m_clients.empty() && m_activities == 0) {
        if (!m_timer.isActive())
            m_timer.start(m_idleTimeout);
    } else {
        m_timer.stop();
    }
}

} // namespace cutefish::store::service
