// Flatpak transactions through the service (H.3, SERVICE.md §23), UpdateAll
// over every backend and installation (H.4, §24), and the sources and their
// changes (H.5, §25): the service
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
                                                  if (const auto it = changed.find("Progress"); it != changed.end())
                                                      m_progress[m.path()].push_back(it->second.value<double>());
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
    Answer updateAll() { return call(kPath, kInterface, "UpdateAll", dbus::VariantMap{}); }
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

    /// The Children of an update-all parent (D-036), in order.
    std::vector<std::string> children(const std::string& t)
    {
        std::vector<std::string> out;
        const auto p = transaction(t);
        if (p.contains("Children"))
            for (const auto& c : p.at("Children").value<std::vector<dbus::ObjectPath>>())
                out.push_back(c.path);
        return out;
    }

    std::vector<dbus::VariantMap> skipped(const std::string& t)
    {
        const auto p = transaction(t);
        return p.contains("Skipped") ? p.at("Skipped").value<std::vector<dbus::VariantMap>>()
                                     : std::vector<dbus::VariantMap>{};
    }

    template <typename T>
    T txProperty(const std::string& t, const char* name)
    {
        const auto p = transaction(t);
        return p.contains(name) ? p.at(name).value<T>() : T{};
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
    std::map<std::string, std::vector<double>> m_progress;
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

TEST_F(FlatpakTransactions, AnUpdateAcrossBothInstallationsIsRefused)
{
    // A request spans one backend and one installation (PlanRequest); the
    // user and system installations together are UpdateAll's (D-031).
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

TEST_F(WithPackageKit, AnUpdateAcrossBothBackendsIsRefused)
{
    // Decision C of H.3: an Update names one backend; UpdateAll (H.4) is for
    // several.
    fakepk::Answer installedDeb;
    installedDeb.packages = {pkg(pk::Info::Installed, "gimp;3.0.4-3;amd64;installed:debian-stable-main")};
    m_pk.answer("GetPackages", installedDeb);
    fakepk::Answer updates;
    updates.packages = {pkg(pk::Info::Security, "gimp;3.0.4-4;amd64;debian-stable-security")};
    m_pk.answer("GetUpdates", updates);
    start([](fp::FakeStore& s) {
        s.installed_[Scope::User] = {{app("org.gimp.GIMP"), "flathub"}};
        s.updates_[Scope::User] = {{app("org.gimp.GIMP"), "flathub", "c2"}};
    }, Options{.packagekit = true});
    ASSERT_TRUE(spin([&] { return has(updateOffers(), "packagekit:gimp") && has(updateOffers(), kGimpUser); }));
    const int added = m_added;
    const Answer a = update({"packagekit:gimp", kGimpUser});
    EXPECT_EQ(a.error, kInvalidArgs);
    settle();
    EXPECT_EQ(m_added, added);
    EXPECT_EQ(m_store->resolves.load(), 0);
    for (const auto& c : m_pk.calls())
        EXPECT_NE(c.method, "UpdatePackages");
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

// --- UpdateAll over every backend and installation (H.4) ------------------------
//
// D-031 (one child per backend and installation), and the H.4 decisions:
// D-036 (Children, Skipped, aggregated Plan, Progress and Cancellable),
// D-037 (one confirmation once every plan is known), D-038 (a locked
// PackageKit fails its child only).

constexpr const char* kBusy = "org.cutefish.Store1.Error.Busy";
constexpr const char* kGimpDebInstalled = "gimp;3.0.4-3;amd64;installed:debian-stable-main";
constexpr const char* kGimpDebUpdate = "gimp;3.0.4-4;amd64;debian-stable-security";
constexpr const char* kLibDebInstalled = "libgimp-old;1.0;amd64;installed:debian-stable-main";

/// GIMP and Inkscape in the user installation, QGIS in the system one, each
/// with an update.
void flatpakUpdates(fp::FakeStore& s, bool system = true, bool inkscape = false)
{
    s.installed_[Scope::User] = {{app("org.gimp.GIMP"), "flathub"}};
    s.updates_[Scope::User] = {{app("org.gimp.GIMP"), "flathub", "c2"}};
    if (inkscape) {
        s.installed_[Scope::User].push_back({app("org.inkscape.Inkscape"), "flathub"});
        s.updates_[Scope::User].push_back({app("org.inkscape.Inkscape"), "flathub", "c2"});
    }
    if (system) {
        s.installed_[Scope::System] = {{app("org.qgis.qgis", "lts"), "flathub"}};
        s.updates_[Scope::System] = {{app("org.qgis.qgis", "lts"), "flathub", "c2"}};
    }
}

TEST_F(FlatpakTransactions, UpdateAllLeavesOutWhatIsAlreadyEngagedAndSaysWhy)
{
    // D-036, decision A: an offer another transaction engages is left out of
    // UpdateAll, and listed in Skipped with the transaction that holds it.
    start([](fp::FakeStore& s) { flatpakUpdates(s, false, true); });
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 2; }));
    const Answer gimp = update({kGimpUser});
    ASSERT_TRUE(gimp.error.empty()) << gimp.error;
    ASSERT_TRUE(waitState(gimp.path, "awaiting-confirmation"));

    const Answer all = updateAll();
    ASSERT_TRUE(all.error.empty()) << all.error;
    const auto left = skipped(all.path);
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].at("offer-id").value<std::string>(), kGimpUser);
    EXPECT_EQ(left[0].at("app-id").value<std::string>(), "org.gimp.GIMP");
    EXPECT_EQ(left[0].at("reason").value<std::string>(), "engaged");
    EXPECT_EQ(left[0].at("transaction").value<dbus::ObjectPath>().path, gimp.path);
    EXPECT_EQ(strings(transaction(all.path), "OfferIds"), std::vector<std::string>{kInkscapeUser});
    const auto kids = children(all.path);
    ASSERT_EQ(kids.size(), 1u);
    EXPECT_EQ(strings(transaction(kids[0]), "OfferIds"), std::vector<std::string>{kInkscapeUser});

    // The two go on separately; the skipped offer stays with its own
    // transaction.
    ASSERT_TRUE(waitState(all.path, "awaiting-confirmation"));
    EXPECT_EQ(confirm(all.path), "");
    ASSERT_TRUE(waitState(all.path, "succeeded"));
    EXPECT_EQ(state(gimp.path), "awaiting-confirmation");
    EXPECT_EQ(cancel(gimp.path), "");
    ASSERT_TRUE(waitState(gimp.path, "cancelled"));
    EXPECT_EQ(m_store->runs.load(), 1);
}

TEST_F(FlatpakTransactions, UpdateAllIsBusyOnlyWhenEveryUpdateIsEngaged)
{
    start([](fp::FakeStore& s) { flatpakUpdates(s, false, true); });
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 2; }));
    const Answer both = update({kGimpUser, kInkscapeUser});
    ASSERT_TRUE(waitState(both.path, "awaiting-confirmation"));
    const int added = m_added;
    const std::size_t journaled = history().size();
    EXPECT_EQ(updateAll().error, kBusy);
    settle();
    EXPECT_EQ(m_added, added); // nothing created
    EXPECT_EQ(history().size(), journaled);
    EXPECT_EQ(m_store->resolves.load(), 1); // only the Update's plan
}

class UpdateAllWithPackageKit : public WithPackageKit
{
protected:
    /// GIMP installed from Debian with a security update. `removes`: the
    /// update also removes a package (a confirmation). `holdSimulation`:
    /// the first simulation stays running until finished by the test.
    /// `holdReal`: the real update stays running, cancellable or not.
    void debUpdate(bool removes = false, bool holdSimulation = false, bool holdReal = false, bool cancellable = true)
    {
        fakepk::Answer installedDeb;
        installedDeb.packages = {pkg(pk::Info::Installed, kGimpDebInstalled)};
        m_pk.answer("GetPackages", installedDeb);
        fakepk::Answer updates;
        updates.packages = {pkg(pk::Info::Security, kGimpDebUpdate)};
        m_pk.answer("GetUpdates", updates);
        fakepk::Answer sim;
        sim.packages = {pkg(pk::Info::Updating, kGimpDebUpdate)};
        if (removes)
            sim.packages.push_back(pkg(pk::Info::Removing, kLibDebInstalled));
        if (holdSimulation) {
            fakepk::Answer held = sim;
            held.hold = true;
            m_pk.answer("UpdatePackages", held, true);
        }
        m_pk.answer("UpdatePackages", sim, true);
        fakepk::Answer real;
        if (holdReal) {
            real.hold = true;
            real.steps = {{pk::Status::Update, 50, cancellable}};
        }
        m_pk.answer("UpdatePackages", real, false);
    }

    int storeUpdates(bool simulate)
    {
        int n = 0;
        for (const auto& c : m_pk.calls())
            n += c.method == "UpdatePackages" && c.simulate() == simulate;
        return n;
    }

    /// The held UpdatePackages of the store, once it is the only transaction
    /// running and the last one asked (the store's PackageKit queries are
    /// one at a time, P-009: none can run beside it).
    std::string heldPackageKit()
    {
        std::vector<std::string> held;
        spin([&] {
            held = m_pk.runningTransactions();
            const auto calls = m_pk.calls();
            return held.size() == 1 && !calls.empty() && calls.back().method == "UpdatePackages";
        });
        return held.size() == 1 ? held.front() : std::string();
    }

    /// The children of a parent, by their only offer.
    std::map<std::string, std::string> childrenByOffer(const std::string& parent)
    {
        std::map<std::string, std::string> out;
        for (const std::string& c : children(parent)) {
            const auto offers = strings(transaction(c), "OfferIds");
            if (offers.size() == 1)
                out[offers.front()] = c;
        }
        return out;
    }
};

TEST_F(UpdateAllWithPackageKit, UpdateAllCoversEveryBackendAndInstallation)
{
    debUpdate(false, false, true);
    start([](fp::FakeStore& s) { flatpakUpdates(s); }, Options{.packagekit = true});
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 3; }));
    EXPECT_EQ(property<std::uint32_t>("UpdateCount"), 3u);

    const Answer all = updateAll();
    ASSERT_TRUE(all.error.empty()) << all.error;
    const auto parent = transaction(all.path);
    EXPECT_EQ(parent.at("Kind").value<std::string>(), "update-all");
    EXPECT_EQ(parent.at("Backend").value<std::string>(), "multiple");
    EXPECT_EQ(parent.at("Parent").value<dbus::ObjectPath>().path, "/");
    EXPECT_TRUE(skipped(all.path).empty());
    EXPECT_EQ(strings(parent, "OfferIds").size(), 3u);

    // One child per backend and installation (D-031), each pointing back.
    const auto byOffer = childrenByOffer(all.path);
    ASSERT_EQ(byOffer.size(), 3u);
    for (const char* offer : {"packagekit:gimp", kGimpUser, kQgisSystem}) {
        ASSERT_TRUE(byOffer.contains(offer)) << offer;
        const auto child = transaction(byOffer.at(offer));
        EXPECT_EQ(child.at("Kind").value<std::string>(), "update");
        EXPECT_EQ(child.at("Parent").value<dbus::ObjectPath>().path, all.path);
        EXPECT_TRUE(children(byOffer.at(offer)).empty());
        EXPECT_TRUE(skipped(byOffer.at(offer)).empty());
    }
    EXPECT_EQ(transaction(byOffer.at("packagekit:gimp")).at("Backend").value<std::string>(), "packagekit");
    EXPECT_EQ(transaction(byOffer.at(kGimpUser)).at("Backend").value<std::string>(), "flatpak");

    // PackageKit needs no confirmation and runs; the two Flatpak children
    // bring permissions and wait: the parent asks once, with every plan.
    ASSERT_TRUE(waitState(byOffer.at("packagekit:gimp"), "running"));
    ASSERT_TRUE(waitState(all.path, "awaiting-confirmation"));
    const auto p = plan(all.path);
    EXPECT_EQ(p.at("kind").value<std::string>(), "update-all");
    EXPECT_TRUE(p.at("complete").value<bool>(false));
    EXPECT_TRUE(p.at("requires-confirmation").value<bool>(false));
    EXPECT_TRUE(has(strings(p, "reasons"), "new-permissions"));
    const auto planned = p.at("children").value<std::vector<dbus::VariantMap>>();
    ASSERT_EQ(planned.size(), 3u);
    std::uint64_t download = 0;
    for (std::size_t i = 0; i < planned.size(); ++i) {
        const std::string path = planned[i].at("transaction").value<dbus::ObjectPath>().path;
        EXPECT_EQ(path, children(all.path)[i]); // same order as Children
        EXPECT_EQ(planned[i].at("plan").value<dbus::VariantMap>(), plan(path));
        EXPECT_EQ(planned[i].at("state").value<std::string>(), state(path));
        download += plan(path).at("download-size").value<std::uint64_t>();
    }
    EXPECT_EQ(p.at("download-size").value<std::uint64_t>(), download);
    EXPECT_EQ(download, 3000u); // two Flatpak plans of 1500, PackageKit gives 0 here

    EXPECT_EQ(confirm(all.path), "");
    ASSERT_TRUE(waitState(byOffer.at(kGimpUser), "succeeded"));
    ASSERT_TRUE(waitState(byOffer.at(kQgisSystem), "succeeded"));
    EXPECT_EQ(state(all.path), "running"); // PackageKit still runs
    m_pk.replace("GetUpdates", fakepk::Answer{});
    ASSERT_TRUE(m_pk.finish(heldPackageKit()));
    ASSERT_TRUE(waitState(all.path, "succeeded"));
    EXPECT_EQ(storeUpdates(false), 1);
    EXPECT_EQ(m_store->runs.load(), 2);

    // Children in the journal, the parent never (D-018, D-031).
    for (const auto& [offer, path] : byOffer)
        EXPECT_EQ(journal(path).at("state").value<std::string>(), "succeeded") << offer;
    const std::string parentId = parent.at("Id").value<std::string>();
    for (auto& h : history())
        EXPECT_NE(h.at("id").value<std::string>(), parentId);
    ASSERT_TRUE(spin([&] { return updateOffers().empty() && property<std::uint32_t>("UpdateCount") == 0; }));
}

TEST_F(UpdateAllWithPackageKit, PackageKitRunsBesideFlatpakAndFlatpakOneAtATime)
{
    // P-009 as decided: the backends never wait for each other (D-030); the
    // two Flatpak installations share one lane (D-034).
    debUpdate(false, false, true);
    start([](fp::FakeStore& s) { flatpakUpdates(s); }, Options{.packagekit = true});
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 3; }));
    const Answer all = updateAll();
    ASSERT_TRUE(waitState(all.path, "awaiting-confirmation"));
    const auto byOffer = childrenByOffer(all.path);
    m_store->whileRunning.close();
    EXPECT_EQ(confirm(all.path), "");

    // PackageKit and one Flatpak child run together; the other Flatpak child
    // waits, confirmed, in planning.
    std::string first;
    std::string second;
    ASSERT_TRUE(spin([&] {
        const std::string user = state(byOffer.at(kGimpUser));
        const std::string system = state(byOffer.at(kQgisSystem));
        if (user == "running" && system == "planning") {
            first = byOffer.at(kGimpUser);
            second = byOffer.at(kQgisSystem);
        } else if (system == "running" && user == "planning") {
            first = byOffer.at(kQgisSystem);
            second = byOffer.at(kGimpUser);
        }
        return !first.empty() && state(byOffer.at("packagekit:gimp")) == "running";
    }));
    EXPECT_EQ(state(all.path), "running");
    EXPECT_EQ(m_store->runs.load(), 1);
    settle();
    EXPECT_EQ(state(second), "planning");

    m_store->whileRunning.open();
    ASSERT_TRUE(waitState(first, "succeeded"));
    ASSERT_TRUE(waitState(second, "succeeded"));
    EXPECT_EQ(state(byOffer.at("packagekit:gimp")), "running");
    ASSERT_TRUE(m_pk.finish(heldPackageKit()));
    ASSERT_TRUE(waitState(all.path, "succeeded"));
}

TEST_F(UpdateAllWithPackageKit, TheParentAsksForConfirmationOnceEveryPlanIsKnown)
{
    // D-037, decision D: a child awaiting a confirmation does not make the
    // parent ask while another child is still planning.
    debUpdate(true, true);
    start([](fp::FakeStore& s) { flatpakUpdates(s, false); }, Options{.packagekit = true});
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 2; }));
    const Answer all = updateAll();
    ASSERT_TRUE(all.error.empty()) << all.error;
    const auto byOffer = childrenByOffer(all.path);
    const std::string deb = byOffer.at("packagekit:gimp");
    const std::string flatpak = byOffer.at(kGimpUser);

    ASSERT_TRUE(waitState(flatpak, "awaiting-confirmation"));
    const std::string simulation = heldPackageKit(); // PackageKit still simulating
    ASSERT_FALSE(simulation.empty());
    settle();
    EXPECT_EQ(state(deb), "planning");
    EXPECT_EQ(state(all.path), "planning");
    EXPECT_EQ(confirm(all.path), kNotAllowed); // not every plan is shown yet
    EXPECT_EQ(txProperty<std::uint64_t>(flatpak, "ConfirmationDeadline"), 0u); // its delay has not started
    auto partial = plan(all.path);
    EXPECT_FALSE(partial.at("complete").value<bool>(true));
    EXPECT_FALSE(partial.contains("download-size")); // never a partial total
    EXPECT_FALSE(partial.contains("installed-size-change"));

    ASSERT_TRUE(m_pk.finish(simulation));
    ASSERT_TRUE(waitState(all.path, "awaiting-confirmation"));
    ASSERT_TRUE(waitState(deb, "awaiting-confirmation"));
    const auto p = plan(all.path);
    EXPECT_TRUE(p.at("complete").value<bool>(false));
    EXPECT_TRUE(p.contains("download-size"));
    EXPECT_TRUE(has(strings(p, "reasons"), "new-permissions"));
    EXPECT_TRUE(has(strings(p, "reasons"), "removes-packages"));
    const auto debDeadline = txProperty<std::uint64_t>(deb, "ConfirmationDeadline");
    const auto flatpakDeadline = txProperty<std::uint64_t>(flatpak, "ConfirmationDeadline");
    EXPECT_NE(debDeadline, 0u);
    EXPECT_NE(flatpakDeadline, 0u);
    EXPECT_EQ(txProperty<std::uint64_t>(all.path, "ConfirmationDeadline"), std::min(debDeadline, flatpakDeadline));

    EXPECT_EQ(confirm(all.path), "");
    ASSERT_TRUE(waitState(all.path, "succeeded"));
    EXPECT_EQ(state(deb), "succeeded");
    EXPECT_EQ(state(flatpak), "succeeded");
    // One confirmation asked, once.
    const auto seen = states(all.path);
    EXPECT_EQ(std::count(seen.begin(), seen.end(), "awaiting-confirmation"), 1);
}

TEST_F(UpdateAllWithPackageKit, AChildConfirmationDelayStartsWhenTheParentAsks)
{
    // D-019 with D-037: a child that waits for its siblings' plans is not
    // cancelled by its delay meanwhile; once the parent asks, the delay
    // applies to every child, and nothing runs without a confirmation.
    debUpdate(true, true);
    start([](fp::FakeStore& s) { flatpakUpdates(s, false); },
          Options{.packagekit = true, .confirmationTimeout = 300ms});
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 2; }));
    const Answer all = updateAll();
    const auto byOffer = childrenByOffer(all.path);
    const std::string flatpak = byOffer.at(kGimpUser);
    ASSERT_TRUE(waitState(flatpak, "awaiting-confirmation"));
    const std::string simulation = heldPackageKit();
    settle(700ms); // more than the delay
    EXPECT_EQ(state(flatpak), "awaiting-confirmation");

    ASSERT_TRUE(m_pk.finish(simulation));
    ASSERT_TRUE(waitState(all.path, "cancelled"));
    EXPECT_EQ(state(flatpak), "cancelled");
    EXPECT_EQ(state(byOffer.at("packagekit:gimp")), "cancelled");
    EXPECT_EQ(m_store->executes.load(), 0);
    EXPECT_EQ(storeUpdates(false), 0);
}

TEST_F(UpdateAllWithPackageKit, ALockedPackageKitFailsItsChildWhileFlatpakUpdates)
{
    // D-038, decision B: Settings is updating the system; the deb child fails
    // locked, Flatpak goes on, and the parent ends failed. The system update
    // is never touched.
    debUpdate();
    start([](fp::FakeStore& s) { flatpakUpdates(s, false); }, Options{.packagekit = true});
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 2; }));
    fakepk::Answer held;
    held.hold = true;
    held.steps = {{pk::Status::Install, 30, false}};
    const std::string external = m_pk.startExternal("UpdatePackages", held);
    ASSERT_TRUE(spin([&] {
        const auto b = backend("packagekit");
        return b.contains("activity") && b.at("activity").value<std::string>() == "changing";
    }));
    const int simulations = storeUpdates(true);

    const Answer all = updateAll();
    ASSERT_TRUE(all.error.empty()) << all.error;
    const auto byOffer = childrenByOffer(all.path);
    ASSERT_TRUE(waitState(byOffer.at("packagekit:gimp"), "failed"));
    EXPECT_EQ(errorCode(byOffer.at("packagekit:gimp")), "locked");
    // The failed child has no plan: every plan is known, the parent asks.
    ASSERT_TRUE(waitState(all.path, "awaiting-confirmation"));
    EXPECT_EQ(confirm(all.path), "");
    ASSERT_TRUE(waitState(byOffer.at(kGimpUser), "succeeded"));
    ASSERT_TRUE(waitState(all.path, "failed"));
    EXPECT_EQ(errorCode(all.path), "locked");
    EXPECT_EQ(storeUpdates(true), simulations); // nothing sent to PackageKit
    EXPECT_EQ(m_pk.cancels(), 0);
    ASSERT_TRUE(m_pk.finish(external));
    EXPECT_EQ(property<std::string>("State"), "ready");
}

TEST_F(UpdateAllWithPackageKit, APartialFailureNeverSucceeds)
{
    debUpdate();
    start([](fp::FakeStore& s) {
        flatpakUpdates(s, false);
        s.runError = Error::backend(BackendFailure::AuthDenied, "polkit said no");
    }, Options{.packagekit = true});
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 2; }));
    const Answer all = updateAll();
    const auto byOffer = childrenByOffer(all.path);
    ASSERT_TRUE(waitState(all.path, "awaiting-confirmation"));
    EXPECT_EQ(confirm(all.path), "");
    ASSERT_TRUE(waitState(all.path, "failed"));
    EXPECT_EQ(state(byOffer.at("packagekit:gimp")), "succeeded");
    EXPECT_EQ(state(byOffer.at(kGimpUser)), "failed");
    EXPECT_EQ(errorCode(all.path), "auth-denied");
    EXPECT_EQ(journal(byOffer.at("packagekit:gimp")).at("state").value<std::string>(), "succeeded");
    EXPECT_EQ(journal(byOffer.at(kGimpUser)).at("state").value<std::string>(), "failed");
}

TEST_F(UpdateAllWithPackageKit, CancellingTheParentReachesWhatCanBeCancelled)
{
    // D-036: the parent is Cancellable when a Cancel would reach at least one
    // child; it never cancels what cannot be.
    debUpdate(false, false, true, false); // PackageKit: AllowCancel false
    start([](fp::FakeStore& s) { flatpakUpdates(s, false); }, Options{.packagekit = true});
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 2; }));
    const Answer all = updateAll();
    const auto byOffer = childrenByOffer(all.path);
    const std::string deb = byOffer.at("packagekit:gimp");
    const std::string flatpak = byOffer.at(kGimpUser);
    ASSERT_TRUE(waitState(all.path, "awaiting-confirmation"));
    EXPECT_TRUE(txProperty<bool>(all.path, "Cancellable")); // the Flatpak child awaits
    m_store->whileRunning.close();
    EXPECT_EQ(confirm(all.path), "");
    ASSERT_TRUE(waitState(flatpak, "running"));
    ASSERT_TRUE(spin([&] { return state(deb) == "running" && !txProperty<bool>(deb, "Cancellable"); }));
    ASSERT_TRUE(spin([&] { return txProperty<bool>(all.path, "Cancellable"); }));

    EXPECT_EQ(cancel(all.path), "");
    ASSERT_TRUE(waitState(flatpak, "cancelled"));
    EXPECT_EQ(state(deb), "running");
    ASSERT_TRUE(spin([&] { return !txProperty<bool>(all.path, "Cancellable"); }));
    EXPECT_EQ(cancel(all.path), kNotAllowed); // only PackageKit is left, and it refuses
    EXPECT_EQ(m_pk.cancels(), 0);
    ASSERT_TRUE(m_pk.finish(heldPackageKit()));
    ASSERT_TRUE(waitState(all.path, "cancelled"));
    EXPECT_EQ(state(deb), "succeeded");
}

TEST_F(UpdateAllWithPackageKit, TheParentShowsNoFractionForSeveralChildren)
{
    // D-036: several children of different kinds have no common measure; the
    // parent never publishes a fraction the children did not give.
    debUpdate(false, false, true);
    start([](fp::FakeStore& s) { flatpakUpdates(s, false); }, Options{.packagekit = true});
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 2; }));
    const Answer all = updateAll();
    const auto byOffer = childrenByOffer(all.path);
    ASSERT_TRUE(waitState(all.path, "awaiting-confirmation"));
    EXPECT_EQ(confirm(all.path), "");
    ASSERT_TRUE(waitState(byOffer.at(kGimpUser), "succeeded"));
    ASSERT_TRUE(spin([&] { return txProperty<double>(byOffer.at("packagekit:gimp"), "Progress") == 0.5; }));
    ASSERT_TRUE(m_pk.finish(heldPackageKit()));
    ASSERT_TRUE(waitState(all.path, "succeeded"));
    // The children had progress (Flatpak 0.5 at its end, PackageKit 50 %).
    EXPECT_FALSE(m_progress[byOffer.at("packagekit:gimp")].empty());
    for (const double fraction : m_progress[all.path])
        EXPECT_EQ(fraction, -1.0);
    EXPECT_EQ(txProperty<double>(all.path, "Progress"), -1.0);
}

TEST_F(FlatpakTransactions, TheParentOfOneChildShowsItsProgress)
{
    start([](fp::FakeStore& s) { flatpakUpdates(s, false); });
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 1; }));
    const Answer all = updateAll();
    ASSERT_TRUE(waitState(all.path, "awaiting-confirmation"));
    EXPECT_EQ(confirm(all.path), "");
    ASSERT_TRUE(waitState(all.path, "succeeded"));
    const auto& seen = m_progress[all.path];
    EXPECT_NE(std::find(seen.begin(), seen.end(), 0.5), seen.end()); // the child's 0.5
}

TEST_F(UpdateAllWithPackageKit, AfterARestartTheChildrenAreRecoveredWithoutTheirParent)
{
    // §5.6 unchanged: the parent is not journaled. A PackageKit child still
    // running is re-attached alone (Parent "/"); a Flatpak child running is
    // interrupted. Nothing is executed again. (H.4 study: see SERVICE.md
    // §24 for the options to keep the link.)
    debUpdate(false, false, true);
    start([](fp::FakeStore& s) { flatpakUpdates(s, false); }, Options{.packagekit = true});
    ASSERT_TRUE(spin([&] { return updateOffers().size() == 2; }));
    const Answer all = updateAll();
    const auto byOffer = childrenByOffer(all.path);
    ASSERT_TRUE(waitState(all.path, "awaiting-confirmation"));
    m_store->whileRunning.close();
    EXPECT_EQ(confirm(all.path), "");
    ASSERT_TRUE(waitState(byOffer.at(kGimpUser), "running"));
    ASSERT_TRUE(waitState(byOffer.at("packagekit:gimp"), "running"));
    const std::string debId = transaction(byOffer.at("packagekit:gimp")).at("Id").value<std::string>();
    const std::string flatpakId = transaction(byOffer.at(kGimpUser)).at("Id").value<std::string>();
    const std::string parentId = transaction(all.path).at("Id").value<std::string>();
    const std::string held = heldPackageKit();
    m_store->whileRunning.open();
    release();

    start({}, Options{.packagekit = true});
    std::string reattached;
    ASSERT_TRUE(spin([&] {
        for (const auto& t : value<std::vector<dbus::ObjectPath>>("ListTransactions").value_or(
                     std::vector<dbus::ObjectPath>{}))
            if (transaction(t.path).at("Id").value<std::string>() == debId)
                reattached = t.path;
        return !reattached.empty();
    }));
    EXPECT_EQ(transaction(reattached).at("Parent").value<dbus::ObjectPath>().path, "/");
    for (const auto& t : value<std::vector<dbus::ObjectPath>>("ListTransactions").value_or(
                 std::vector<dbus::ObjectPath>{})) {
        EXPECT_NE(transaction(t.path).at("Id").value<std::string>(), parentId);
        EXPECT_NE(transaction(t.path).at("Kind").value<std::string>(), "update-all");
    }
    std::map<std::string, std::string> byId;
    ASSERT_TRUE(spin([&] {
        byId.clear();
        for (auto& h : history())
            byId[h.at("id").value<std::string>()] = h.at("state").value<std::string>();
        return byId[flatpakId] == "interrupted";
    }));
    ASSERT_TRUE(m_pk.finish(held));
    ASSERT_TRUE(waitState(reattached, "succeeded"));
    EXPECT_EQ(m_store->executes.load(), 0);
    EXPECT_EQ(storeUpdates(false), 1); // never run again
}


// --- Sources (H.5, SERVICE.md §25) ---------------------------------------------------

constexpr const char* kSpare = "flatpak:user:spare";
constexpr const char* kUnsigned = "flatpak:user:unsigned";
constexpr const char* kFlathubUser = "flatpak:user:flathub";
constexpr const char* kFlathubSystem = "flatpak:system:flathub";
constexpr const char* kNotFound = "org.cutefish.Store1.Error.NotFound";
constexpr const char* kBackendUnavailable = "org.cutefish.Store1.Error.BackendUnavailable";

class FlatpakSources : public FlatpakTransactions
{
protected:
    void SetUp() override
    {
        FlatpakTransactions::SetUp();
        m_matches.push_back(m_conn->subscribe(rule(kInterface, "SourcesChanged"),
                                              [this](const dbus::Message&) { ++m_sourcesChanged; }));
    }

    /// Two more user remotes: `spare` (signed, unused) and `unsigned`
    /// (gpg-verify=false), with `enabled` as given; GIMP installed from the
    /// user Flathub, so that remote is in use.
    static void script(fp::FakeStore& s, bool spareEnabled = true)
    {
        s.remotes_[Scope::User].push_back({.name = "spare", .url = "https://example.net/repo/", .disabled = !spareEnabled,
                                           .gpgVerify = true, .metadataPath = {}, .iconsPath = {}});
        s.remotes_[Scope::User].push_back({.name = "unsigned", .url = "https://example.org/repo/", .disabled = true,
                                           .gpgVerify = false, .metadataPath = {}, .iconsPath = {}});
        s.installed_[Scope::User] = {{app("org.gimp.GIMP"), "flathub"}};
    }

    std::vector<dbus::VariantMap> sources()
    {
        return value<std::vector<dbus::VariantMap>>("ListSources").value_or(std::vector<dbus::VariantMap>{});
    }

    std::optional<dbus::VariantMap> source(const std::string& id)
    {
        for (auto& s : sources())
            if (s.at("id").value<std::string>() == id)
                return s;
        return std::nullopt;
    }

    const fp::RemoteInfo* remote(Scope scope, const std::string& name)
    {
        for (const fp::RemoteInfo& r : m_store->remotes_[scope])
            if (r.name == name)
                return &r;
        return nullptr;
    }

    Answer enableSource(const std::string& id) { return call(kPath, kInterface, "EnableSource", id); }
    Answer disableSource(const std::string& id) { return call(kPath, kInterface, "DisableSource", id); }
    Answer removeSource(const std::string& id) { return call(kPath, kInterface, "RemoveSource", id); }

    int m_sourcesChanged = 0;
};

TEST_F(FlatpakSources, ListsWhatTheBackendsHaveEnabledOrNot)
{
    start([](fp::FakeStore& s) { script(s, false); });
    ASSERT_TRUE(spin([&] { return source(kSpare).has_value(); }));
    const auto spare = *source(kSpare);
    // A disabled remote is listed (it is not in the catalogue).
    EXPECT_FALSE(spare.at("enabled").value<bool>(true));
    EXPECT_TRUE(spare.at("configured").value<bool>(false));
    EXPECT_TRUE(spare.at("can-toggle").value<bool>(false));
    EXPECT_TRUE(spare.at("can-remove").value<bool>(false));
    EXPECT_EQ(spare.at("offers").value<std::uint32_t>(), 0u);
    EXPECT_TRUE(spare.at("gpg-verify").value<bool>(false));

    const auto user = *source(kFlathubUser);
    EXPECT_EQ(user.at("kind").value<std::string>(), "flathub");
    EXPECT_GT(user.at("offers").value<std::uint32_t>(), 0u); // from the catalogue
    EXPECT_TRUE(user.at("can-toggle").value<bool>(false));
    EXPECT_FALSE(user.at("can-remove").value<bool>(true)); // GIMP came from it (D-041)
    EXPECT_EQ(user.at("installed-count").value<std::uint32_t>(), 1u);

    const auto system = *source(kFlathubSystem);
    EXPECT_FALSE(system.at("can-toggle").value<bool>(true)); // D-011
    EXPECT_FALSE(system.at("can-remove").value<bool>(true));

    EXPECT_FALSE(source(kUnsigned)->at("can-toggle").value<bool>(true)); // D-044

    EXPECT_EQ(backend("flatpak").at("source-operations").value<std::vector<std::string>>(),
              (std::vector<std::string>{"enable", "disable", "remove"}));
    // Exported with a backend that changes sources; never an addition (D-040).
    // Asynchronously: the service answers on this very loop.
    auto xml = std::make_shared<std::string>();
    auto done = std::make_shared<bool>(false);
    std::vector<dbus::PendingCall> calls;
    calls.push_back(dbus::Proxy(m_conn.get(), kStore, kPath, "org.freedesktop.DBus.Introspectable")
                            .callAsync("Introspect", [xml, done](const dbus::Reply& r) {
                                if (!r.isError())
                                    r.read(*xml);
                                *done = true;
                            }));
    ASSERT_TRUE(spin([&] { return *done; }));
    ASSERT_FALSE(xml->empty());
    for (const char* member : {"\"EnableSource\"", "\"DisableSource\"", "\"RemoveSource\""})
        EXPECT_NE(xml->find(member), std::string::npos) << member;
    for (const char* member : {"\"AddSource\"", "\"GetPermissions\""})
        EXPECT_EQ(xml->find(member), std::string::npos) << member;
}

TEST_F(FlatpakSources, DisablingAndRemovingNeedNoConfirmation)
{
    start([](fp::FakeStore& s) { script(s); });
    const int changedBefore = m_sourcesChanged;
    const Answer disabled = disableSource(kSpare);
    ASSERT_EQ(disabled.error, "");
    ASSERT_TRUE(waitState(disabled.path, "succeeded"));
    EXPECT_FALSE(has(states(disabled.path), "awaiting-confirmation")); // D-042
    EXPECT_TRUE(has(states(disabled.path), "running"));
    const auto tx = transaction(disabled.path);
    EXPECT_EQ(tx.at("Kind").value<std::string>(), "disable-source");
    EXPECT_EQ(tx.at("Backend").value<std::string>(), "flatpak");
    EXPECT_FALSE(tx.at("Plan").value<dbus::VariantMap>().at("requires-confirmation").value<bool>(true));
    EXPECT_TRUE(remote(Scope::User, "spare")->disabled);
    // Read again from the backend, and said.
    ASSERT_TRUE(spin([&] { return m_sourcesChanged > changedBefore; }));
    ASSERT_TRUE(spin([&] { return !source(kSpare)->at("enabled").value<bool>(true); }));

    const Answer removed = removeSource(kSpare);
    ASSERT_EQ(removed.error, "");
    ASSERT_TRUE(waitState(removed.path, "succeeded"));
    EXPECT_EQ(transaction(removed.path).at("Kind").value<std::string>(), "remove-source");
    EXPECT_EQ(remote(Scope::User, "spare"), nullptr);
    ASSERT_TRUE(spin([&] { return !source(kSpare).has_value(); }));
    // Not journaled (D-043).
    EXPECT_TRUE(journal(disabled.path).empty());
    EXPECT_TRUE(journal(removed.path).empty());
}

TEST_F(FlatpakSources, ReactivatingAConfiguredThirdPartyRemoteWaitsForConfirm)
{
    // Permitted: not the addition of a source (D-040), but a plan and an
    // explicit Confirm (D-042), as confirmed by the owner for H.5.
    start([](fp::FakeStore& s) { script(s, false); });
    ASSERT_TRUE(spin([&] { return source(kSpare).has_value(); }));
    EXPECT_EQ(source(kSpare)->at("kind").value<std::string>(), "third-party");
    const std::size_t remotesBefore = m_store->remotes_[Scope::User].size();
    const Answer a = enableSource(kSpare);
    ASSERT_EQ(a.error, "");
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    const auto p = plan(a.path);
    EXPECT_EQ(p.at("kind").value<std::string>(), "enable-source");
    EXPECT_TRUE(p.at("requires-confirmation").value<bool>(false));
    EXPECT_EQ(strings(p, "reasons"), std::vector<std::string>{"new-source"});
    EXPECT_EQ(p.at("source").value<dbus::VariantMap>().at("id").value<std::string>(), kSpare);
    EXPECT_EQ(p.at("source").value<dbus::VariantMap>().at("kind").value<std::string>(), "third-party");
    EXPECT_GT(txProperty<std::uint64_t>(a.path, "ConfirmationDeadline"), 0u);
    EXPECT_TRUE(txProperty<bool>(a.path, "Cancellable"));
    // Nothing changes before Confirm.
    settle(200ms);
    EXPECT_TRUE(remote(Scope::User, "spare")->disabled);

    EXPECT_EQ(confirm(a.path), "");
    ASSERT_TRUE(waitState(a.path, "succeeded"));
    EXPECT_FALSE(remote(Scope::User, "spare")->disabled);
    EXPECT_TRUE(strings(plan(a.path), "reasons").empty()); // no confirmation pending any more
    EXPECT_EQ(confirm(a.path), kNotAllowed);
    EXPECT_EQ(m_store->remotes_[Scope::User].size(), remotesBefore); // re-enabled, nothing added
    ASSERT_TRUE(spin([&] { return source(kSpare)->at("enabled").value<bool>(false); }));
}

TEST_F(FlatpakSources, AnUnconfirmedOrCancelledEnableChangesNothing)
{
    start([](fp::FakeStore& s) { script(s, false); }, Options{.packagekit = false, .confirmationTimeout = 300ms});
    const Answer expired = enableSource(kSpare);
    ASSERT_TRUE(waitState(expired.path, "awaiting-confirmation"));
    ASSERT_TRUE(waitState(expired.path, "cancelled")); // D-019
    EXPECT_TRUE(remote(Scope::User, "spare")->disabled);

    const Answer cancelled = enableSource(kSpare);
    ASSERT_TRUE(waitState(cancelled.path, "awaiting-confirmation"));
    EXPECT_EQ(cancel(cancelled.path), "");
    ASSERT_TRUE(waitState(cancelled.path, "cancelled"));
    EXPECT_EQ(confirm(cancelled.path), kNotAllowed);
    settle();
    EXPECT_TRUE(remote(Scope::User, "spare")->disabled);
}

TEST_F(FlatpakSources, ARemoteWithoutSignaturesIsNeverEnabled)
{
    start([](fp::FakeStore& s) { script(s); });
    const Answer a = enableSource(kUnsigned);
    ASSERT_EQ(a.error, "");
    ASSERT_TRUE(waitState(a.path, "failed"));
    EXPECT_EQ(errorCode(a.path), "untrusted"); // D-044
    EXPECT_FALSE(has(states(a.path), "awaiting-confirmation")); // nothing to confirm
    EXPECT_TRUE(remote(Scope::User, "unsigned")->disabled);
    EXPECT_EQ(confirm(a.path), kNotAllowed); // the refusal cannot be confirmed away
    EXPECT_FALSE(source(kUnsigned)->at("can-toggle").value<bool>(true));
    // Asked again, refused again: never re-enabled.
    const Answer again = enableSource(kUnsigned);
    ASSERT_TRUE(waitState(again.path, "failed"));
    EXPECT_EQ(errorCode(again.path), "untrusted");
    EXPECT_TRUE(remote(Scope::User, "unsigned")->disabled);
    // Removing it stays possible.
    const Answer removed = removeSource(kUnsigned);
    ASSERT_TRUE(waitState(removed.path, "succeeded"));
    EXPECT_EQ(remote(Scope::User, "unsigned"), nullptr);
}

TEST_F(FlatpakSources, AnInstallFromARemoteWithoutSignaturesFailsUntrusted)
{
    start([](fp::FakeStore& s) { s.remotes_[Scope::User][0].gpgVerify = false; });
    const Answer a = install(kGimpUser);
    ASSERT_TRUE(waitState(a.path, "failed"));
    EXPECT_EQ(errorCode(a.path), "untrusted"); // D-044, while planning
    EXPECT_EQ(m_store->executes.load(), 0);
}

TEST_F(FlatpakSources, RequestsAreCheckedBeforeAnyTransaction)
{
    start([](fp::FakeStore& s) { script(s, false); });
    const int added = m_added;
    // The system installation is read-only (D-011).
    EXPECT_EQ(enableSource(kFlathubSystem).error, kNotAllowed);
    EXPECT_EQ(disableSource(kFlathubSystem).error, kNotAllowed);
    EXPECT_EQ(removeSource(kFlathubSystem).error, kNotAllowed);
    // A remote something installed came from is not removed (D-041).
    EXPECT_EQ(removeSource(kFlathubUser).error, kNotAllowed);
    EXPECT_EQ(enableSource(kFlathubUser).error, kNotAllowed);  // already enabled
    EXPECT_EQ(disableSource(kSpare).error, kNotAllowed);       // already disabled
    EXPECT_EQ(enableSource("flatpak:user:nowhere").error, kNotFound);
    EXPECT_EQ(enableSource("").error, kInvalidArgs);
    EXPECT_EQ(m_added, added); // no transaction was created
    EXPECT_FALSE(remote(Scope::User, "flathub")->disabled);

    // A second change of a source whose change is not finished: Busy.
    const Answer first = enableSource(kSpare);
    ASSERT_TRUE(waitState(first.path, "awaiting-confirmation"));
    EXPECT_EQ(enableSource(kSpare).error, kBusy);
    EXPECT_EQ(removeSource(kSpare).error, kBusy);
    EXPECT_EQ(cancel(first.path), "");
    ASSERT_TRUE(waitState(first.path, "cancelled"));
}

TEST_F(FlatpakSources, ASourceChangeWaitsForTheRunningTransaction)
{
    // One modification at a time on Flatpak (P-009, D-034): the change waits
    // in the lane behind a running installation.
    start([](fp::FakeStore& s) { script(s); });
    const Answer installing = install(kInkscapeUser);
    ASSERT_TRUE(waitState(installing.path, "awaiting-confirmation"));
    m_store->whileRunning.close();
    EXPECT_EQ(confirm(installing.path), "");
    ASSERT_TRUE(waitState(installing.path, "running"));

    const Answer change = disableSource(kSpare);
    ASSERT_EQ(change.error, "");
    settle(200ms);
    EXPECT_EQ(state(change.path), "queued");
    EXPECT_FALSE(remote(Scope::User, "spare")->disabled);

    m_store->whileRunning.open();
    ASSERT_TRUE(waitState(installing.path, "succeeded"));
    ASSERT_TRUE(waitState(change.path, "succeeded"));
    EXPECT_TRUE(remote(Scope::User, "spare")->disabled);
}

TEST_F(FlatpakSources, AQueuedChangeCanBeCancelled)
{
    start([](fp::FakeStore& s) { script(s); });
    const Answer installing = install(kInkscapeUser);
    ASSERT_TRUE(waitState(installing.path, "awaiting-confirmation"));
    m_store->whileRunning.close();
    EXPECT_EQ(confirm(installing.path), "");
    ASSERT_TRUE(waitState(installing.path, "running"));
    const Answer change = disableSource(kSpare);
    ASSERT_TRUE(waitState(change.path, "queued"));
    EXPECT_EQ(cancel(change.path), "");
    ASSERT_TRUE(waitState(change.path, "cancelled"));
    m_store->whileRunning.open();
    ASSERT_TRUE(waitState(installing.path, "succeeded"));
    settle();
    EXPECT_FALSE(remote(Scope::User, "spare")->disabled); // never ran
}

TEST_F(FlatpakSources, EnablingARemoteNeverAllowsAnImplicitAddition)
{
    // G.1 stays: the consent given to one source is not a consent to add
    // another during a transaction.
    start([](fp::FakeStore& s) { script(s, false); });
    const Answer enabled = enableSource(kSpare);
    ASSERT_TRUE(waitState(enabled.path, "awaiting-confirmation"));
    EXPECT_EQ(confirm(enabled.path), "");
    ASSERT_TRUE(waitState(enabled.path, "succeeded"));

    m_store->remoteAddAttempted = true;
    const Answer a = install(kInkscapeUser);
    ASSERT_EQ(a.error, "");
    ASSERT_TRUE(waitState(a.path, "failed"));
    EXPECT_EQ(errorCode(a.path), "not-allowed");
    EXPECT_EQ(m_store->executes.load(), 0);
    EXPECT_EQ(m_store->remotes_[Scope::User].size(), 3u); // nothing added
}

TEST_F(FlatpakSources, AfterARestartAPendingEnableIsGone)
{
    // D-043: source transactions are not journaled; nothing is replayed.
    start([](fp::FakeStore& s) { script(s, false); });
    const Answer a = enableSource(kSpare);
    ASSERT_TRUE(waitState(a.path, "awaiting-confirmation"));
    release();
    start([](fp::FakeStore& s) { script(s, false); });
    EXPECT_TRUE(value<std::vector<dbus::ObjectPath>>("ListTransactions").value_or(std::vector<dbus::ObjectPath>{}).empty());
    EXPECT_TRUE(history().empty());
    EXPECT_TRUE(m_store->remotes_[Scope::User][1].disabled);
}

TEST_F(FlatpakSources, AnUnusableBackendChangesNoSource)
{
    // No Flatpak installation: the backend is configured, not usable.
    auto store = newStore();
    store->scopes_ = {};
    m_instance = std::make_unique<Instance>(m_loop, *m_conn, m_dir.path(), std::move(store), Options{});
    m_store = m_instance->store_;
    ASSERT_TRUE(m_instance->service->start());
    ASSERT_TRUE(spin([&] {
        const auto b = backend("flatpak");
        return b.contains("available") && b.at("reason").value<std::string>() != "not checked yet";
    }));
    EXPECT_EQ(enableSource(kSpare).error, kBackendUnavailable);
    EXPECT_EQ(disableSource(kFlathubUser).error, kBackendUnavailable);
    // The catalogue still lists its sources, as not configured.
    const auto user = source(kFlathubUser);
    ASSERT_TRUE(user);
    EXPECT_FALSE(user->at("configured").value<bool>(true));
    EXPECT_FALSE(user->at("can-toggle").value<bool>(true));
}

} // namespace
