#pragma once

// A simulated PackageKit daemon, for the tests of the PackageKit backend: it
// owns org.freedesktop.PackageKit on a private bus (privatebus.h), on a
// thread and loop of its own, and answers as PackageKit 1.3.1 does: one
// object per transaction, SetHints, a method that is answered at once, then
// property changes (Status, Percentage, AllowCancel, DownloadSizeRemaining),
// ItemProgress, Packages or RepoDetail, ErrorCode, Finished and Destroy.
// What it answers is scripted by the test; every call is recorded.
//
// A double, not PackageKit: it reproduces the protocol the backend relies
// on (ARCHITECTURE §7.2, prototype 4), not APT's resolution.

#include "pkdata.h"
#include "pkenums.h"

#include <base/dbus.h>
#include <base/event_loop.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace fakepk {

namespace dbus = cutefish::base::dbus;
namespace pk = cutefish::store::packagekit;
using namespace std::chrono_literals;

/// One step of a transaction's progress.
struct Step
{
    pk::Status status = pk::Status::Running;
    std::uint32_t percentage = 101;
    bool allowCancel = true;
    std::string item = {};
};

/// What one call answers.
struct Answer
{
    std::vector<pk::PkPackage> packages = {};
    std::vector<pk::PkRepo> repos = {};
    pk::Exit exit = pk::Exit::Success;
    std::optional<std::uint32_t> error = {};
    std::string errorDetails = {};
    std::uint64_t downloadSize = 0;
    std::vector<Step> steps = {};
    /// Answers the method itself with this D-Bus error (no Finished).
    std::string dbusError = {};
    /// Never finishes (for the timeouts).
    bool hang = false;
    /// Runs its steps, then stays running until finish() is called (a
    /// transaction that outlives the service, for re-attachment).
    bool hold = false;
};

struct Call
{
    std::string method;
    std::uint64_t flags = 0; ///< filter or transaction flags
    std::vector<std::string> values = {};
    std::vector<std::string> hints = {};
    bool simulate() const { return flags & pk::flag::kSimulate; }
};

inline std::uint32_t errorCode(const char* name)
{
    for (std::uint32_t c = 0; c < 80; ++c)
        if (pk::errorName(c) == name)
            return c;
    return 0;
}

class FakePackageKit
{
public:
    FakePackageKit() { start(); }
    ~FakePackageKit() { stop(); }

    // --- Script (any thread) --------------------------------------------------
    std::string backendName = "apt";
    std::string distroId = "debian;13;x86_64";
    std::uint64_t roles = ~std::uint64_t(0);

    /// Answers for `method`; for the three actions, `simulate` tells the
    /// simulation from the real transaction. Answers are used in order; the
    /// last one is kept for further calls.
    void answer(const std::string& method, Answer a, bool simulate = false)
    {
        std::lock_guard lock(m_mutex);
        m_answers[{method, simulate}].push_back(std::move(a));
    }

    /// Replaces every answer waiting for `method`.
    void replace(const std::string& method, Answer a, bool simulate = false)
    {
        std::lock_guard lock(m_mutex);
        auto& list = m_answers[{method, simulate}];
        list.clear();
        list.push_back(std::move(a));
    }

    std::vector<Call> calls() const
    {
        std::lock_guard lock(m_mutex);
        return m_calls;
    }
    int cancels() const
    {
        std::lock_guard lock(m_mutex);
        return m_cancels;
    }
    /// Most transactions that ran at the same time.
    int maxConcurrent() const
    {
        std::lock_guard lock(m_mutex);
        return m_maxActive;
    }

    /// Emits a daemon signal (InstalledChanged, UpdatesChanged, RepoListChanged).
    void emitDaemon(const std::string& member)
    {
        run([this, member] { m_daemon->emitSignal(member); });
    }

    /// Starts a transaction as another client would (a previous instance of
    /// the service), and returns its path.
    std::string startDetached(const std::string& method, Answer a)
    {
        std::promise<std::string> path;
        auto future = path.get_future();
        run([this, method, a = std::move(a), &path]() mutable {
            const std::string p = newTransaction();
            m_transactions[p]->begin({method}, std::move(a));
            path.set_value(p);
        });
        return future.get();
    }

    /// The transactions the daemon is still running (GetTransactionList).
    /// Asks its own loop, like any other client.
    std::vector<std::string> runningTransactions()
    {
        std::promise<std::vector<std::string>> result;
        auto future = result.get_future();
        run([this, &result] {
            std::vector<std::string> paths;
            for (const auto& [path, transaction] : m_transactions)
                if (transaction->running())
                    paths.push_back(path);
            result.set_value(std::move(paths));
        });
        return future.get();
    }

    /// Ends a transaction started with `hold`, as packagekitd would when the
    /// work finishes. False when the daemon no longer has it.
    bool finish(const std::string& path, pk::Exit exit = pk::Exit::Success,
                std::optional<std::uint32_t> error = std::nullopt)
    {
        std::promise<bool> result;
        auto future = result.get_future();
        run([this, path, exit, error, &result] {
            const auto it = m_transactions.find(path);
            if (it == m_transactions.end()) {
                result.set_value(false);
                return;
            }
            it->second->finishNow(exit, error);
            result.set_value(true);
        });
        return future.get();
    }

    /// The daemon goes away: name released, connection closed.
    void stop()
    {
        if (!m_thread.joinable())
            return;
        m_handle.post([this] { m_loop->quit(0); });
        m_thread.join();
    }
    void start()
    {
        if (m_thread.joinable())
            return;
        std::promise<void> ready;
        auto future = ready.get_future();
        m_thread = std::thread([this, &ready] {
            cutefish::base::EventLoop loop;
            m_loop = &loop;
            m_handle = loop.handle();
            m_connection = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);
            exportDaemon();
            m_connection->requestName("org.freedesktop.PackageKit");
            ready.set_value();
            loop.run();
            m_transactions.clear();
            m_daemon.reset();
            m_connection.reset();
            m_loop = nullptr;
        });
        future.wait();
    }

private:
    class Transaction
    {
    public:
        Transaction(FakePackageKit& fake, const std::string& path) : m_fake(fake), m_object(fake.m_connection.get(), path,
                                                                                                "org.freedesktop.PackageKit.Transaction")
        {
            m_object.addProperty("Status", [this] { return static_cast<std::uint32_t>(m_status); });
            m_object.addProperty("Percentage", [this] { return m_percentage; });
            m_object.addProperty("AllowCancel", [this] { return m_allowCancel; });
            m_object.addProperty("DownloadSizeRemaining", [this] { return m_downloadRemaining; });
            m_object.addProperty("Role", [] { return std::uint32_t(0); });
            m_object.addMethod("SetHints", "as", "", [this](dbus::MethodCall& c) {
                c.read(m_hints);
                c.reply();
            });
            auto action = [this](const char* method, const char* signature) {
                m_object.addMethod(method, signature, "", [this, method](dbus::MethodCall& c) { called(method, c); });
            };
            action("GetPackages", "t");
            action("GetRepoList", "t");
            action("GetUpdates", "t");
            action("Resolve", "tas");
            action("InstallPackages", "tas");
            action("RemovePackages", "tasbb");
            action("UpdatePackages", "tas");
            action("RefreshCache", "b");
            m_object.addMethod("Cancel", "", "", [this](dbus::MethodCall& c) {
                {
                    std::lock_guard lock(m_fake.m_mutex);
                    ++m_fake.m_cancels;
                }
                if (!m_allowCancel || m_finished)
                    return c.replyError("org.freedesktop.PackageKit.Transaction.CannotCancel", "not now");
                c.reply();
                m_cancelled = true;
            });
            for (const char* s : {"Package", "Packages", "RepoDetail", "ErrorCode", "ItemProgress", "Finished", "Destroy"})
                m_object.addSignal(s, signatureOf(s));
            m_object.publish();
        }

        bool running() const { return !m_finished; }

        /// Ends a held transaction, as the daemon would when its work ends.
        void finishNow(pk::Exit exit, std::optional<std::uint32_t> error)
        {
            if (m_finished)
                return;
            m_answer.exit = exit;
            m_answer.error = error;
            m_answer.steps.clear();
            m_answer.hold = false;
            end();
        }

        void begin(Call call, Answer a)
        {
            m_answer = std::move(a);
            m_downloadRemaining = m_answer.downloadSize;
            {
                std::lock_guard lock(m_fake.m_mutex);
                call.hints = m_hints;
                m_fake.m_calls.push_back(std::move(call));
                m_fake.m_maxActive = std::max(m_fake.m_maxActive, ++m_fake.m_active);
            }
            if (m_answer.hang)
                return;
            m_timer = m_fake.m_loop->startTimer(15ms, true, [this] { tick(); });
        }

    private:
        static std::string signatureOf(const std::string& s)
        {
            static const std::map<std::string, std::string> kSig{{"Package", "uss"}, {"Packages", "a(uss)"},
                                                                 {"RepoDetail", "ssb"}, {"ErrorCode", "us"},
                                                                 {"ItemProgress", "suu"}, {"Finished", "uu"},
                                                                 {"Destroy", ""}};
            return kSig.at(s);
        }

        void called(const std::string& method, dbus::MethodCall& c)
        {
            Call call{method};
            const auto& args = c.arguments();
            if (!args.empty()) {
                if (auto f = args[0].get<std::uint64_t>())
                    call.flags = *f;
                else if (auto b = args[0].get<bool>())
                    call.flags = *b;
            }
            if (args.size() > 1)
                if (auto v = args[1].get<std::vector<std::string>>())
                    call.values = *v;
            Answer a = m_fake.next(method, call.simulate());
            if (!a.dbusError.empty()) {
                c.replyError(a.dbusError, "refused by the fake");
                std::lock_guard lock(m_fake.m_mutex);
                m_fake.m_calls.push_back(std::move(call));
                return;
            }
            c.reply();
            begin(std::move(call), std::move(a));
        }

        void tick()
        {
            if (m_cancelled) {
                m_answer.exit = pk::Exit::Cancelled;
                m_answer.error = errorCode("transaction-cancelled");
                return end();
            }
            if (m_step < m_answer.steps.size()) {
                const Step& s = m_answer.steps[m_step++];
                m_status = s.status;
                m_percentage = s.percentage;
                m_allowCancel = s.allowCancel;
                if (m_downloadRemaining > 0)
                    m_downloadRemaining /= 2;
                m_object.emitPropertiesChanged({"Status", "Percentage", "AllowCancel", "DownloadSizeRemaining"});
                if (!s.item.empty())
                    m_object.emitSignal("ItemProgress", s.item, static_cast<std::uint32_t>(s.status), s.percentage);
                return;
            }
            if (m_answer.hold)
                return;
            end();
        }

        void end()
        {
            m_fake.m_loop->stopTimer(m_timer);
            m_timer = 0;
            m_finished = true;
            if (!m_answer.packages.empty()) {
                std::vector<std::tuple<std::uint32_t, std::string, std::string>> list;
                for (const auto& p : m_answer.packages)
                    list.emplace_back(p.info, p.id, p.summary);
                m_object.emitSignal("Packages", list);
            }
            for (const auto& r : m_answer.repos)
                m_object.emitSignal("RepoDetail", r.id, r.description, r.enabled);
            if (m_answer.error)
                m_object.emitSignal("ErrorCode", *m_answer.error, m_answer.errorDetails);
            m_status = pk::Status::Finished;
            m_object.emitPropertiesChanged({"Status"});
            m_object.emitSignal("Finished", static_cast<std::uint32_t>(m_answer.exit), std::uint32_t(10));
            m_object.emitSignal("Destroy");
            std::lock_guard lock(m_fake.m_mutex);
            --m_fake.m_active;
        }

        FakePackageKit& m_fake;
        dbus::Object m_object;
        std::vector<std::string> m_hints;
        Answer m_answer;
        std::size_t m_step = 0;
        pk::Status m_status = pk::Status::Wait;
        std::uint32_t m_percentage = 101;
        bool m_allowCancel = true;
        std::uint64_t m_downloadRemaining = 0;
        bool m_cancelled = false;
        bool m_finished = false;
        int m_timer = 0;
    };

    Answer next(const std::string& method, bool simulate)
    {
        std::lock_guard lock(m_mutex);
        const bool action = method == "InstallPackages" || method == "RemovePackages" || method == "UpdatePackages";
        auto& list = m_answers[{method, action && simulate}];
        if (list.empty())
            return {};
        Answer a = list.front();
        if (list.size() > 1)
            list.pop_front();
        return a;
    }

    std::string newTransaction()
    {
        const std::string path = "/" + std::to_string(++m_next) + "_fake";
        m_transactions[path] = std::make_unique<Transaction>(*this, path);
        return path;
    }

    void exportDaemon()
    {
        m_daemon = std::make_unique<dbus::Object>(m_connection.get(), "/org/freedesktop/PackageKit",
                                                  "org.freedesktop.PackageKit");
        m_daemon->addProperty("BackendName", [this] {
            std::lock_guard lock(m_mutex);
            return backendName;
        });
        m_daemon->addProperty("DistroId", [this] {
            std::lock_guard lock(m_mutex);
            return distroId;
        });
        m_daemon->addProperty("Roles", [this] {
            std::lock_guard lock(m_mutex);
            return roles;
        });
        m_daemon->addProperty("Filters", [] { return ~std::uint64_t(0); });
        m_daemon->addProperty("VersionMajor", [] { return std::uint32_t(1); });
        m_daemon->addMethod("CreateTransaction", "", "o",
                            [this](dbus::MethodCall& c) { c.reply(dbus::ObjectPath{newTransaction()}); });
        m_daemon->addMethod("GetTransactionList", "", "ao", [this](dbus::MethodCall& c) {
            std::vector<dbus::ObjectPath> paths;
            for (const auto& [p, t] : m_transactions)
                if (t->running())
                    paths.push_back({p});
            c.reply(paths);
        });
        for (const char* s : {"InstalledChanged", "UpdatesChanged", "RepoListChanged"})
            m_daemon->addSignal(s, "");
        m_daemon->publish();
    }

    void run(std::function<void()> task) { m_handle.post(std::move(task)); }

    mutable std::mutex m_mutex;
    std::map<std::pair<std::string, bool>, std::deque<Answer>> m_answers;
    std::vector<Call> m_calls;
    int m_cancels = 0;
    int m_active = 0;
    int m_maxActive = 0;
    int m_next = 0;

    std::thread m_thread;
    cutefish::base::EventLoop* m_loop = nullptr;
    cutefish::base::LoopHandle m_handle;
    std::unique_ptr<dbus::Connection> m_connection;
    std::unique_ptr<dbus::Object> m_daemon;
    std::map<std::string, std::unique_ptr<Transaction>> m_transactions;
};

} // namespace fakepk
