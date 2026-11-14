#include "pktransport.h"

#include "pkenums.h"

#include <base/logging.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <type_traits>
#include <future>
#include <tuple>

namespace cutefish::store::service {

namespace dbus = cutefish::base::dbus;

namespace {

base::LogCategory cfPk("cutefish.store.packagekit");

constexpr const char* kService = "org.freedesktop.PackageKit";
constexpr const char* kPath = "/org/freedesktop/PackageKit";
constexpr const char* kInterface = "org.freedesktop.PackageKit";
constexpr const char* kTransaction = "org.freedesktop.PackageKit.Transaction";

pk::PkResult dbusFailure(const std::string& name, const std::string& message)
{
    pk::PkResult r;
    r.dbusError = name;
    r.dbusErrorMessage = message;
    return r;
}

pk::PkResult exitResult(pk::Exit exit, std::string details = {})
{
    pk::PkResult r;
    r.exit = static_cast<std::uint32_t>(exit);
    r.errorDetails = std::move(details);
    return r;
}

} // namespace

struct PkTransport::Worker
{
    /// A transaction being followed: a call of the queue, or an attached one.
    struct Running
    {
        PkCall call;
        std::string path;
        pk::PkResult result;
        pk::PkProgress progress;
        std::vector<dbus::Match> matches;
        dbus::PendingCall pending;
        int timer = 0;
        bool methodCalled = false;
        bool cancelRequested = false;
        bool finished = false;
    };

    explicit Worker(Bus b) : bus(b) {}

    Bus bus;
    base::EventLoop* loop = nullptr;
    std::unique_ptr<dbus::Connection> connection;
    std::deque<PkCall> queue;
    std::unique_ptr<Running> current;
    std::vector<std::unique_ptr<Running>> attached;
    dbus::NameWatch daemonWatch;
    std::string daemonOwner;
    std::vector<dbus::Match> daemonSignals;

    /// Every transaction packagekitd lists, with what is known of it (H.2).
    struct Watched
    {
        pk::PkTransactionInfo info;
        bool known = false; ///< properties read
        dbus::PendingCall pending;
    };
    std::map<std::string, Watched> watched;
    std::function<void(std::vector<pk::PkTransactionInfo>)> activityChanged;
    std::vector<dbus::Match> activitySignals;
    dbus::PendingCall listCall;

    dbus::Proxy daemon() const { return dbus::Proxy(connection.get(), kService, kPath, kInterface); }
    dbus::Proxy transaction(const std::string& path) const
    {
        return dbus::Proxy(connection.get(), kService, path, kTransaction);
    }

    void open()
    {
        connection = dbus::Connection::open(bus == Bus::System ? dbus::Connection::Bus::System
                                                               : dbus::Connection::Bus::Session,
                                            loop);
        if (!connection) {
            CF_WARNING(cfPk) << "no connection to the bus PackageKit is on";
            return;
        }
        // A transaction whose daemon goes away is lost (SERVICE.md §5.6):
        // packagekitd quitting while idle is normal, during work it is not.
        daemonWatch = connection->watchName(kService, [this](const std::string& owner) {
            const bool lost = !daemonOwner.empty() && owner.empty();
            const bool came = daemonOwner.empty() && !owner.empty();
            daemonOwner = owner;
            if (lost)
                loop->post([this] { daemonLost(); });
            // A daemon (re)started: what does it run?
            if (came)
                loop->post([this] {
                    if (activityChanged)
                        readTransactionList();
                });
        });
    }

    // --- Other programs' transactions (H.2) ---------------------------------

    bool ownPath(const std::string& path) const
    {
        if (current && current->path == path)
            return true;
        return std::any_of(attached.begin(), attached.end(), [&](const auto& a) { return a->path == path; });
    }

    void reportActivity()
    {
        if (!activityChanged)
            return;
        std::vector<pk::PkTransactionInfo> list;
        for (const auto& [path, w] : watched)
            if (w.known && !ownPath(path))
                list.push_back(w.info);
        activityChanged(std::move(list));
    }

    void watchTransactions(std::function<void(std::vector<pk::PkTransactionInfo>)> changed)
    {
        activitySignals.clear();
        watched.clear();
        listCall = {};
        activityChanged = std::move(changed);
        if (!activityChanged)
            return;
        if (!connection)
            return reportActivity();
        // The list changes when a transaction is created or destroyed; its
        // Role is set when its method is called, its Status as it works:
        // both come as PropertiesChanged of the Transaction object.
        activitySignals.push_back(daemon().onSignal("TransactionListChanged", [this](const dbus::Message& m) {
            std::vector<std::string> ids;
            if (m.read(ids))
                setTransactionList(std::move(ids));
            else
                readTransactionList();
        }));
        dbus::MatchRule rule;
        rule.sender = kService;
        rule.interface = "org.freedesktop.DBus.Properties";
        rule.member = "PropertiesChanged";
        rule.arg0 = kTransaction;
        activitySignals.push_back(connection->subscribe(rule, [this](const dbus::Message& m) {
            const auto it = watched.find(m.path());
            if (it == watched.end() || !it->second.known)
                return;
            std::string iface;
            dbus::VariantMap changed;
            std::vector<std::string> invalidated;
            if (!m.read(iface, changed, invalidated))
                return;
            if (applyInfo(it->second.info, changed))
                reportActivity();
        }));
        readTransactionList();
    }

    static bool applyInfo(pk::PkTransactionInfo& info, const dbus::VariantMap& properties)
    {
        bool changed = false;
        auto take = [&](const char* key, auto& field) {
            using T = std::decay_t<decltype(field)>;
            if (const auto it = properties.find(key); it != properties.end())
                if (const auto v = it->second.get<T>(); v && *v != field) {
                    field = *v;
                    changed = true;
                }
        };
        take("Role", info.role);
        take("Status", info.status);
        take("TransactionFlags", info.flags);
        return changed;
    }

    void readTransactionList()
    {
        listCall = daemon().callAsync("GetTransactionList", [this](const dbus::Reply& reply) {
            std::vector<dbus::ObjectPath> list;
            if (reply.isError() || !reply.read(list))
                return setTransactionList({});
            std::vector<std::string> paths;
            for (auto& p : list)
                paths.push_back(std::move(p.path));
            setTransactionList(std::move(paths));
        });
    }

    void setTransactionList(std::vector<std::string> paths)
    {
        const std::set<std::string> now(paths.begin(), paths.end());
        const bool removed = std::erase_if(watched, [&](const auto& w) { return !now.contains(w.first); }) > 0;
        for (const std::string& path : now) {
            if (watched.contains(path))
                continue;
            Watched& w = watched[path];
            w.info.path = path;
            w.pending = transaction(path).getAllAsync([this, path](const dbus::Reply& reply) {
                const auto it = watched.find(path);
                if (it == watched.end())
                    return;
                if (const auto all = reply.value<dbus::VariantMap>()) {
                    applyInfo(it->second.info, *all);
                    it->second.known = true;
                    reportActivity();
                }
                // Gone before it could be read: the next list drops it.
            });
        }
        if (removed)
            reportActivity();
    }

    void daemonLost()
    {
        if (activityChanged && !watched.empty()) {
            watched.clear();
            reportActivity();
        }
        auto lose = [this](Running* r) {
            if (r && r->methodCalled && !r->finished)
                finish(*r, exitResult(pk::Exit::Killed, "packagekitd went away during the transaction"));
        };
        lose(current.get());
        for (auto& a : attached)
            lose(a.get());
    }

    void submit(PkCall call)
    {
        if (call.urgent)
            queue.push_front(std::move(call));
        else
            queue.push_back(std::move(call));
        runNext();
    }

    void runNext()
    {
        if (current || queue.empty())
            return;
        current = std::make_unique<Running>();
        current->call = std::move(queue.front());
        queue.pop_front();
        start(*current);
    }

    void start(Running& r)
    {
        if (!connection)
            return finish(r, dbusFailure("org.freedesktop.DBus.Error.Disconnected", "no bus connection"));
        r.timer = loop->startTimer(r.call.timeout, false, [this, &r] {
            r.timer = 0;
            finish(r, dbusFailure("org.cutefish.Store.Error.Timeout", "PackageKit did not finish in time"));
        });
        r.pending = daemon().callAsync("CreateTransaction", [this, &r](const dbus::Reply& reply) {
            if (reply.isError())
                return finish(r, dbusFailure(reply.error().name, reply.error().message));
            const auto path = reply.value<dbus::ObjectPath>();
            if (!path)
                return finish(r, dbusFailure("org.freedesktop.DBus.Error.InvalidSignature", "no transaction path"));
            r.path = path->path;
            if (r.cancelRequested)
                return finish(r, exitResult(pk::Exit::Cancelled, "cancelled before it started"));
            subscribe(r);
            if (r.call.created)
                r.call.created(r.path);
            setHints(r);
        });
    }

    void subscribe(Running& r)
    {
        // Before the method is called, so that nothing is missed.
        const dbus::Proxy tx = transaction(r.path);
        r.matches.push_back(tx.onSignal("Package", [&r](const dbus::Message& m) {
            pk::PkPackage p;
            if (m.read(p.info, p.id, p.summary))
                r.result.packages.push_back(std::move(p));
        }));
        r.matches.push_back(tx.onSignal("Packages", [&r](const dbus::Message& m) {
            std::vector<std::tuple<std::uint32_t, std::string, std::string>> list;
            if (m.read(list))
                for (auto& [info, id, summary] : list)
                    r.result.packages.push_back({info, std::move(id), std::move(summary)});
        }));
        r.matches.push_back(tx.onSignal("RepoDetail", [&r](const dbus::Message& m) {
            pk::PkRepo repo;
            if (m.read(repo.id, repo.description, repo.enabled))
                r.result.repos.push_back(std::move(repo));
        }));
        r.matches.push_back(tx.onSignal("ErrorCode", [&r](const dbus::Message& m) {
            std::uint32_t code = 0;
            std::string details;
            if (m.read(code, details)) {
                r.result.errorCode = code;
                r.result.errorDetails = std::move(details);
            }
        }));
        r.matches.push_back(tx.onSignal("ItemProgress", [&r](const dbus::Message& m) {
            std::string id;
            std::uint32_t status = 0;
            std::uint32_t percentage = 0;
            if (!m.read(id, status, percentage))
                return;
            pk::PkProgress p = r.progress;
            p.item = std::move(id);
            if (r.call.progress)
                r.call.progress(p);
        }));
        r.matches.push_back(tx.onPropertiesChanged(
                [&r](const dbus::VariantMap& changed, const std::vector<std::string>&) { applyProperties(r, changed); }));
        r.matches.push_back(tx.onSignal("Finished", [this, &r](const dbus::Message& m) {
            std::uint32_t exit = 0;
            std::uint32_t runtime = 0;
            m.read(exit, runtime);
            pk::PkResult result = std::move(r.result);
            result.exit = exit;
            result.runtimeMs = runtime;
            finish(r, std::move(result));
        }));
    }

    static void applyProperties(Running& r, const dbus::VariantMap& changed)
    {
        bool any = false;
        if (const auto it = changed.find("Status"); it != changed.end())
            if (const auto v = it->second.get<std::uint32_t>()) {
                r.progress.status = *v;
                any = true;
            }
        if (const auto it = changed.find("Percentage"); it != changed.end())
            if (const auto v = it->second.get<std::uint32_t>()) {
                r.progress.percentage = *v;
                any = true;
            }
        if (const auto it = changed.find("AllowCancel"); it != changed.end())
            if (const auto v = it->second.get<bool>()) {
                r.progress.allowCancel = *v;
                any = true;
            }
        if (const auto it = changed.find("DownloadSizeRemaining"); it != changed.end())
            if (const auto v = it->second.get<std::uint64_t>()) {
                r.progress.downloadSizeRemaining = *v;
                r.result.downloadSizeRemaining = *v;
                any = true;
            }
        if (any && r.call.progress) {
            pk::PkProgress p = r.progress;
            p.item.clear();
            r.call.progress(p);
        }
    }

    void setHints(Running& r)
    {
        std::vector<std::string> hints{"supports-plural-signals=true",
                                       std::string("interactive=") + (r.call.interactive ? "true" : "false")};
        r.pending = transaction(r.path).callAsync(
                "SetHints",
                [this, &r](const dbus::Reply& reply) {
                    if (reply.isError())
                        return finish(r, dbusFailure(reply.error().name, reply.error().message));
                    callMethod(r);
                },
                hints);
    }

    void callMethod(Running& r)
    {
        dbus::Message m = connection->newMethodCall(kService, r.path, kTransaction, r.call.method);
        if (r.call.arguments)
            r.call.arguments(m);
        r.methodCalled = true;
        r.pending = connection->callAsync(m, [this, &r](const dbus::Reply& reply) {
            // An invalid argument is refused here, without Finished
            // (ARCHITECTURE §7.2, point 6).
            if (reply.isError())
                finish(r, dbusFailure(reply.error().name, reply.error().message));
        });
    }

    void finish(Running& r, pk::PkResult result)
    {
        if (r.finished)
            return;
        r.finished = true;
        if (r.timer) {
            loop->stopTimer(r.timer);
            r.timer = 0;
        }
        auto done = std::move(r.call.done);
        if (done)
            done(std::move(result));
        // Destroyed on the next turn: we may be inside one of its own
        // callbacks (a signal match, a reply).
        Running* raw = &r;
        loop->post([this, raw] {
            if (current.get() == raw) {
                current.reset();
                runNext();
                return;
            }
            std::erase_if(attached, [raw](const auto& a) { return a.get() == raw; });
        });
    }

    void cancel(const std::string& operation)
    {
        // Waiting calls never reach PackageKit.
        std::deque<PkCall> kept;
        for (PkCall& c : queue) {
            if (c.operation == operation && !operation.empty()) {
                if (c.done)
                    c.done(exitResult(pk::Exit::Cancelled, "cancelled while waiting"));
            } else {
                kept.push_back(std::move(c));
            }
        }
        queue = std::move(kept);
        auto ask = [this, &operation](Running* r) {
            if (!r || r->finished || operation.empty() || r->call.operation != operation)
                return;
            if (r->path.empty()) {
                r->cancelRequested = true; // CreateTransaction still on its way
                return;
            }
            // PackageKit decides; its Finished tells (ARCHITECTURE §7.2).
            transaction(r->path).send("Cancel");
        };
        ask(current.get());
        for (auto& a : attached)
            ask(a.get());
    }

    void attach(const std::string& path, std::string operation, std::function<void(const pk::PkProgress&)> progress,
                std::function<void(pk::PkResult)> done)
    {
        auto r = std::make_unique<Running>();
        r->path = path;
        r->methodCalled = true;
        r->call.operation = std::move(operation);
        r->call.progress = std::move(progress);
        r->call.done = std::move(done);
        Running& ref = *r;
        attached.push_back(std::move(r));
        if (!connection)
            return finish(ref, dbusFailure("org.freedesktop.DBus.Error.Disconnected", "no bus connection"));
        subscribe(ref);
        ref.timer = loop->startTimer(std::chrono::minutes(30), false, [this, &ref] {
            ref.timer = 0;
            finish(ref, dbusFailure("org.cutefish.Store.Error.Timeout", "the attached transaction did not finish"));
        });
        // Signals first, then the current state (SERVICE.md §5.6: signals
        // measured, properties to be validated). A transaction that ended
        // in between cannot tell how: it counts as lost, and the host
        // reconciles the installed state.
        ref.pending = transaction(path).getAllAsync([this, &ref](const dbus::Reply& reply) {
            const auto all = reply.value<dbus::VariantMap>();
            if (!all)
                return finish(ref, exitResult(pk::Exit::Killed, "the transaction ended before it could be followed"));
            applyProperties(ref, *all);
            if (ref.progress.status == static_cast<std::uint32_t>(pk::Status::Finished))
                finish(ref, exitResult(pk::Exit::Killed, "the transaction had already finished; outcome unknown"));
        });
    }
};

PkTransport::PkTransport(Bus bus) : m_worker(std::make_unique<Worker>(bus))
{
    std::promise<base::LoopHandle> ready;
    auto handle = ready.get_future();
    m_thread = std::thread([this, &ready] {
        base::EventLoop loop;
        m_worker->loop = &loop;
        m_worker->open();
        ready.set_value(loop.handle());
        loop.run();
        // Everything that uses the loop goes before it.
        m_worker->activitySignals.clear();
        m_worker->watched.clear();
        m_worker->listCall = {};
        m_worker->activityChanged = {};
        m_worker->daemonSignals.clear();
        m_worker->attached.clear();
        m_worker->current.reset();
        m_worker->queue.clear();
        m_worker->daemonWatch = {};
        m_worker->connection.reset();
        m_worker->loop = nullptr;
    });
    m_loop = handle.get();
}

PkTransport::~PkTransport()
{
    Worker* w = m_worker.get();
    m_loop.post([w] { w->loop->quit(0); });
    if (m_thread.joinable())
        m_thread.join();
}

void PkTransport::post(std::function<void(Worker&)> job)
{
    Worker* w = m_worker.get();
    m_loop.post([w, job = std::move(job)] { job(*w); });
}

void PkTransport::submit(PkCall call)
{
    auto shared = std::make_shared<PkCall>(std::move(call));
    post([shared](Worker& w) { w.submit(std::move(*shared)); });
}

void PkTransport::cancel(const std::string& operation)
{
    post([operation](Worker& w) { w.cancel(operation); });
}

void PkTransport::daemonInfo(std::function<void(DaemonInfo)> done)
{
    post([done = std::move(done)](Worker& w) mutable {
        if (!w.connection) {
            DaemonInfo info;
            info.error = "org.freedesktop.DBus.Error.Disconnected";
            return done(std::move(info));
        }
        auto slot = std::make_shared<dbus::PendingCall>();
        *slot = w.daemon().getAllAsync([&w, slot, done = std::move(done)](const dbus::Reply& reply) mutable {
            DaemonInfo info;
            if (reply.isError()) {
                info.error = reply.error().name;
            } else if (const auto all = reply.value<dbus::VariantMap>()) {
                auto get = [&](const char* key) { return all->contains(key) ? all->at(key) : dbus::Variant(); };
                info.backendName = get("BackendName").value<std::string>();
                info.distroId = get("DistroId").value<std::string>();
                info.roles = get("Roles").value<std::uint64_t>();
                info.filters = get("Filters").value<std::uint64_t>();
            }
            done(std::move(info));
            // The slot owns this callback: released on the next turn.
            w.loop->post([slot] { slot->reset(); });
        });
    });
}

void PkTransport::transactions(std::function<void(std::vector<std::string>, std::string)> done)
{
    post([done = std::move(done)](Worker& w) mutable {
        if (!w.connection)
            return done({}, "org.freedesktop.DBus.Error.Disconnected");
        auto slot = std::make_shared<dbus::PendingCall>();
        *slot = w.daemon().callAsync("GetTransactionList",
                                     [&w, slot, done = std::move(done)](const dbus::Reply& reply) mutable {
                                         std::vector<std::string> paths;
                                         if (reply.isError()) {
                                             done({}, reply.error().name);
                                         } else {
                                             std::vector<dbus::ObjectPath> list;
                                             reply.read(list);
                                             for (auto& p : list)
                                                 paths.push_back(std::move(p.path));
                                             done(std::move(paths), {});
                                         }
                                         // The slot owns this callback: released on the next turn.
                                         w.loop->post([slot] { slot->reset(); });
                                     });
    });
}

void PkTransport::attach(const std::string& path, std::string operation,
                         std::function<void(const pk::PkProgress&)> progress, std::function<void(pk::PkResult)> done)
{
    post([path, operation = std::move(operation), progress = std::move(progress),
          done = std::move(done)](Worker& w) mutable {
        w.attach(path, std::move(operation), std::move(progress), std::move(done));
    });
}

void PkTransport::watchDaemon(std::function<void(bool, bool, bool)> changed)
{
    post([changed = std::move(changed)](Worker& w) {
        w.daemonSignals.clear();
        if (!w.connection || !changed)
            return;
        const dbus::Proxy d = w.daemon();
        w.daemonSignals.push_back(d.onSignal("InstalledChanged", [changed](const dbus::Message&) {
            changed(true, false, false);
        }));
        w.daemonSignals.push_back(d.onSignal("UpdatesChanged", [changed](const dbus::Message&) {
            changed(false, true, false);
        }));
        w.daemonSignals.push_back(d.onSignal("RepoListChanged", [changed](const dbus::Message&) {
            changed(false, false, true);
        }));
    });
}

void PkTransport::watchTransactions(std::function<void(std::vector<pk::PkTransactionInfo>)> changed)
{
    post([changed = std::move(changed)](Worker& w) mutable { w.watchTransactions(std::move(changed)); });
}

} // namespace cutefish::store::service
