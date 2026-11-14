// TransactionManager with two scripted backends: the generic manager, not a
// package manager. The service tests can only have PackageKit today, so a
// parent UpdateAll there has one child; this test covers the part that needs
// two: a partial failure must never become a global success (D-031).

#include "privatebus.h"
#include "transactionmanager.h"

#include <cutefish/store/backend.h>
#include <cutefish/store/plan.h>
#include <cutefish/store/statestore.h>

#include <base/dbus.h>
#include <base/event_loop.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cutefish::store;
using namespace cutefish::store::service;
namespace base = cutefish::base;
namespace dbus = cutefish::base::dbus;
using namespace std::chrono_literals;

namespace {

/// A backend that answers plans at once and can fail its execution, as a
/// second installation would. Test-only: it implements the contract, not a
/// package manager.
class FakeBackend final : public Backend
{
public:
    FakeBackend(base::EventLoop& loop, BackendKind kind, bool fails)
        : m_loop(loop), m_kind(kind), m_fails(fails)
    {
    }

    BackendKind kind() const noexcept override { return m_kind; }

    BackendCapabilities capabilities() const noexcept override
    {
        return {.userScope = m_kind == BackendKind::Flatpak,
                .systemScope = m_kind == BackendKind::PackageKit,
                .permissions = false,
                .refresh = false,
                .cancelAnytime = false,
                .reattach = false,
                .verifiesExecutedPlan = false};
    }

    void availability(Completion<void> done) override { post([done] { done(Result<void>::success()); }); }

    void queryInstalled(Completion<std::vector<InstalledItem>> done) override
    {
        post([done] { done(std::vector<InstalledItem>{}); });
    }

    void sources(Completion<std::vector<BackendSource>> done) override
    {
        post([done] { done(std::vector<BackendSource>{}); });
    }

    void changeSource(const std::string&, const SourceRequest&, Completion<void> done) override
    {
        post([done] { done(Error::notSupported("no source changes in this fake")); });
    }

    void catalogSources(Completion<std::vector<SourceMetadata>> done) override
    {
        post([done] { done(std::vector<SourceMetadata>{}); });
    }

    void queryUpdates(Completion<std::vector<AvailableUpdate>> done) override
    {
        post([done] { done(std::vector<AvailableUpdate>{}); });
    }

    void permissions(const OfferId&, Completion<std::vector<std::string>> done) override
    {
        post([done] { done(Error::notSupported("no permissions in this fake")); });
    }

    void plan(const std::string&, const PlanRequest& request, Completion<Plan> done) override
    {
        auto plan = Plan::create({.kind = request.kind(),
                                  .backend = m_kind,
                                  .scope = request.scope(),
                                  .items = {{.operation = PlanOperation::Update,
                                             .kind = PlanItemKind::Application,
                                             .requested = true,
                                             .package = request.targets().front().package}}});
        if (!plan)
            return post([done, error = plan.error()] { done(Result<Plan>(error)); });
        post([done, value = std::move(plan).value()]() mutable { done(Result<Plan>(std::move(value))); });
    }

    void execute(const std::string&, const PlanRequest&, const Plan& accepted, OperationEvents events,
                 Completion<ExecutionResult> done) override
    {
        post([this, accepted, events = std::move(events), done = std::move(done)]() mutable {
            if (m_fails)
                return done(Error::backend(BackendFailure::AuthDenied, "denied by the fake"));
            if (events.started)
                events.started(accepted);
            if (m_holdExecution) {
                // Running until complete(); progress() reports meanwhile.
                m_events = std::move(events);
                m_done = std::move(done);
                return;
            }
            done(ExecutionResult{.outcome = ExecutionResult::Outcome::Completed});
        });
    }

    // --- Scripting (H.4) ---------------------------------------------------
    /// The execution stays running until complete().
    void holdExecution() { m_holdExecution = true; }
    bool running() const { return static_cast<bool>(m_done); }
    /// Reports progress of the held execution, as the backend would.
    void progress(const Progress& p)
    {
        post([this, p] {
            if (m_events.progress)
                m_events.progress(p);
        });
    }
    void complete()
    {
        post([this] {
            if (auto done = std::exchange(m_done, {}))
                done(ExecutionResult{.outcome = ExecutionResult::Outcome::Completed});
        });
    }

    void reattach(const std::string&, const std::string&, OperationEvents,
                  Completion<ExecutionResult> done) override
    {
        post([done] { done(Error::notSupported("no re-attach in this fake")); });
    }

    void refresh(const std::string&, OperationEvents, Completion<void> done) override
    {
        post([done] { done(Result<void>::success()); });
    }

    Result<void> cancel(const std::string&) override { return Result<void>::success(); }

    void setChangeListener(std::function<void(const BackendChanges&)>) override {}

private:
    void post(std::function<void()> task) { m_loop.post(std::move(task)); }

    base::EventLoop& m_loop;
    BackendKind m_kind;
    bool m_fails;
    bool m_holdExecution = false;
    OperationEvents m_events;
    Completion<ExecutionResult> m_done;
};

class TransactionManagerTest : public ::testing::Test
{
protected:
    struct Fetch
    {
        std::size_t pending = 0;
        bool done = false;
    };

    void SetUp() override
    {
        m_connection = dbus::Connection::open(dbus::Connection::Bus::Session, &m_loop);
        ASSERT_TRUE(m_connection);
        auto state = StateStore::open(m_dir / "state.sqlite", static_cast<UnixSeconds>(std::time(nullptr)));
        ASSERT_TRUE(state) << state.error().message();
        m_state.emplace(std::move(state).value());
        m_manager = std::make_unique<TransactionManager>(
                m_loop, *m_connection, *m_state,
                TransactionManager::Options{.confirmationTimeout = 60s, .retention = 60s});
        m_manager->newPath = [this] { return "/org/cutefish/Store/Transactions/" + std::to_string(++m_paths); };
    }

    bool spin(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 10s)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (true) {
            const std::vector<std::string> paths = m_manager->paths();
            if (!refresh(paths))
                return false;
            if (pred())
                return true;
            if (std::chrono::steady_clock::now() > end)
                return false;
            const int t = m_loop.startTimer(10ms, false, [this] { m_loop.quit(0); });
            m_loop.run();
            m_loop.stopTimer(t);
        }
    }

    /// Runs the loop for a while, for "nothing more happens" checks.
    void settle(std::chrono::milliseconds d = 300ms)
    {
        const auto end = std::chrono::steady_clock::now() + d;
        spin([&] { return std::chrono::steady_clock::now() > end; });
    }

    dbus::Proxy transaction(const std::string& path) const
    {
        return dbus::Proxy(m_connection.get(), m_connection->uniqueName(), path, "org.cutefish.Store1.Transaction");
    }

    /// Reads State, Backend and Error of every path asynchronously: a
    /// synchronous call from the connection that owns the objects would wait
    /// for a loop turn it is itself blocking.
    bool refresh(const std::vector<std::string>& paths)
    {
        auto state = std::make_shared<Fetch>();
        state->pending = paths.size();
        m_fetch = state;
        std::vector<dbus::PendingCall> calls;
        calls.reserve(paths.size());
        for (const std::string& path : paths) {
            calls.push_back(transaction(path).getAllAsync([this, path, state](const dbus::Reply& reply) {
                if (m_fetch != state)
                    return;
                if (const auto all = reply.value<dbus::VariantMap>()) {
                    m_all[path] = *all;
                    if (const auto it = all->find("State"); it != all->end())
                        if (const auto v = it->second.get<std::string>())
                            m_states[path] = *v;
                    if (const auto it = all->find("Backend"); it != all->end())
                        if (const auto v = it->second.get<std::string>())
                            m_backends[path] = *v;
                    if (const auto it = all->find("Error"); it != all->end())
                        if (const auto v = it->second.get<dbus::VariantMap>())
                            m_errors[path] = *v;
                }
                if (--state->pending == 0) {
                    state->done = true;
                    m_loop.quit(0);
                }
            }));
        }
        const int timer = m_loop.startTimer(5s, true, [this, state] {
            if (m_fetch == state) {
                m_fetch.reset();
                m_loop.quit(0);
            }
        });
        m_loop.run();
        m_loop.stopTimer(timer);
        const bool done = state->done;
        if (m_fetch == state)
            m_fetch.reset();
        return done;
    }

    std::string stateOf(const std::string& path) const
    {
        const auto it = m_states.find(path);
        return it == m_states.end() ? std::string("<none>") : it->second;
    }

    std::string backendOf(const std::string& path) const
    {
        const auto it = m_backends.find(path);
        return it == m_backends.end() ? std::string("<none>") : it->second;
    }

    static bool terminal(const std::string& state)
    {
        return state == "succeeded" || state == "failed" || state == "cancelled" || state == "interrupted";
    }

    privatebus::PrivateBus m_bus;
    base::EventLoop m_loop;
    std::unique_ptr<dbus::Connection> m_connection;
    catalogfixture::TempDir m_dir;
    std::optional<StateStore> m_state;
    std::unique_ptr<TransactionManager> m_manager;
    int m_paths = 0;
    std::map<std::string, std::string> m_states;
    std::map<std::string, std::string> m_backends;
    std::map<std::string, dbus::VariantMap> m_errors;
    /// Every property of each path, as last read.
    std::map<std::string, dbus::VariantMap> m_all;
    std::shared_ptr<Fetch> m_fetch;

    template <typename T>
    T prop(const std::string& path, const char* name) const
    {
        const auto it = m_all.find(path);
        if (it == m_all.end() || !it->second.contains(name))
            return T{};
        return it->second.at(name).value<T>();
    }
};

TEST_F(TransactionManagerTest, UpdateAllPartialFailureNeverSucceeds)
{
    FakeBackend packagekit(m_loop, BackendKind::PackageKit, false);
    FakeBackend flatpak(m_loop, BackendKind::Flatpak, true);
    m_manager->addBackend(packagekit);
    m_manager->addBackend(flatpak);

    auto pkOffer = OfferId::parse("packagekit:gimp");
    auto fpOffer = OfferId::parse("flatpak:user:flathub:app/org.example.App/x86_64/stable");
    ASSERT_TRUE(pkOffer);
    ASSERT_TRUE(fpOffer);
    auto parent = m_manager->startUpdateAll({{pkOffer.value(), std::nullopt}, {fpOffer.value(), std::nullopt}});
    ASSERT_TRUE(parent) << parent.error().message();
    const std::string parentPath = parent.value();

    std::vector<std::string> children;
    ASSERT_TRUE(spin([&] {
        children.clear();
        for (const std::string& path : m_manager->paths())
            if (path != parentPath)
                children.push_back(path);
        return children.size() == 2 && terminal(stateOf(parentPath))
               && terminal(stateOf(children[0])) && terminal(stateOf(children[1]));
    }));

    std::map<std::string, std::string> byBackend;
    for (const std::string& child : children)
        byBackend[backendOf(child)] = stateOf(child);
    ASSERT_EQ(byBackend.size(), 2u);
    EXPECT_EQ(byBackend["packagekit"], "succeeded");
    EXPECT_EQ(byBackend["flatpak"], "failed");
    EXPECT_EQ(stateOf(parentPath), "failed"); // not succeeded, despite one child succeeding
    const auto error = m_errors.find(parentPath);
    ASSERT_NE(error, m_errors.end());
    ASSERT_FALSE(error->second.empty());
    EXPECT_EQ(error->second.at("code").value<std::string>(), "auth-denied");

    // The two children are journaled, the parent is not (D-018, D-031).
    auto history = m_state->history(10);
    ASSERT_TRUE(history) << history.error().message();
    ASSERT_EQ(history->size(), 2u);
    EXPECT_NE(std::find_if(history->begin(), history->end(),
                           [](const TransactionRecord& r) { return r.snapshot.state == TransactionState::Failed; }),
              history->end());
    EXPECT_NE(std::find_if(history->begin(), history->end(),
                           [](const TransactionRecord& r) { return r.snapshot.state == TransactionState::Succeeded; }),
              history->end());
}

// --- Aggregated progress of the parent (H.4, D-036) ------------------------------

std::vector<std::string> childrenOf(const std::vector<std::string>& paths, const std::string& parent)
{
    std::vector<std::string> out;
    for (const std::string& p : paths)
        if (p != parent)
            out.push_back(p);
    return out;
}

Progress progress(double fraction, std::uint64_t downloaded, std::uint64_t total)
{
    Progress p;
    p.fraction = fraction;
    p.downloadedBytes = downloaded;
    p.totalBytes = total;
    return p;
}

TEST_F(TransactionManagerTest, AParentWithOneChildShowsItsProgress)
{
    FakeBackend packagekit(m_loop, BackendKind::PackageKit, false);
    packagekit.holdExecution();
    m_manager->addBackend(packagekit);
    auto started = m_manager->startUpdateAll({{OfferId::parse("packagekit:gimp").value(), std::nullopt}});
    ASSERT_TRUE(started) << started.error().message();
    const std::string parent = started.value();
    std::vector<std::string> children;
    ASSERT_TRUE(spin([&] {
        children = childrenOf(m_manager->paths(), parent);
        return children.size() == 1 && stateOf(children[0]) == "running";
    }));
    EXPECT_EQ(prop<double>(parent, "Progress"), -1.0); // nothing told yet

    packagekit.progress(progress(0.5, 100, 400));
    ASSERT_TRUE(spin([&] { return prop<double>(parent, "Progress") == 0.5; }));
    EXPECT_EQ(prop<std::uint64_t>(parent, "DownloadedBytes"), 100u);
    EXPECT_EQ(prop<std::uint64_t>(parent, "TotalBytes"), 400u);
    // Its only child cannot be cancelled now (the fake says so): neither can
    // the parent.
    EXPECT_FALSE(prop<bool>(parent, "Cancellable"));

    packagekit.complete();
    ASSERT_TRUE(spin([&] { return stateOf(parent) == "succeeded"; }));
}

TEST_F(TransactionManagerTest, AParentWithSeveralChildrenNeverInventsAFraction)
{
    // D-036: a deb and a Flatpak are not comparable work. The parent gives
    // no fraction; it adds up the bytes only when every child still open
    // runs and has announced its total.
    FakeBackend packagekit(m_loop, BackendKind::PackageKit, false);
    FakeBackend flatpak(m_loop, BackendKind::Flatpak, false);
    packagekit.holdExecution();
    flatpak.holdExecution();
    m_manager->addBackend(packagekit);
    m_manager->addBackend(flatpak);
    auto started = m_manager->startUpdateAll(
            {{OfferId::parse("packagekit:gimp").value(), std::nullopt},
             {OfferId::parse("flatpak:user:flathub:app/org.example.App/x86_64/stable").value(), std::nullopt}});
    ASSERT_TRUE(started) << started.error().message();
    const std::string parent = started.value();
    std::map<std::string, std::string> byBackend; // backend → path
    ASSERT_TRUE(spin([&] {
        byBackend.clear();
        for (const std::string& c : childrenOf(m_manager->paths(), parent))
            byBackend[backendOf(c)] = c;
        return byBackend.size() == 2 && stateOf(byBackend["packagekit"]) == "running"
               && stateOf(byBackend["flatpak"]) == "running";
    }));
    const std::string pkChild = byBackend["packagekit"];
    const std::string fpChild = byBackend["flatpak"];

    // The Flatpak child does not know its total yet: no byte count.
    packagekit.progress(progress(0.5, 100, 400));
    flatpak.progress(progress(0.25, 50, 0));
    ASSERT_TRUE(spin([&] {
        return prop<double>(pkChild, "Progress") == 0.5 && prop<double>(fpChild, "Progress") == 0.25;
    }));
    settle();
    EXPECT_EQ(prop<double>(parent, "Progress"), -1.0);
    EXPECT_EQ(prop<std::uint64_t>(parent, "TotalBytes"), 0u);
    EXPECT_EQ(prop<std::uint64_t>(parent, "DownloadedBytes"), 0u);

    // Both totals known: the bytes add up; still no fraction.
    flatpak.progress(progress(0.3, 200, 600));
    ASSERT_TRUE(spin([&] { return prop<std::uint64_t>(parent, "TotalBytes") == 1000u; }));
    EXPECT_EQ(prop<std::uint64_t>(parent, "DownloadedBytes"), 300u);
    EXPECT_EQ(prop<double>(parent, "Progress"), -1.0);

    packagekit.complete();
    ASSERT_TRUE(spin([&] { return stateOf(pkChild) == "succeeded"; }));
    EXPECT_EQ(prop<double>(parent, "Progress"), -1.0);
    EXPECT_EQ(stateOf(parent), "running");
    flatpak.complete();
    ASSERT_TRUE(spin([&] { return stateOf(parent) == "succeeded"; }));
    EXPECT_EQ(prop<double>(parent, "Progress"), -1.0);
}

} // namespace
