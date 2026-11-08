#pragma once

// A private session bus for one test: dbus-daemon with a configuration of
// its own, no privilege, nothing activatable. DBUS_SESSION_BUS_ADDRESS
// points to it for the rest of the process.

#include "catalogfixture.h"

#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace privatebus {

class PrivateBus
{
public:
    PrivateBus()
    {
        {
            std::ofstream out(m_dir / "bus.conf");
            out << "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
                   " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                   "<busconfig>\n  <type>session</type>\n"
                << "  <listen>unix:path=" << (m_dir / "bus").string() << "</listen>\n"
                << "  <policy context=\"default\">\n"
                   "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                   "    <allow eavesdrop=\"true\"/>\n    <allow own=\"*\"/>\n  </policy>\n"
                   "</busconfig>\n";
        }
        const std::string address = "unix:path=" + (m_dir / "bus").string();
        ::setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1);
        int ready[2];
        if (::pipe(ready) != 0)
            std::abort();
        m_pid = ::fork();
        if (m_pid == 0) {
            ::close(ready[0]);
            const std::string config = "--config-file=" + (m_dir / "bus.conf").string();
            const std::string print = "--print-address=" + std::to_string(ready[1]);
            ::execlp(DBUS_DAEMON, DBUS_DAEMON, config.c_str(), "--nofork", print.c_str(), nullptr);
            ::_exit(127);
        }
        ::close(ready[1]);
        char buffer[512];
        const ssize_t n = ::read(ready[0], buffer, sizeof buffer);
        ::close(ready[0]);
        if (n <= 0)
            ADD_FAILURE() << "dbus-daemon did not start";
    }

    ~PrivateBus()
    {
        if (m_pid > 0) {
            ::kill(m_pid, SIGTERM);
            int status = 0;
            ::waitpid(m_pid, &status, 0);
        }
    }

    const std::filesystem::path& dir() const { return m_dir.path(); }

private:
    catalogfixture::TempDir m_dir;
    pid_t m_pid = -1;
};

} // namespace privatebus
