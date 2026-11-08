#pragma once

// D-Bus transport to PackageKit (D-007, SERVICE.md §6.1): a thread of its
// own, with its own EventLoop and its own connection to the bus (the
// system bus in production; a private bus in the tests). Every PackageKit
// transaction goes through one queue, one at a time, queries included
// (P-009: the APT backend runs one transaction at a time anyway). Nothing
// here interprets what PackageKit answers: the results are plain
// structures (pkdata.h) for store-backend-packagekit.
//
// Threading: submit(), cancel() and the other public methods may be called
// from any thread; every callback of a PkCall runs on the PackageKit
// thread, and its owner posts what it needs to its own loop.

#include "pkdata.h"

#include <base/dbus.h>
#include <base/event_loop.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cutefish::store::service {

namespace pk = cutefish::store::packagekit;

/// One PackageKit transaction to run.
struct PkCall
{
    /// The Transaction method (GetPackages, InstallPackages...).
    std::string method;
    /// Appends the method's arguments.
    std::function<void(base::dbus::Message&)> arguments;
    /// SetHints `interactive=true`: polkit may show a dialog. Only for an
    /// action the user asked for (SERVICE.md §6.4).
    bool interactive = false;
    /// The service's own limit: PackageKit may answer a method with a D-Bus
    /// error and no Finished (SERVICE.md §5.3).
    std::chrono::milliseconds timeout = std::chrono::minutes(2);
    /// The operation this transaction belongs to, for cancel(); may be empty.
    std::string operation;
    /// Runs before any other call waiting in the queue: the second half of
    /// an operation (a simulation after its Resolve, the real transaction
    /// after its simulation) does not let another transaction in between.
    bool urgent = false;
    /// Called once the transaction object exists (its path is the handle).
    std::function<void(const std::string& path)> created = {};
    std::function<void(const pk::PkProgress&)> progress = {};
    /// Called once, at the end.
    std::function<void(pk::PkResult)> done;
};

class PkTransport
{
public:
    enum class Bus { System, Session };

    explicit PkTransport(Bus bus);
    /// Stops the thread. Transactions still running in packagekitd go on
    /// there (SERVICE.md §3.4); their callbacks are dropped.
    ~PkTransport();

    PkTransport(const PkTransport&) = delete;
    PkTransport& operator=(const PkTransport&) = delete;

    void submit(PkCall call);

    /// Cancels the operation's waiting calls (done() with exit cancelled),
    /// and asks PackageKit to cancel its running transaction. The request
    /// only: PackageKit decides (ARCHITECTURE §7.2).
    void cancel(const std::string& operation);

    /// The daemon's properties (GetAll on org.freedesktop.PackageKit), or a
    /// D-Bus error name in `error`.
    struct DaemonInfo
    {
        std::string error = {};
        std::string backendName = {};
        std::string distroId = {};
        std::uint64_t roles = 0;
        std::uint64_t filters = 0;
    };
    void daemonInfo(std::function<void(DaemonInfo)> done);

    /// The transactions packagekitd is running (GetTransactionList).
    void transactions(std::function<void(std::vector<std::string> paths, std::string error)> done);

    /// Follows a transaction started earlier (by a previous instance of the
    /// service): its progress and its end, as for a call.
    void attach(const std::string& path, std::string operation, std::function<void(const pk::PkProgress&)> progress,
                std::function<void(pk::PkResult)> done);

    /// InstalledChanged, UpdatesChanged, RepoListChanged of the daemon:
    /// `changed(installed, updates, repositories)`, on the PackageKit thread.
    void watchDaemon(std::function<void(bool installed, bool updates, bool repositories)> changed);

private:
    struct Worker;
    void post(std::function<void(Worker&)> job);

    std::unique_ptr<Worker> m_worker;
    base::LoopHandle m_loop;
    std::thread m_thread;
};

} // namespace cutefish::store::service
