#pragma once

// Clients and inactivity (P-010, SERVICE.md §3.2). A known client is a bus
// connection (unique name) that called a method; the service follows its
// disappearance. It stays up while a known client is connected or work is
// in progress, then stops after the idle timeout.
//
// Limit: a client that only reads properties (Get, GetAll) is not seen:
// Cutefish Toolkit's property getters do not expose the caller. The
// interface calls methods from its start, so it is counted.

#include <base/dbus.h>
#include <base/event_loop.h>
#include <base/timer.h>

#include <chrono>
#include <functional>
#include <map>
#include <string>

namespace cutefish::store::service {

class Lifecycle
{
public:
    Lifecycle(base::EventLoop& loop, base::dbus::Connection& connection, std::chrono::milliseconds idleTimeout,
              std::function<void()> onIdle);

    /// A method call from `sender` (unique name).
    void noteCaller(const std::string& sender);
    /// Work the service must not stop during (a refresh, an indexing).
    void beginActivity();
    void endActivity();

    std::size_t clients() const noexcept { return m_clients.size(); }
    int activities() const noexcept { return m_activities; }

    /// Starts the idle timer if nothing keeps the service up.
    void evaluate();

private:
    void clientGone(const std::string& name);

    base::EventLoop& m_loop;
    base::dbus::Connection& m_connection;
    std::chrono::milliseconds m_idleTimeout;
    std::function<void()> m_onIdle;
    base::Timer m_timer;
    std::map<std::string, base::dbus::NameWatch> m_clients;
    int m_activities = 0;
};

} // namespace cutefish::store::service
