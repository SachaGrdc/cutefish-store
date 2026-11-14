// Flatpak transactions through the service (H.3, SERVICE.md §23): the service
// in-process with the real Flatpak backend (FlatpakBackend) over a double of
// libflatpak (tests/backend-flatpak/fakestore.h), and, where both backends
// are needed, its real PackageKit backend against a simulated PackageKit
// (fakepackagekit.h), on a private bus. No Flatpak installation, PackageKit,
// APT or polkit is touched: every "installation" is the double's.

#include "fakepackagekit.h"
#include "fakestore.h"
#include "flatpakbackend.h"
#include "pkbackend.h"
#include "storeservice.h"

#include "catalogfixture.h"
#include "privatebus.h"

#include <base/dbus.h>
#include <base/event_loop.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cutefish::store;
using namespace cutefish::store::service;
namespace base = cutefish::base;
namespace dbus = cutefish::base::dbus;
namespace pk = cutefish::store::packagekit;
namespace fp = cutefish::store::flatpak;
using namespace std::chrono_literals;

namespace {

constexpr const char* kStore = "org.cutefish.Store";
constexpr const char* kPath = "/org/cutefish/Store";
constexpr const char* kInterface = "org.cutefish.Store1";
constexpr const char* kTxInterface = "org.cutefish.Store1.Transaction";
constexpr const char* kNotAllowed = "org.cutefish.Store1.Error.NotAllowed";
constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

constexpr const char* kGimpUser = "flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable";
constexpr const char* kGimpSystem = "flatpak:system:flathub:app/org.gimp.GIMP/x86_64/stable";
constexpr const char* kInkscapeUser = "flatpak:user:flathub:app/org.inkscape.Inkscape/x86_64/stable";
constexpr const char* kQgisSystem = "flatpak:system:flathub:app/org.qgis.qgis/x86_64/lts";

fp::FlatpakRef app(const char* name, const char* branch = "stable")
{
    return {fp::RefKind::App, name, "x86_64", branch};
}

/// The catalogue of these tests: Debian, and Flathub in both installations
/// (catalogfixture has Flathub in the user installation only). The updates of
/// the system installation are listed only for applications of its
/// catalogue (D-010, D-023).
void buildCatalogue(const std::filesystem::path& path)
{
    auto writer = fixtures::unwrap(CatalogWriter::create(path, {.generation = 1, .searchLocales = {"fr_FR.UTF-8"}}));
    const Source debian = fixtures::debianSource();
    const Source user = fixtures::flathubSource(Scope::User);
    const Source system = fixtures::flathubSource(Scope::System);
    for (const Source& s : {debian, user, system})
        EXPECT_TRUE(writer.addSource(s));
    EXPECT_TRUE(writer.addComponent(debian.id(), catalogfixture::deb("org.gimp.GIMP", "gimp", "GIMP")));
    for (const Source& s : {user, system}) {
        EXPECT_TRUE(writer.addComponent(s.id(), catalogfixture::flatpak("org.gimp.GIMP", "app/org.gimp.GIMP/x86_64/stable",
                                                                        "GIMP", "", "3.0.4")));
        EXPECT_TRUE(writer.addComponent(s.id(), catalogfixture::flatpak("org.inkscape.Inkscape",
                                                                        "app/org.inkscape.Inkscape/x86_64/stable",
                                                                        "Inkscape")));
        EXPECT_TRUE(writer.addComponent(s.id(), catalogfixture::flatpak("org.qgis.qgis", "app/org.qgis.qgis/x86_64/lts",
                                                                        "QGIS", "", "3.34")));
    }
    EXPECT_TRUE(writer.finish());
}

/// A D-Bus call's outcome: its error name, or its object path.
struct Answer
{
    std::string error;
    std::string path;
};

struct Options
{
    bool packagekit = false;
    std::chrono::milliseconds confirmationTimeout = std::chrono::minutes(10);
};

/// One service instance over a data directory: the bus and the directory
/// outlive it, so that a second instance can follow (restart).
class Instance
{
public:
    Instance(base::EventLoop& loop, dbus::Connection& conn, const std::filesystem::path& dir,
             std::unique_ptr<fp::FakeStore> store, const Options& o)
    {
        ServiceOptions options;
        options.dataDir = dir;
        options.sourcesFile = dir / "sources.conf";
        options.indexer = "cutefish-store-indexer";
        options.readLocales = {"C"};
        options.busName = kStore;
        options.confirmationTimeout = o.confirmationTimeout;
        std::vector<std::unique_ptr<Backend>> backends;
        if (o.packagekit)
            backends.push_back(std::make_unique<PackageKitBackend>(
                    loop, PackageKitBackend::Options{.bus = PkTransport::Bus::Session,
                                                     .swcatalog = dir / "swcatalog",
                                                     .changeDelay = 100ms}));
        store_ = store.get();
        auto post = [&loop](std::function<void()> task) { loop.post(std::move(task)); };
        backends.push_back(std::make_unique<fp::FlatpakBackend>(fp::FlatpakBackend::Options{.post = std::move(post)},
                                                                std::move(store)));
        service = std::make_unique<StoreService>(loop, conn, std::move(options), std::move(backends));
    }

    fp::FakeStore* store_ = nullptr;
    std::unique_ptr<StoreService> service;
};

class FlatpakTransactions : public ::testing::Test
{
protected:
    void SetUp() override
    {
        buildCatalogue(m_dir / "catalog.sqlite");
        m_conn = dbus::Connection::open(dbus::Connection::Bus::Session, &m_loop);
        ASSERT_TRUE(m_conn);
        m_matches.push_back(m_conn->subscribe(rule(kInterface, "InstalledChanged"), [this](const dbus::Message&) {
            ++m_installedChanged;
        }));
        m_matches.push_back(m_conn->subscribe(rule(kInterface, "TransactionAdded"), [this](const dbus::Message&) {
            ++m_added;
        }));
        // Every State a transaction goes through, in order.
        m_matches.push_back(m_conn->subscribe(rule("org.freedesktop.DBus.Properties", "PropertiesChanged"),
                                              [this](const dbus::Message& m) {
                                                  std::string iface;
                                                  dbus::VariantMap changed;
                                                  std::vector<std::string> invalidated;
                                                  if (!m.read(iface, changed, invalidated) || iface != kTxInterface)
                                                      return;
                                                  if (const auto it = changed.find("State"); it != changed.end())
                                                      m_states[m.path()].push_back(it->second.value<std::string>());
                                              }));
    }

    void TearDown() override
    {
        if (m_instance)
            release();
    }

    /// A store with both installations and a Flathub remote in each.
    static std::unique_ptr<fp::FakeStore> newStore()
    {
        auto store = std::make_unique<fp::FakeStore>();
        for (const Scope scope : {Scope::User, Scope::System})
            store->remotes_[scope] = {{.name = "flathub",
                                       .url = "https://dl.flathub.org/repo/",
                                       .disabled = false,
                                       .gpgVerify = true,
                                       .metadataPath = {},
                                       .iconsPath = {}}};
        store->applyCompleted = true;
        return store;
    }

    /// Starts an instance; `script` sets up its store before the backend
    /// first uses it.
    void start(const std::function<void(fp::FakeStore&)>& script = {}, Options options = {})
    {
        auto store = newStore();
        if (script)
            script(*store);
        m_instance = std::make_unique<Instance>(m_loop, *m_conn, m_dir.path(), std::move(store), options);
        m_store = m_instance->store_;
        ASSERT_TRUE(m_instance->service->start());
        const std::size_t expected = options.packagekit ? 2 : 1;
        ASSERT_TRUE(spin([&] {
            const auto b = backends();
            if (b.size() != expected)
                return false;
            for (const auto& entry : b)
                if (!entry.at("available").value<bool>(false))
                    return false;
            return property<std::string>("State") == "ready";
        })) << "the service did not become ready";
    }

    /// The instance goes away as on a crash or an idle exit: destroyed, then
    /// its name released (main.cpp).
    void release()
    {
        m_instance.reset();
        m_store = nullptr;
        m_conn->releaseName(kStore);
    }

    static dbus::MatchRule rule(const char* interface, const char* member)
    {
        dbus::MatchRule r;
        r.interface = interface;
        r.member = member;
        return r;
    }

    bool spin(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 10s)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            const int t = m_loop.startTimer(5ms, false, [this] { m_loop.quit(0); });
            m_loop.run();
            m_loop.stopTimer(t);
        }
        return true;
    }

    /// Runs the loop for a while, for "nothing more happens" checks.
    void settle(std::chrono::milliseconds d = 150ms)
    {
        const auto end = std::chrono::steady_clock::now() + d;
        spin([&] { return std::chrono::steady_clock::now() > end; });
    }

    template <typename... Args>
    Answer call(const std::string& path, const char* interface, const char* method, Args&&... args)
    {
        auto out = std::make_shared<Answer>();
        auto done = std::make_shared<bool>(false);
        std::vector<dbus::PendingCall> calls;
        calls.push_back(dbus::Proxy(m_conn.get(), kStore, path, interface)
                                .callAsync(method, [out, done](const dbus::Reply& r) {
                                    if (r.isError()) {
                                        out->error = r.error().name;
                                    } else {
                                        dbus::ObjectPath p;
                                        if (r.read(p))
                                            out->path = p.path;
                                    }
                                    *done = true;
                                }, std::forward<Args>(args)...));
        spin([&] { return *done; });
        return *out;
    }

    template <typename T>
    std::optional<T> value(const std::string& method, auto&&... args)
    {
        auto out = std::make_shared<std::optional<T>>();
        auto done = std::make_shared<bool>(false);
        std::vector<dbus::PendingCall> calls;
        calls.push_back(dbus::Proxy(m_conn.get(), kStore, kPath, kInterface)
                                .callAsync(method, [out, done](const dbus::Reply& r) {
                                    if (!r.isError())
                                        *out = r.value<T>();
                                    *done = true;
                                }, args...));
        spin([&] { return *done; });
        return *out;
    }

    std::optional<dbus::VariantMap> getAll(const std::string& path, const char* interface)
    {
        auto out = std::make_shared<std::optional<dbus::VariantMap>>();
        auto done = std::make_shared<bool>(false);
        std::vector<dbus::PendingCall> calls;
        calls.push_back(dbus::Proxy(m_conn.get(), kStore, path, interface).getAllAsync([out, done](const dbus::Reply& r) {
            *out = r.value<dbus::VariantMap>();
            *done = true;
        }));
        spin([&] { return *done; });
        return *out;
    }

    template <typename T>
    T property(const char* name)
    {
        auto p = getAll(kPath, kInterface);
        return p && p->contains(name) ? p->at(name).value<T>() : T{};
    }

    std::vector<dbus::VariantMap> backends() { return property<std::vector<dbus::VariantMap>>("Backends"); }

    dbus::VariantMap backend(const std::string& id)
    {
        for (auto& b : backends())
            if (b.at("id").value<std::string>() == id)
                return b;
        return {};
    }

    Answer install(const std::string& offer) { return call(kPath, kInterface, "Install", offer, dbus::VariantMap{}); }
    Answer remove(const std::string& offer) { return call(kPath, kInterface, "Remove", offer, dbus::VariantMap{}); }
    Answer update(const std::vector<std::string>& offers)
    {
        return call(kPath, kInterface, "Update", offers, dbus::VariantMap{});
    }
    std::string confirm(const std::string& t) { return call(t, kTxInterface, "Confirm").error; }
    std::string cancel(const std::string& t) { return call(t, kTxInterface, "Cancel").error; }

    dbus::VariantMap transaction(const std::string& t) { return getAll(t, kTxInterface).value_or(dbus::VariantMap{}); }

    std::string state(const std::string& t)
    {
        const auto p = transaction(t);
        return p.contains("State") ? p.at("State").value<std::string>() : std::string();
    }

    std::string errorCode(const std::string& t)
    {
        const auto p = transaction(t);
        if (!p.contains("Error"))
            return {};
        const auto e = p.at("Error").value<dbus::VariantMap>();
        return e.contains("code") ? e.at("code").value<std::string>() : std::string();
    }

    bool waitState(const std::string& t, const std::string& wanted)
    {
        return spin([&] { return state(t) == wanted; });
    }

    dbus::VariantMap plan(const std::string& t)
    {
        const auto p = transaction(t);
        return p.contains("Plan") ? p.at("Plan").value<dbus::VariantMap>() : dbus::VariantMap{};
    }

    static std::vector<std::string> strings(const dbus::VariantMap& m, const char* key)
    {
        return m.contains(key) ? m.at(key).value<std::vector<std::string>>() : std::vector<std::string>{};
    }

    static bool has(const std::vector<std::string>& list, const std::string& v)
    {
        return std::find(list.begin(), list.end(), v) != list.end();
    }

    std::vector<std::string> installedOffers()
    {
        std::vector<std::string> out;
        for (const auto& i : value<std::vector<dbus::VariantMap>>("GetInstalled", dbus::VariantMap{}).value_or(
                     std::vector<dbus::VariantMap>{}))
            out.push_back(i.at("offer-id").value<std::string>());
        return out;
    }

    std::vector<std::string> updateOffers()
    {
        std::vector<std::string> out;
        for (const auto& u : value<std::vector<dbus::VariantMap>>("GetUpdates").value_or(std::vector<dbus::VariantMap>{}))
            out.push_back(u.at("offer-id").value<std::string>());
        return out;
    }

    std::vector<dbus::VariantMap> history()
    {
        return value<std::vector<dbus::VariantMap>>("GetTransactionHistory", std::uint32_t(50))
                .value_or(std::vector<dbus::VariantMap>{});
    }

    /// The journal entry of a transaction (by its Id).
    dbus::VariantMap journal(const std::string& t)
    {
        const auto id = transaction(t).at("Id").value<std::string>();
        for (auto& h : history())
            if (h.at("id").value<std::string>() == id)
                return h;
        return {};
    }

    const std::vector<std::string>& states(const std::string& t) { return m_states[t]; }

    privatebus::PrivateBus m_bus;
    base::EventLoop m_loop;
    std::unique_ptr<dbus::Connection> m_conn;
    catalogfixture::TempDir m_dir;
    std::unique_ptr<Instance> m_instance;
    fp::FakeStore* m_store = nullptr;
    std::vector<dbus::Match> m_matches;
    std::map<std::string, std::vector<std::string>> m_states;
    int m_installedChanged = 0;
    int m_added = 0;
};

// --- Install, Remove, Update in each installation ------------------------------

TEST_F(FlatpakTransactions, AUserInstallRunsBothPassesAndIsRecorded)
{
    start();
    const Answer a = install(kGimpUser);
    ASSERT_TRUE(a.error.empty()) << a.error;
    // The application brings permissions: a confirmation (P-008).
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    const auto p = plan(a.path);
    EXPECT_EQ(p.at("backend").value<std::string>(), "flatpak");
    EXPECT_EQ(p.at("scope").value<std::string>(), "user");
    EXPECT_TRUE(has(strings(p, "reasons"), "new-permissions"));
    EXPECT_FALSE(has(strings(p, "reasons"), "system-scope"));
    const auto ops = p.at("operations").value<std::vector<dbus::VariantMap>>();
    ASSERT_EQ(ops.size(), 2u); // the application, and the runtime Flatpak adds
    EXPECT_EQ(ops[0].at("package").value<std::string>(), "app/org.gimp.GIMP/x86_64/stable");
    EXPECT_TRUE(ops[0].at("requested").value<bool>(false));
    EXPECT_EQ(ops[0].at("origin").value<std::string>(), "flathub");
    EXPECT_EQ(ops[1].at("package").value<std::string>(), "runtime/org.example.Platform/x86_64/24.08");
    EXPECT_FALSE(ops[1].at("requested").value<bool>(true));
    EXPECT_EQ(p.at("download-size").value<std::uint64_t>(), 1500u);
    EXPECT_EQ(m_store->resolves.load(), 1);
    EXPECT_EQ(m_store->executes.load(), 0);

    EXPECT_EQ(confirm(a.path), "");
    ASSERT_TRUE(waitState(a.path, "succeeded"));
    // Second pass checked in `ready`, then run once (D-017).
    EXPECT_EQ(m_store->executes.load(), 1);
    EXPECT_EQ(m_store->runs.load(), 1);
    EXPECT_TRUE(has(states(a.path), "running"));
    const auto tx = transaction(a.path);
    EXPECT_EQ(tx.at("Backend").value<std::string>(), "flatpak");
    EXPECT_EQ(tx.at("AppIds").value<std::vector<std::string>>(), std::vector<std::string>{"org.gimp.GIMP"});
    // In the journal with its installation.
    const auto j = journal(a.path);
    EXPECT_EQ(j.at("state").value<std::string>(), "succeeded");
    EXPECT_EQ(j.at("backend").value<std::string>(), "flatpak");
    EXPECT_EQ(j.at("scope").value<std::string>(), "user");
    // The installed state is asked again, never assumed (§8.3).
    ASSERT_TRUE(spin([&] { return has(installedOffers(), kGimpUser); }));
    EXPECT_GE(m_installedChanged, 1);
}

TEST_F(FlatpakTransactions, ANewSystemInstallIsRefusedBeforeAnyTransaction)
{
    start();
    const int added = m_added;
    const Answer a = install(kGimpSystem);
    EXPECT_EQ(a.error, kNotAllowed);
    EXPECT_TRUE(a.path.empty());
    settle();
    EXPECT_EQ(m_added, added);
    EXPECT_EQ(m_store->resolves.load(), 0);
    EXPECT_EQ(m_store->executes.load(), 0);
    EXPECT_TRUE(history().empty());
    // The user installation of the same application is accepted.
    const Answer user = install(kGimpUser);
    EXPECT_TRUE(user.error.empty()) << user.error;
}

TEST_F(FlatpakTransactions, ASystemRemovalAsksForTheSystemScopeConfirmation)
{
    start([](fp::FakeStore& s) { s.installed_[Scope::System] = {{app("org.gimp.GIMP"), "flathub"}}; });
    ASSERT_TRUE(spin([&] { return has(installedOffers(), kGimpSystem); }));
    const Answer a = remove(kGimpSystem);
    ASSERT_TRUE(a.error.empty()) << a.error;
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    const auto p = plan(a.path);
    EXPECT_EQ(p.at("scope").value<std::string>(), "system");
    EXPECT_TRUE(has(strings(p, "reasons"), "system-scope")); // D-011, even without a polkit dialog
    const auto ops = p.at("operations").value<std::vector<dbus::VariantMap>>();
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_TRUE(has(strings(ops[0], "reasons"), "system-scope"));
    EXPECT_EQ(m_store->executes.load(), 0);
    EXPECT_EQ(confirm(a.path), "");
    ASSERT_TRUE(waitState(a.path, "succeeded"));
    EXPECT_EQ(journal(a.path).at("scope").value<std::string>(), "system");
    ASSERT_TRUE(spin([&] { return !has(installedOffers(), kGimpSystem); }));
}

TEST_F(FlatpakTransactions, ASystemUpdateThatInstallsARuntimeShowsIt)
{
    // D-014: allowed, in the plan, with a confirmation on that runtime.
    start([](fp::FakeStore& s) {
        s.installed_[Scope::System] = {{app("org.qgis.qgis", "lts"), "flathub"}};
        s.updates_[Scope::System] = {{app("org.qgis.qgis", "lts"), "flathub", "c2"}};
        fp::FlatpakOp update{.type = fp::OperationType::Update,
                             .ref = app("org.qgis.qgis", "lts"),
                             .remote = "flathub",
                             .commit = "c2",
                             .downloadSize = 3000,
                             .installedSize = 0,
                             .permissions = {"shared=network"},
                             .oldPermissions = {"shared=network"},
                             .requested = true,
                             .addsSource = false,
                             .rebaseFrom = ""};
        fp::FlatpakOp runtime{.type = fp::OperationType::Install,
                              .ref = {fp::RefKind::Runtime, "org.kde.Platform", "x86_64", "6.8"},
                              .remote = "flathub",
                              .commit = "r1",
                              .downloadSize = 9000,
                              .installedSize = 0,
                              .permissions = {},
                              .oldPermissions = {},
                              .requested = false,
                              .addsSource = false,
                              .rebaseFrom = ""};
        s.resolveOps = std::vector<fp::FlatpakOp>{update, runtime};
        s.executeOps = s.resolveOps;
    });
    ASSERT_TRUE(spin([&] { return has(updateOffers(), kQgisSystem); }));
    const Answer a = update({kQgisSystem});
    ASSERT_TRUE(a.error.empty()) << a.error;
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    const auto p = plan(a.path);
    EXPECT_TRUE(has(strings(p, "reasons"), "system-scope"));
    const auto ops = p.at("operations").value<std::vector<dbus::VariantMap>>();
    ASSERT_EQ(ops.size(), 2u);
    // The runtime is what asks for it, not the update of the application.
    for (const auto& op : ops) {
        const bool isRuntime = op.at("package").value<std::string>() == "runtime/org.kde.Platform/x86_64/6.8";
        EXPECT_EQ(has(strings(op, "reasons"), "system-scope"), isRuntime) << op.at("package").value<std::string>();
    }
    EXPECT_EQ(confirm(a.path), "");
    ASSERT_TRUE(waitState(a.path, "succeeded"));
    ASSERT_TRUE(spin([&] { return !has(updateOffers(), kQgisSystem); }));
}

TEST_F(FlatpakTransactions, AnUpdateAcrossBothBackendsIsRefused)
{
    // Decision C of H.3: one backend and one installation per Update; several
    // backends are UpdateAll's (H.4).
    start([](fp::FakeStore& s) {
        s.installed_[Scope::User] = {{app("org.gimp.GIMP"), "flathub"}};
        s.updates_[Scope::User] = {{app("org.gimp.GIMP"), "flathub", "c2"}};
        s.installed_[Scope::System] = {{app("org.qgis.qgis", "lts"), "flathub"}};
        s.updates_[Scope::System] = {{app("org.qgis.qgis", "lts"), "flathub", "c2"}};
    });
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 2; }));
    const int added = m_added;
    const Answer a = update({kGimpUser, kQgisSystem});
    EXPECT_EQ(a.error, kInvalidArgs);
    settle();
    EXPECT_EQ(m_added, added);
    EXPECT_EQ(m_store->resolves.load(), 0);
}

// --- Second pass, refusals and errors ------------------------------------------

TEST_F(FlatpakTransactions, APlanThatChangedAtTheSecondPassIsNeverExecuted)
{
    start();
    const Answer a = install(kGimpUser);
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    m_store->planChanged = true; // the second pass would also remove a runtime
    EXPECT_EQ(confirm(a.path), "");
    ASSERT_TRUE(spin([&] { return m_store->executes.load() == 1 && state(a.path) == "awaiting-confirmation"; }));
    EXPECT_TRUE(has(strings(plan(a.path), "reasons"), "plan-changed"));
    EXPECT_EQ(m_store->runs.load(), 0); // nothing ran
    EXPECT_FALSE(has(states(a.path), "running"));
    EXPECT_EQ(cancel(a.path), "");
    ASSERT_TRUE(waitState(a.path, "cancelled"));
    EXPECT_EQ(journal(a.path).at("state").value<std::string>(), "cancelled");
    EXPECT_FALSE(has(installedOffers(), kGimpUser));
}

TEST_F(FlatpakTransactions, ATransactionThatWouldAddARemoteFailsNotAllowed)
{
    // While planning (P-011, G.1).
    start([](fp::FakeStore& s) { s.remoteAddAttempted = true; });
    const Answer a = install(kGimpUser);
    ASSERT_TRUE(waitState(a.path, "failed"));
    EXPECT_EQ(errorCode(a.path), "not-allowed");
    EXPECT_EQ(m_store->executes.load(), 0);
    EXPECT_EQ(m_store->remotes_[Scope::User].size(), 1u); // nothing configured
}

TEST_F(FlatpakTransactions, ARemoteAdditionAtTheSecondPassFailsWithoutRunning)
{
    start();
    const Answer a = install(kGimpUser);
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    m_store->remoteAddAttempted = true;
    EXPECT_EQ(confirm(a.path), "");
    ASSERT_TRUE(waitState(a.path, "failed"));
    EXPECT_EQ(errorCode(a.path), "not-allowed");
    EXPECT_EQ(m_store->runs.load(), 0);
    EXPECT_FALSE(has(states(a.path), "running"));
    EXPECT_EQ(journal(a.path).at("state").value<std::string>(), "failed");
}

TEST_F(FlatpakTransactions, AnAuthorizationRefusedByPolkitIsReported)
{
    // flatpak-system-helper → polkit says no: the store reports it, never
    // retries or works around it (P-007).
    start([](fp::FakeStore& s) {
        s.installed_[Scope::System] = {{app("org.gimp.GIMP"), "flathub"}};
        s.runError = Error::backend(BackendFailure::AuthDenied, "not authorized");
    });
    ASSERT_TRUE(spin([&] { return has(installedOffers(), kGimpSystem); }));
    const Answer a = remove(kGimpSystem);
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    EXPECT_EQ(confirm(a.path), "");
    ASSERT_TRUE(waitState(a.path, "failed"));
    EXPECT_EQ(errorCode(a.path), "auth-denied");
    EXPECT_EQ(m_store->executes.load(), 1);
    EXPECT_EQ(journal(a.path).at("state").value<std::string>(), "failed");
    // Still installed: the state is asked again, not assumed.
    settle();
    EXPECT_TRUE(has(installedOffers(), kGimpSystem));
}

// --- Cancellation --------------------------------------------------------------

TEST_F(FlatpakTransactions, ACancellationDuringTheSecondPassRunsNothing)
{
    start();
    const Answer a = install(kGimpUser);
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    m_store->beforeReady.close();
    EXPECT_EQ(confirm(a.path), "");
    ASSERT_TRUE(spin([&] { return m_store->beforeReady.reached() == 1; }));
    EXPECT_EQ(state(a.path), "planning");
    EXPECT_EQ(cancel(a.path), "");
    m_store->beforeReady.open();
    ASSERT_TRUE(waitState(a.path, "cancelled"));
    EXPECT_EQ(m_store->runs.load(), 0);
    EXPECT_FALSE(has(states(a.path), "running"));
    EXPECT_EQ(journal(a.path).at("state").value<std::string>(), "cancelled");
    EXPECT_FALSE(has(installedOffers(), kGimpUser));
}

TEST_F(FlatpakTransactions, ARunningTransactionIsCancelledAtItsNextStep)
{
    start();
    const Answer a = install(kGimpUser);
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    m_store->whileRunning.close();
    EXPECT_EQ(confirm(a.path), "");
    ASSERT_TRUE(waitState(a.path, "running"));
    EXPECT_TRUE(transaction(a.path).at("Cancellable").value<bool>(false));
    EXPECT_EQ(cancel(a.path), ""); // cancelAnytime: the GCancellable is honoured
    ASSERT_TRUE(waitState(a.path, "cancelled"));
    EXPECT_EQ(m_store->cancels.load(), 1);
    EXPECT_EQ(journal(a.path).at("state").value<std::string>(), "cancelled");
    m_store->whileRunning.open();
}

TEST_F(FlatpakTransactions, AnUnansweredConfirmationExpires)
{
    start({}, Options{.confirmationTimeout = 300ms}); // D-019 is 10 minutes
    const Answer a = install(kGimpUser);
    ASSERT_TRUE(waitState(a.path, "cancelled"));
    EXPECT_EQ(m_store->executes.load(), 0);
}

// --- One lane for Flatpak, shared by both installations ------------------------

TEST_F(FlatpakTransactions, UserAndSystemTransactionsShareOneLane)
{
    // Decision B of H.3: one modifying operation at a time on the Flatpak
    // backend, whatever the installation.
    start([](fp::FakeStore& s) { s.installed_[Scope::System] = {{app("org.qgis.qgis", "lts"), "flathub"}}; });
    ASSERT_TRUE(spin([&] { return has(installedOffers(), kQgisSystem); }));
    const Answer user = install(kGimpUser);
    ASSERT_TRUE(waitState(user.path, "awaiting-confirmation"));
    // Awaiting a confirmation gives the lane up (D-030): the other plans.
    const Answer system = remove(kQgisSystem);
    ASSERT_TRUE(waitState(system.path, "awaiting-confirmation"));
    EXPECT_EQ(m_store->resolves.load(), 2);

    m_store->whileRunning.close();
    EXPECT_EQ(confirm(user.path), "");
    ASSERT_TRUE(waitState(user.path, "running"));
    EXPECT_EQ(confirm(system.path), "");
    // Confirmed, it waits its turn in planning: no second execution starts.
    settle(300ms);
    EXPECT_EQ(state(system.path), "planning");
    EXPECT_EQ(m_store->executes.load(), 1);

    m_store->whileRunning.open();
    ASSERT_TRUE(waitState(user.path, "succeeded"));
    ASSERT_TRUE(waitState(system.path, "succeeded"));
    EXPECT_EQ(m_store->executes.load(), 2);
    EXPECT_EQ(m_store->whileRunning.reached(), 2); // one after the other
}

TEST_F(FlatpakTransactions, TwoTransactionsOnOneOfferAreRefusedBusy)
{
    start();
    const Answer a = install(kGimpUser);
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    const Answer again = install(kGimpUser);
    EXPECT_EQ(again.error, "org.cutefish.Store1.Error.Busy");
    // Another offer of the same application is another installation.
    const Answer other = install(kInkscapeUser);
    EXPECT_TRUE(other.error.empty()) << other.error;
}

// --- Restart -------------------------------------------------------------------

TEST_F(FlatpakTransactions, AfterARestartAFlatpakTransactionIsInterruptedNeverReattached)
{
    // A Flatpak transaction runs inside the service: when the service stops
    // during it, its outcome cannot be followed (reattach is false). The
    // next instance marks it interrupted (§5.6); one that awaited a
    // confirmation is cancelled. Nothing is executed again.
    start([](fp::FakeStore& s) { s.installed_[Scope::System] = {{app("org.qgis.qgis", "lts"), "flathub"}}; });
    ASSERT_TRUE(spin([&] { return has(installedOffers(), kQgisSystem); }));
    const Answer running = install(kGimpUser);
    ASSERT_TRUE(waitState(running.path, "awaiting-confirmation"));
    const Answer awaiting = remove(kQgisSystem);
    ASSERT_TRUE(waitState(awaiting.path, "awaiting-confirmation"));
    m_store->whileRunning.close();
    EXPECT_EQ(confirm(running.path), "");
    ASSERT_TRUE(waitState(running.path, "running"));
    const std::string runningId = transaction(running.path).at("Id").value<std::string>();
    const std::string awaitingId = transaction(awaiting.path).at("Id").value<std::string>();
    // The service goes away mid-transaction; its worker is let go so that the
    // backend can be destroyed, but nothing it posts is delivered any more.
    m_store->whileRunning.open();
    release();

    start();
    EXPECT_EQ(m_store->executes.load(), 0);
    std::map<std::string, std::string> byId;
    ASSERT_TRUE(spin([&] {
        byId.clear();
        for (auto& h : history())
            byId[h.at("id").value<std::string>()] = h.at("state").value<std::string>();
        return byId[runningId] == "interrupted" && byId[awaitingId] == "cancelled";
    })) << "running: " << byId[runningId] << ", awaiting: " << byId[awaitingId];
    settle();
    EXPECT_EQ(m_store->executes.load(), 0);
    EXPECT_EQ(m_store->resolves.load(), 0);
}

// --- With PackageKit -----------------------------------------------------------

pk::PkPackage pkg(pk::Info info, const std::string& id)
{
    return {static_cast<std::uint32_t>(info), id, ""};
}

constexpr const char* kGimpDeb = "gimp;3.0.4-3+deb13u11;amd64;debian-stable-main";

class WithPackageKit : public FlatpakTransactions
{
protected:
    void SetUp() override
    {
        FlatpakTransactions::SetUp();
        fakepk::Answer resolve;
        resolve.packages = {pkg(pk::Info::Available, kGimpDeb)};
        m_pk.answer("Resolve", resolve);
        fakepk::Answer sim;
        sim.packages = {pkg(pk::Info::Installing, kGimpDeb)};
        m_pk.answer("InstallPackages", sim, true);
    }

    int realInstalls()
    {
        int n = 0;
        for (const auto& c : m_pk.calls())
            n += c.method == "InstallPackages" && !c.simulate();
        return n;
    }

    fakepk::FakePackageKit m_pk;
};

TEST_F(WithPackageKit, PackageKitAndFlatpakRunAtTheSameTime)
{
    // P-009 asks no more than one lane per backend; the two backends never
    // wait for each other.
    fakepk::Answer real;
    real.hold = true;
    real.steps = {{pk::Status::Install, 50, true}};
    m_pk.answer("InstallPackages", real, false);
    start({}, Options{.packagekit = true});

    const Answer flatpak = install(kGimpUser);
    ASSERT_TRUE(waitState(flatpak.path, "awaiting-confirmation"));
    m_store->whileRunning.close();
    EXPECT_EQ(confirm(flatpak.path), "");
    ASSERT_TRUE(waitState(flatpak.path, "running"));

    const Answer deb = install("packagekit:gimp");
    ASSERT_TRUE(deb.error.empty()) << deb.error;
    ASSERT_TRUE(waitState(deb.path, "running")); // not held behind Flatpak
    EXPECT_EQ(state(flatpak.path), "running");
    EXPECT_EQ(realInstalls(), 1);

    // Each ends on its own.
    std::vector<std::string> held;
    ASSERT_TRUE(spin([&] {
        held = m_pk.runningTransactions();
        return held.size() == 1;
    }));
    ASSERT_TRUE(m_pk.finish(held.front()));
    ASSERT_TRUE(waitState(deb.path, "succeeded"));
    EXPECT_EQ(state(flatpak.path), "running");
    m_store->whileRunning.open();
    ASSERT_TRUE(waitState(flatpak.path, "succeeded"));
    EXPECT_EQ(journal(deb.path).at("backend").value<std::string>(), "packagekit");
    EXPECT_EQ(journal(flatpak.path).at("backend").value<std::string>(), "flatpak");
}

TEST_F(WithPackageKit, ASystemUpdateFromSettingsDoesNotHoldFlatpak)
{
    // H.2 coexistence (§22), with the real Flatpak backend: deb actions are
    // locked, Flatpak runs to its end.
    start({}, Options{.packagekit = true});
    fakepk::Answer held;
    held.hold = true;
    held.steps = {{pk::Status::Install, 30, false}};
    const std::string external = m_pk.startExternal("UpdatePackages", held);
    ASSERT_TRUE(spin([&] {
        const auto b = backend("packagekit");
        return b.contains("activity") && b.at("activity").value<std::string>() == "changing";
    }));
    const Answer deb = install("packagekit:gimp");
    ASSERT_TRUE(waitState(deb.path, "failed"));
    EXPECT_EQ(errorCode(deb.path), "locked");

    const Answer flatpak = install(kGimpUser);
    ASSERT_TRUE(waitState(flatpak.path, "awaiting-confirmation"));
    EXPECT_EQ(confirm(flatpak.path), "");
    ASSERT_TRUE(waitState(flatpak.path, "succeeded"));
    EXPECT_EQ(m_pk.cancels(), 0); // the system update is never touched
    EXPECT_EQ(realInstalls(), 0);
    ASSERT_TRUE(m_pk.finish(external));
    EXPECT_EQ(property<std::string>("State"), "ready");
}

} // namespace
