#pragma once

// A private session bus per test, without any privilege: dbus-daemon with
// a configuration of our own, an activation directory holding
// org.cutefish.Store.service (Exec = the service binary under test, with
// its data in the test's directory), and a client connection made with
// Cutefish Toolkit. The service is activated by the first call, exactly as
// on a real session (without systemd: no SystemdService line).

#include "catalogfixture.h"

#include <base/dbus.h>
#include <base/event_loop.h>

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace busfixture {

namespace base = cutefish::base;
namespace dbus = cutefish::base::dbus;
using catalogfixture::TempDir;
using namespace std::chrono_literals;

inline const std::string kName = "org.cutefish.Store";
inline const std::string kPath = "/org/cutefish/Store";
inline const std::string kInterface = "org.cutefish.Store1";
inline const std::string kTransaction = "org.cutefish.Store1.Transaction";

struct ServiceSetup
{
    std::chrono::milliseconds idleTimeout = 60s;
    std::chrono::milliseconds retention = 60s;
    /// Seconds the indexer waits before working (a slow refresh).
    int indexerDelay = 0;
    std::chrono::milliseconds confirmationTimeout = 60s;
    /// --packagekit: `off` so that no test reaches the system's packagekitd;
    /// `session` for a simulated one on the test's bus (fakepackagekit.h).
    std::string packagekit = "off";
    /// The Debian test source in sources.conf (otherwise it may come from
    /// the backend's catalogue sources, `swcatalog`).
    bool debianInFile = true;
};

/// The bus, the service's files and a client.
class Bus
{
public:
    explicit Bus(ServiceSetup setup = {})
    {
        std::filesystem::create_directories(dir / "services");
        std::filesystem::create_directories(dir / "data");
        // The test sources, copied so that a test may change or remove them.
        std::filesystem::copy_file(std::filesystem::path(SERVICE_TEST_DATA) / "debian.yml", dir / "debian.yml");
        std::filesystem::copy_file(std::filesystem::path(SERVICE_TEST_DATA) / "flathub.xml", dir / "flathub.xml");
        writeSources(true, setup.debianInFile);
        std::filesystem::create_directories(dir / "swcatalog" / "yaml");
        writeIndexer(setup.indexerDelay);
        writeServiceFile(setup);
        writeConfig();
        startDaemon();
        client = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);
        if (!client)
            ADD_FAILURE() << "no client connection";
    }

    ~Bus()
    {
        // Stop an instance still running (a test that ends early), then the
        // bus. The pid is asked while the client is still connected.
        const auto pid = servicePid();
        client.reset();
        if (pid) {
            ::kill(static_cast<pid_t>(*pid), SIGTERM);
            for (int i = 0; i < 250 && ::kill(static_cast<pid_t>(*pid), 0) == 0; ++i)
                std::this_thread::sleep_for(20ms);
            ::kill(static_cast<pid_t>(*pid), SIGKILL);
        }
        if (m_daemon > 0) {
            ::kill(m_daemon, SIGTERM);
            int status = 0;
            ::waitpid(m_daemon, &status, 0);
        }
    }

    /// Lists the two test sources, or one of them.
    void writeSources(bool withFlathub, bool withDebian = true)
    {
        std::ofstream out(dir / "sources.conf", std::ios::trunc);
        out << "# test sources\n";
        if (withDebian)
            out << "id=packagekit:test-main,backend=packagekit,scope=system,kind=distribution,origin=test-main,"
                   "priority=10,file="
                << (dir / "debian.yml").string() << "\n";
        if (withFlathub)
            out << "id=flatpak:user:flathub,backend=flatpak,scope=user,kind=flathub,origin=flathub,priority=20,file="
                << (dir / "flathub.xml").string() << "\n";
    }

    dbus::Proxy store() const { return dbus::Proxy(client.get(), kName, kPath, kInterface); }
    dbus::Proxy transaction(const std::string& path) const
    {
        return dbus::Proxy(client.get(), kName, path, kTransaction);
    }
    dbus::Proxy bus() const
    {
        return dbus::Proxy(client.get(), "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus");
    }

    bool serviceRunning() const
    {
        bool has = false;
        return bus().call("NameHasOwner", kName).read(has) && has;
    }
    std::optional<std::uint32_t> servicePid() const
    {
        if (!client)
            return std::nullopt;
        std::uint32_t pid = 0;
        if (!bus().call("GetConnectionUnixProcessID", kName).read(pid))
            return std::nullopt;
        return pid;
    }

    template <typename T>
    std::optional<T> property(const std::string& name) const
    {
        return store().property<T>(name);
    }

    /// Polls until pred() is true or the timeout passes, running the
    /// client's loop meanwhile (signals are delivered).
    bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 20s)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            const int t = loop.startTimer(20ms, false, [this] { loop.quit(0); });
            loop.run();
            loop.stopTimer(t);
        }
        return true;
    }

    /// Activates the service if needed and waits for a catalogue.
    bool waitReady()
    {
        return waitUntil([this] { return property<std::string>("State") == std::optional<std::string>("ready"); });
    }

    TempDir dir;
    base::EventLoop loop;
    std::unique_ptr<dbus::Connection> client;

private:
    void writeIndexer(int delay)
    {
        // A wrapper, so that a test can make the refresh slow.
        const auto path = dir / "indexer.sh";
        std::ofstream out(path);
        out << "#!/bin/sh\n";
        // `wait` returns at once on SIGTERM, as the real indexer would stop.
        if (delay > 0)
            out << "trap 'exit 143' TERM\nsleep " << delay << " &\nwait $!\n";
        out << "exec '" << INDEXER_BINARY << "' \"$@\"\n";
        out.close();
        ::chmod(path.c_str(), 0755);
    }

    void writeServiceFile(const ServiceSetup& s)
    {
        std::ofstream out(dir / "services" / "org.cutefish.Store.service");
        out << "[D-BUS Service]\nName=" << kName << "\nExec=" << SERVICE_BINARY << " --data-dir "
            << (dir / "data").string() << " --sources " << (dir / "sources.conf").string() << " --indexer "
            << (dir / "indexer.sh").string() << " --idle-timeout-ms " << s.idleTimeout.count()
            << " --retention-ms " << s.retention.count()
            << " --confirmation-timeout-ms " << s.confirmationTimeout.count() << " --locale C --packagekit " << s.packagekit
            << " --swcatalog " << (dir / "swcatalog").string() << "\n";
    }

    void writeConfig()
    {
        std::ofstream out(dir / "bus.conf");
        out << "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
               " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
               "<busconfig>\n  <type>session</type>\n"
            << "  <listen>unix:path=" << (dir / "bus").string() << "</listen>\n"
            << "  <servicedir>" << (dir / "services").string() << "</servicedir>\n"
            << "  <policy context=\"default\">\n"
               "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
               "    <allow eavesdrop=\"true\"/>\n    <allow own=\"*\"/>\n  </policy>\n"
               "</busconfig>\n";
    }

    void startDaemon()
    {
        address = "unix:path=" + (dir / "bus").string();
        // Clients and the activated service find the bus here.
        ::setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1);
        int ready[2];
        if (::pipe(ready) != 0)
            std::abort();
        m_daemon = ::fork();
        if (m_daemon == 0) {
            ::close(ready[0]);
            const std::string config = "--config-file=" + (dir / "bus.conf").string();
            const std::string print = "--print-address=" + std::to_string(ready[1]);
            ::execlp(DBUS_DAEMON, DBUS_DAEMON, config.c_str(), "--nofork", print.c_str(), nullptr);
            ::_exit(127);
        }
        ::close(ready[1]);
        // dbus-daemon prints its address once it listens.
        char buffer[512];
        const ssize_t n = ::read(ready[0], buffer, sizeof buffer);
        ::close(ready[0]);
        if (n <= 0)
            ADD_FAILURE() << "dbus-daemon did not start";
    }

    pid_t m_daemon = -1;

public:
    std::string address;
};

/// A signal subscription rule.
inline dbus::MatchRule rule(const std::string& path, const std::string& interface, const std::string& member)
{
    dbus::MatchRule r;
    r.path = path;
    r.interface = interface;
    r.member = member;
    return r;
}

/// A Search or Browse result, decoded.
struct Page
{
    std::uint64_t generation = 0;
    std::uint32_t total = 0;
    std::vector<dbus::VariantMap> items;

    std::vector<std::string> ids() const
    {
        std::vector<std::string> out;
        for (const auto& m : items)
            out.push_back(m.at("id").value<std::string>());
        return out;
    }
};

inline std::optional<Page> page(const dbus::Reply& r)
{
    Page p;
    if (!r.read(p.generation, p.total, p.items))
        return std::nullopt;
    return p;
}

} // namespace busfixture
