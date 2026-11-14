// The service with two scripted backends (H.1): the Backends property lists
// both, one backend's unavailability does not degrade the other, the service
// degrades only when all are unavailable, and an action selects the backend
// its offer names. H.2: GetInstalled, GetUpdates, UpdateCount and their
// signals over both backends, through D-Bus. In-process (no service binary),
// on a private bus, with a prebuilt catalogue; no package manager is
// touched.

#include "fakebackend.h"
#include "storeservice.h"

#include "catalogfixture.h"
#include "privatebus.h"

#include <base/dbus.h>
#include <base/event_loop.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <future>
#include <atomic>
#include <functional>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cutefish::store;
using namespace cutefish::store::service;
using namespace cutefish::store::service::testutil;
namespace base = cutefish::base;
namespace dbus = cutefish::base::dbus;
using namespace std::chrono_literals;

namespace {

struct Fetch
{
    bool done = false;
    std::optional<dbus::VariantMap> map;
    std::optional<dbus::Reply> reply;
};

class ServiceFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        catalogfixture::build(m_dir / "catalog.sqlite");
        m_conn = dbus::Connection::open(dbus::Connection::Bus::Session, &m_loop);
        ASSERT_TRUE(m_conn);
        ServiceOptions options;
        options.dataDir = m_dir.path();
        options.sourcesFile = m_dir / "sources.conf";
        options.indexer = "cutefish-store-indexer";
        options.readLocales = {"C"};
        options.busName = "org.cutefish.Store";
        m_service = std::make_unique<StoreService>(m_loop, *m_conn, std::move(options), makeBackends());
        ASSERT_TRUE(m_service->start()) << "service did not start";
    }

    /// The backends the fixture registers; overridden per test.
    virtual std::vector<std::unique_ptr<Backend>> makeBackends() = 0;

    bool spin(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 5s)
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

    std::optional<dbus::VariantMap> properties()
    {
        auto fetch = std::make_shared<Fetch>();
        std::vector<dbus::PendingCall> calls;
        calls.push_back(store().getAllAsync([fetch](const dbus::Reply& reply) {
            fetch->map = reply.value<dbus::VariantMap>();
            fetch->done = true;
        }));
        spin([&] { return fetch->done; });
        return fetch->map;
    }

    /// Until a catalogue is in service (State ready or degraded): before
    /// it, actions are refused with NotReady whatever the backend.
    bool waitReady()
    {
        return spin([&] {
            const auto props = properties();
            if (!props || !props->contains("State"))
                return false;
            const std::string state = props->at("State").value<std::string>();
            return state == "ready" || state == "degraded";
        });
    }

    std::optional<dbus::Reply> install(const std::string& offer)
    {
        auto fetch = std::make_shared<Fetch>();
        std::vector<dbus::PendingCall> calls;
        calls.push_back(store().callAsync("Install", [fetch](const dbus::Reply& reply) {
            fetch->reply = reply;
            fetch->done = true;
        }, offer, dbus::VariantMap{}));
        spin([&] { return fetch->done; });
        return fetch->reply;
    }

    std::optional<dbus::VariantMap> transaction(const std::string& path)
    {
        auto fetch = std::make_shared<Fetch>();
        std::vector<dbus::PendingCall> calls;
        calls.push_back(dbus::Proxy(m_conn.get(), "org.cutefish.Store", path, "org.cutefish.Store1.Transaction")
                                .getAllAsync([fetch](const dbus::Reply& reply) {
                                    fetch->map = reply.value<dbus::VariantMap>();
                                    fetch->done = true;
                                }));
        spin([&] { return fetch->done; });
        return fetch->map;
    }

    dbus::Proxy store() const
    {
        return dbus::Proxy(m_conn.get(), "org.cutefish.Store", "/org/cutefish/Store", "org.cutefish.Store1");
    }

    base::EventLoop& loop() { return m_loop; }

    privatebus::PrivateBus m_bus;
    base::EventLoop m_loop;
    std::unique_ptr<dbus::Connection> m_conn;
    catalogfixture::TempDir m_dir;
    std::unique_ptr<StoreService> m_service;
};

class TwoAvailableBackends : public ServiceFixture
{
    std::vector<std::unique_ptr<Backend>> makeBackends() override
    {
        std::vector<std::unique_ptr<Backend>> backends;
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::PackageKit));
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::Flatpak));
        return backends;
    }
};

TEST_F(TwoAvailableBackends, ListsBothBackends)
{
    std::optional<dbus::VariantMap> props;
    ASSERT_TRUE(spin([&] {
        props = properties();
        if (!props)
            return false;
        const auto it = props->find("State");
        return it != props->end() && it->second.get<std::string>().value_or("") == "ready";
    }));

    const auto backends = props->at("Backends").value<std::vector<dbus::VariantMap>>();
    ASSERT_EQ(backends.size(), 2u);
    const dbus::VariantMap* packagekit = nullptr;
    const dbus::VariantMap* flatpak = nullptr;
    for (const auto& entry : backends)
        (entry.at("id").value<std::string>() == "packagekit" ? packagekit : flatpak) = &entry;
    ASSERT_TRUE(packagekit);
    ASSERT_TRUE(flatpak);
    EXPECT_EQ(packagekit->at("available").value<bool>(false), true);
    EXPECT_EQ(packagekit->at("formats").value<std::vector<std::string>>(), std::vector<std::string>{"deb"});
    EXPECT_EQ(flatpak->at("available").value<bool>(false), true);
    EXPECT_EQ(flatpak->at("formats").value<std::vector<std::string>>(), std::vector<std::string>{"flatpak"});
    EXPECT_EQ(flatpak->at("scopes").value<std::vector<std::string>>(),
              (std::vector<std::string>{"system", "user"}));
}

TEST_F(TwoAvailableBackends, SelectsTheBackendEachOfferNames)
{
    ASSERT_TRUE(waitReady());
    // Both backends answered (an action on one that has not is refused).
    ASSERT_TRUE(spin([&] {
        const auto props = properties();
        if (!props)
            return false;
        const auto list = props->at("Backends").value<std::vector<dbus::VariantMap>>();
        return list.size() == 2 && std::all_of(list.begin(), list.end(), [](const dbus::VariantMap& b) {
                   return b.at("available").value<bool>(false);
               });
    }));

    // Two offers of the same application, from two sources: each names its
    // own backend, and the selection must not fall back.
    auto deb = install("packagekit:gimp");
    ASSERT_TRUE(deb) << "Install(packagekit:gimp) was not answered";
    dbus::ObjectPath debPath;
    ASSERT_TRUE(deb->read(debPath)) << (deb->isError() ? deb->error().name + ": " + deb->error().message : "");
    auto debTx = transaction(debPath.path);
    ASSERT_TRUE(debTx);
    EXPECT_EQ(debTx->at("Backend").value<std::string>(), "packagekit");

    auto flatpak = install("flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable");
    ASSERT_TRUE(flatpak);
    dbus::ObjectPath flatpakPath;
    ASSERT_TRUE(flatpak->read(flatpakPath));
    auto flatpakTx = transaction(flatpakPath.path);
    ASSERT_TRUE(flatpakTx);
    EXPECT_EQ(flatpakTx->at("Backend").value<std::string>(), "flatpak");
}

class OneAvailableOneUnavailable : public ServiceFixture
{
    std::vector<std::unique_ptr<Backend>> makeBackends() override
    {
        std::vector<std::unique_ptr<Backend>> backends;
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::PackageKit));
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::Flatpak,
                                                         Error::backend(BackendFailure::Unavailable, "no flatpak")));
        return backends;
    }
};

TEST_F(OneAvailableOneUnavailable, OneUnavailableBackendDoesNotDegradeTheService)
{
    std::optional<dbus::VariantMap> props;
    ASSERT_TRUE(spin([&] {
        props = properties();
        if (!props)
            return false;
        const auto it = props->find("State");
        return it != props->end() && it->second.get<std::string>().value_or("") == "ready";
    }));
    EXPECT_EQ(props->at("State").value<std::string>(), "ready");

    const auto backends = props->at("Backends").value<std::vector<dbus::VariantMap>>();
    ASSERT_EQ(backends.size(), 2u);
    for (const auto& entry : backends) {
        if (entry.at("id").value<std::string>() == "flatpak") {
            EXPECT_EQ(entry.at("available").value<bool>(true), false);
            EXPECT_EQ(entry.at("reason").value<std::string>(), "no flatpak");
        } else {
            EXPECT_EQ(entry.at("available").value<bool>(false), true);
        }
    }
}

TEST_F(OneAvailableOneUnavailable, AFlatpakOfferIsRefusedWhenFlatpakIsUnavailable)
{
    ASSERT_TRUE(waitReady());
    const auto reply = install("flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable");
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->isError() ? reply->error().name : std::string("<no error>"),
              "org.cutefish.Store1.Error.BackendUnavailable");
}

class AllUnavailable : public ServiceFixture
{
    std::vector<std::unique_ptr<Backend>> makeBackends() override
    {
        std::vector<std::unique_ptr<Backend>> backends;
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::PackageKit,
                                                         Error::backend(BackendFailure::Unavailable, "no packagekit")));
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::Flatpak,
                                                         Error::backend(BackendFailure::Unavailable, "no flatpak")));
        return backends;
    }
};

TEST_F(AllUnavailable, DegradesWhenEveryBackendIsUnavailable)
{
    std::optional<dbus::VariantMap> props;
    ASSERT_TRUE(spin([&] {
        props = properties();
        if (!props)
            return false;
        const auto it = props->find("State");
        return it != props->end() && it->second.get<std::string>().value_or("") == "degraded";
    }));
    EXPECT_EQ(props->at("State").value<std::string>(), "degraded");
}

// --- H.2: the unified inventory over D-Bus ---------------------------------------------

constexpr const char* kPath = "/org/cutefish/Store";
constexpr const char* kInterface = "org.cutefish.Store1";
constexpr const char* kGimpFlathub = "flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable";
constexpr const char* kGimpSystem = "flatpak:system:flathub:app/org.gimp.GIMP/x86_64/stable";
constexpr const char* kGimpNightly = "flatpak:user:gnome-nightly:app/org.gimp.GIMP/x86_64/stable";
constexpr const char* kQgisLts = "flatpak:user:flathub:app/org.qgis.qgis/x86_64/lts";
constexpr const char* kQgisStable = "flatpak:user:flathub:app/org.qgis.qgis/x86_64/stable";
constexpr const char* kPlatformUser = "flatpak:user:flathub:runtime/org.freedesktop.Platform/x86_64/24.08";
constexpr const char* kPlatformSystem = "flatpak:system:flathub:runtime/org.freedesktop.Platform/x86_64/24.08";

template <typename T>
T must(Result<T> r)
{
    EXPECT_TRUE(r) << (r ? "" : r.error().message());
    return std::move(r).value();
}

InstalledItem debItem(const std::string& package)
{
    return must(InstalledItem::create({.offer = must(OfferId::parse("packagekit:" + package)),
                                       .backend = BackendKind::PackageKit,
                                       .scope = Scope::System,
                                       .version = must(Version::debian("1.0-1")),
                                       .origin = "debian-stable-main"}));
}

InstalledItem flatpakItem(const std::string& id)
{
    const OfferId offer = must(OfferId::parse(id));
    auto address = must(decodeOfferId(offer));
    return must(InstalledItem::create({.offer = offer,
                                       .backend = BackendKind::Flatpak,
                                       .scope = address.scope,
                                       .origin = address.origin}));
}

AvailableUpdate update(const std::string& id, PlanItemKind kind = PlanItemKind::Application, const char* to = nullptr)
{
    AvailableUpdate u{.offer = must(OfferId::parse(id)), .kind = kind};
    if (to)
        u.toVersion = id.starts_with("packagekit:") ? must(Version::debian(to)) : must(Version::upstream(to));
    return u;
}

dbus::MatchRule rule(const std::string& interface, const std::string& member)
{
    dbus::MatchRule r;
    r.path = kPath;
    r.interface = interface;
    r.member = member;
    return r;
}

/// Both backends, with installations and updates of the same applications
/// (GIMP: deb, Flathub user and system, another remote; QGIS: two branches).
class ScriptedBackends : public ServiceFixture
{
protected:
    virtual bool flatpakAvailable() const { return true; }

    std::vector<std::unique_ptr<Backend>> makeBackends() override
    {
        auto pk = std::make_unique<FakeBackend>(loop(), BackendKind::PackageKit);
        pk->setInstalled({debItem("gimp"), debItem("libfoo1")});
        pk->setUpdates({update("packagekit:gimp", PlanItemKind::Application, "2.10.38-2"),
                        update("packagekit:gimp", PlanItemKind::Application, "2.10.38-2"),
                        update("packagekit:libfoo1")}); // no application: left out (D-010)
        auto fp = std::make_unique<FakeBackend>(
                loop(), BackendKind::Flatpak,
                flatpakAvailable() ? Result<void>::success()
                                   : Result<void>(Error::backend(BackendFailure::Unavailable, "no flatpak")));
        fp->setInstalled({flatpakItem(kGimpFlathub), flatpakItem(kGimpSystem), flatpakItem(kGimpNightly),
                          flatpakItem(kQgisLts)});
        fp->setUpdates({update(kQgisStable, PlanItemKind::Application, "3.42"), update(kGimpFlathub),
                        update(kQgisLts, PlanItemKind::Application, "3.34.2"),
                        update(kPlatformSystem, PlanItemKind::Runtime), update(kPlatformUser, PlanItemKind::Runtime)});
        m_pk = pk.get();
        m_fp = fp.get();
        std::vector<std::unique_ptr<Backend>> backends;
        backends.push_back(std::move(pk));
        backends.push_back(std::move(fp));
        return backends;
    }

    std::optional<std::vector<dbus::VariantMap>> list(const char* method, bool withOptions)
    {
        auto fetch = std::make_shared<std::optional<std::vector<dbus::VariantMap>>>();
        auto done = std::make_shared<bool>(false);
        std::vector<dbus::PendingCall> calls;
        auto cb = [fetch, done](const dbus::Reply& reply) {
            if (!reply.isError())
                *fetch = reply.value<std::vector<dbus::VariantMap>>();
            *done = true;
        };
        if (withOptions)
            calls.push_back(store().callAsync(method, cb, dbus::VariantMap{}));
        else
            calls.push_back(store().callAsync(method, cb));
        spin([&] { return *done; });
        return *fetch;
    }

    std::vector<dbus::VariantMap> installed() { return list("GetInstalled", true).value_or(std::vector<dbus::VariantMap>{}); }
    std::vector<dbus::VariantMap> updates() { return list("GetUpdates", false).value_or(std::vector<dbus::VariantMap>{}); }

    std::optional<std::uint32_t> updateCount()
    {
        auto props = properties();
        if (!props || !props->contains("UpdateCount"))
            return std::nullopt;
        return props->at("UpdateCount").value<std::uint32_t>();
    }

    static std::vector<std::string> offers(const std::vector<dbus::VariantMap>& items)
    {
        std::vector<std::string> out;
        for (const auto& i : items)
            out.push_back(i.at("offer-id").value<std::string>());
        return out;
    }

    /// The updates once both backends answered (6 with this data).
    bool settled(std::size_t expected)
    {
        return spin([&] { return updateCount() == std::optional<std::uint32_t>(static_cast<std::uint32_t>(expected)); });
    }

    FakeBackend* m_pk = nullptr;
    FakeBackend* m_fp = nullptr;
};

TEST_F(ScriptedBackends, GetInstalledListsEveryInstallationWithItsProvenance)
{
    std::vector<dbus::VariantMap> items;
    // Six rows, linked: the inventory may be recorded before the catalogue
    // is in service, and linked by the next reconciliation.
    ASSERT_TRUE(spin([&] {
        items = installed();
        if (items.size() != 6)
            return false;
        for (const auto& i : items) {
            const std::string offer = i.at("offer-id").value<std::string>();
            if ((offer == kGimpFlathub || offer == "packagekit:gimp")
                && i.at("app-id").value<std::string>() != "org.gimp.GIMP")
                return false;
        }
        return true;
    }));
    // Sorted by offer id; GIMP four times, never merged on its AppStream id.
    EXPECT_EQ(offers(items), (std::vector<std::string>{kGimpSystem, kGimpFlathub, kQgisLts, kGimpNightly,
                                                       "packagekit:gimp", "packagekit:libfoo1"}));
    auto field = [&](std::size_t i, const char* key) { return items[i].at(key).value<std::string>(); };
    EXPECT_EQ(field(0, "backend"), "flatpak");
    EXPECT_EQ(field(0, "scope"), "system");
    EXPECT_EQ(field(0, "origin"), "flathub");
    EXPECT_EQ(field(1, "scope"), "user");
    EXPECT_EQ(field(1, "app-id"), "org.gimp.GIMP"); // linked with the catalogue
    EXPECT_EQ(field(3, "origin"), "gnome-nightly");
    EXPECT_EQ(field(3, "app-id"), ""); // that remote is not in the catalogue
    EXPECT_EQ(field(4, "backend"), "packagekit");
    EXPECT_EQ(field(4, "app-id"), "org.gimp.GIMP");
    EXPECT_EQ(field(4, "version"), "1.0-1");
    // The backend option still filters, and the answer does not change
    // between calls.
    std::vector<dbus::VariantMap> flatpakOnly;
    auto done = std::make_shared<bool>(false);
    std::vector<dbus::PendingCall> calls;
    calls.push_back(store().callAsync("GetInstalled", [&, done](const dbus::Reply& reply) {
        flatpakOnly = reply.value<std::vector<dbus::VariantMap>>().value_or(std::vector<dbus::VariantMap>{});
        *done = true;
    }, dbus::VariantMap{{"backend", dbus::Variant(std::string("flatpak"))}}));
    ASSERT_TRUE(spin([&] { return *done; }));
    EXPECT_EQ(flatpakOnly.size(), 4u);
    EXPECT_EQ(offers(installed()), offers(items));
}

TEST_F(ScriptedBackends, GetUpdatesAndUpdateCountAgreeOverBothBackends)
{
    ASSERT_TRUE(settled(6));
    const auto list = updates();
    // One entry per offer: the duplicate deb is counted once, GIMP from two
    // backends twice, QGIS on two branches twice, the runtime in two scopes
    // twice.
    EXPECT_EQ(offers(list), (std::vector<std::string>{kPlatformSystem, kGimpFlathub, kQgisLts, kQgisStable,
                                                      kPlatformUser, "packagekit:gimp"}));
    EXPECT_EQ(updateCount(), std::optional<std::uint32_t>(static_cast<std::uint32_t>(list.size())));
    EXPECT_EQ(list[0].at("backend").value<std::string>(), "flatpak");
    EXPECT_EQ(list[0].at("kind").value<std::string>(), "runtime");
    EXPECT_EQ(list[2].at("to-version").value<std::string>(), "3.34.2");
    EXPECT_EQ(list[3].at("to-version").value<std::string>(), "3.42");
    EXPECT_EQ(list[5].at("backend").value<std::string>(), "packagekit");
    EXPECT_EQ(list[5].at("app-id").value<std::string>(), "org.gimp.GIMP");
    EXPECT_EQ(list[5].at("to-version").value<std::string>(), "2.10.38-2");
    EXPECT_EQ(offers(updates()), offers(list)); // deterministic
}

TEST_F(ScriptedBackends, ChangesFromEachBackendAreSignalledOnceAndOnlyWhenTheyChangeSomething)
{
    ASSERT_TRUE(settled(6));
    std::vector<std::vector<std::string>> installedSignals;
    std::vector<std::uint32_t> updatesSignals;
    int countChanges = 0;
    std::vector<dbus::Match> matches;
    matches.push_back(m_conn->subscribe(rule(kInterface, "InstalledChanged"), [&](const dbus::Message& m) {
        std::vector<std::string> apps;
        if (m.read(apps))
            installedSignals.push_back(apps);
    }));
    matches.push_back(m_conn->subscribe(rule(kInterface, "UpdatesChanged"), [&](const dbus::Message& m) {
        std::uint32_t count = 0;
        if (m.read(count))
            updatesSignals.push_back(count);
    }));
    matches.push_back(m_conn->subscribe(rule("org.freedesktop.DBus.Properties", "PropertiesChanged"),
                                        [&](const dbus::Message& m) {
                                            std::string iface;
                                            dbus::VariantMap changed;
                                            std::vector<std::string> invalidated;
                                            if (m.read(iface, changed, invalidated) && changed.contains("UpdateCount"))
                                                ++countChanges;
                                        }));

    // PackageKit: `apt install gitg` outside the store.
    m_pk->setInstalled({debItem("gimp"), debItem("libfoo1"), debItem("gitg")});
    m_pk->emitChange({.installed = true});
    ASSERT_TRUE(spin([&] { return installedSignals.size() == 1; }));
    EXPECT_EQ(installedSignals[0], std::vector<std::string>{"org.gnome.gitg"});

    // Flatpak: the same updates in another order, then one fewer. Only the
    // second is a change: one UpdatesChanged, carrying the new total.
    m_fp->setUpdates({update(kPlatformUser, PlanItemKind::Runtime), update(kPlatformSystem, PlanItemKind::Runtime),
                      update(kQgisLts, PlanItemKind::Application, "3.34.2"), update(kGimpFlathub),
                      update(kQgisStable, PlanItemKind::Application, "3.42")});
    m_fp->emitChange({.updates = true});
    m_fp->setUpdates({update(kGimpFlathub), update(kQgisLts, PlanItemKind::Application, "3.34.2"),
                      update(kQgisStable, PlanItemKind::Application, "3.42"),
                      update(kPlatformSystem, PlanItemKind::Runtime), update(kPlatformUser, PlanItemKind::Runtime),
                      update("flatpak:user:flathub:app/org.inkscape.Inkscape/x86_64/stable")});
    m_fp->emitChange({.updates = true});
    ASSERT_TRUE(spin([&] { return updatesSignals.size() == 1 && countChanges == 1; }));
    EXPECT_EQ(updatesSignals[0], 7u);
    EXPECT_EQ(updateCount(), std::optional<std::uint32_t>(7));
    EXPECT_EQ(updates().size(), 7u);

    // A list that changes but keeps the total: UpdatesChanged, and the
    // UpdateCount property is not re-emitted.
    m_fp->setUpdates({update(kGimpFlathub), update(kQgisLts, PlanItemKind::Application, "3.34.3"),
                      update(kQgisStable, PlanItemKind::Application, "3.42"),
                      update(kPlatformSystem, PlanItemKind::Runtime), update(kPlatformUser, PlanItemKind::Runtime),
                      update("flatpak:user:flathub:app/org.inkscape.Inkscape/x86_64/stable")});
    m_fp->emitChange({.updates = true});
    ASSERT_TRUE(spin([&] { return updatesSignals.size() == 2; }));
    EXPECT_EQ(updatesSignals[1], 7u);
    // The PropertiesChanged would have been sent before UpdatesChanged.
    EXPECT_EQ(countChanges, 1);
    EXPECT_EQ(installedSignals.size(), 1u); // nothing else was signalled
}

class ScriptedFlatpakUnavailable : public ScriptedBackends
{
protected:
    bool flatpakAvailable() const override { return false; }
};

TEST_F(ScriptedFlatpakUnavailable, TheOtherBackendStaysListedAndCounted)
{
    ASSERT_TRUE(settled(1));
    std::vector<dbus::VariantMap> items;
    ASSERT_TRUE(spin([&] {
        items = installed();
        return items.size() == 2;
    }));
    EXPECT_EQ(offers(items), (std::vector<std::string>{"packagekit:gimp", "packagekit:libfoo1"}));
    EXPECT_EQ(offers(updates()), std::vector<std::string>{"packagekit:gimp"});
    // Consistent with the Backends property.
    const auto props = properties();
    ASSERT_TRUE(props);
    EXPECT_EQ(props->at("State").value<std::string>(), "ready");
    for (const auto& entry : props->at("Backends").value<std::vector<dbus::VariantMap>>())
        EXPECT_EQ(entry.at("available").value<bool>(false), entry.at("id").value<std::string>() == "packagekit");
}

TEST_F(ScriptedFlatpakUnavailable, ABackendThatBecomesUsableJoinsTheCount)
{
    ASSERT_TRUE(settled(1));
    int backendsChanges = 0;
    std::vector<std::uint32_t> updatesSignals;
    std::vector<dbus::Match> matches;
    matches.push_back(m_conn->subscribe(rule(kInterface, "UpdatesChanged"), [&](const dbus::Message& m) {
        std::uint32_t count = 0;
        if (m.read(count))
            updatesSignals.push_back(count);
    }));
    matches.push_back(m_conn->subscribe(rule("org.freedesktop.DBus.Properties", "PropertiesChanged"),
                                        [&](const dbus::Message& m) {
                                            std::string iface;
                                            dbus::VariantMap changed;
                                            std::vector<std::string> invalidated;
                                            if (m.read(iface, changed, invalidated) && changed.contains("Backends"))
                                                ++backendsChanges;
                                        }));
    // The daemon came back: a change report makes the inventory ask again.
    m_fp->setAvailability(Result<void>::success());
    m_fp->emitChange({.installed = true});
    ASSERT_TRUE(settled(6));
    ASSERT_TRUE(spin([&] { return installed().size() == 6; }));
    ASSERT_TRUE(spin([&] { return !updatesSignals.empty(); }));
    EXPECT_EQ(updatesSignals.back(), 6u);
    EXPECT_EQ(updatesSignals.size(), 1u);
    EXPECT_EQ(backendsChanges, 1);
}

} // namespace

// --- H.2 final audit: a new instance right after an idle shutdown ------------------------
//
// Instance A goes idle while its inventory write is held on its I/O thread,
// before it reaches SQLite (where its long waits are: the busy timeout, the
// catalogue read; inside its transaction it only writes, for milliseconds —
// that case is covered in testinventory.cpp, AWriteOfAStoppedInstance…);
// as main.cpp does, onIdle releases the bus name (StoreService stops its
// inventory writes first). A is destroyed with the write still held.
// Instance B takes the name at once and records a newer inventory. Only then
// does A's thread go on. Ordered by barriers on A's thread, not by delays.

namespace {

InstalledItem debInstalled(const std::string& package)
{
    return must(InstalledItem::create({.offer = must(OfferId::parse("packagekit:" + package)),
                                       .backend = BackendKind::PackageKit,
                                       .scope = Scope::System,
                                       .version = must(Version::debian("1.0-1")),
                                       .origin = "debian-stable-main"}));
}

ServiceOptions instanceOptions(const std::filesystem::path& dir)
{
    ServiceOptions options;
    options.dataDir = dir;
    options.sourcesFile = dir / "sources.conf";
    options.indexer = "cutefish-store-indexer";
    options.readLocales = {"C"};
    options.busName = "org.cutefish.Store";
    return options;
}

} // namespace

TEST(ServiceRestart, ANewInstanceNeverSeesTheOldOnesLateWrite)
{
    privatebus::PrivateBus bus;
    base::EventLoop loop;
    catalogfixture::TempDir dir;
    catalogfixture::build(dir / "catalog.sqlite");
    auto conn = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);
    ASSERT_TRUE(conn);
    auto spin = [&](const std::function<bool()>& pred) {
        const auto end = std::chrono::steady_clock::now() + 10s;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            const int t = loop.startTimer(5ms, false, [&] { loop.quit(0); });
            loop.run();
            loop.stopTimer(t);
        }
        return true;
    };

    // Instance A: idle after 50 ms; its first inventory write is held.
    auto reachedA = std::make_shared<std::promise<void>>();
    auto openA = std::make_shared<std::promise<void>>();
    std::shared_future<void> reached = reachedA->get_future().share();
    std::shared_future<void> opened = openA->get_future().share();
    auto endedA = std::make_shared<std::promise<void>>();
    std::shared_future<void> aEnded = endedA->get_future().share();
    ServiceOptions a = instanceOptions(dir.path());
    a.idleTimeout = 50ms;
    a.inventoryIo.hooks.beforeWrite = [reachedA, opened, used = std::make_shared<std::atomic<bool>>(false)] {
        if (used->exchange(true))
            return;
        reachedA->set_value();
        opened.wait();
    };
    a.inventoryIo.hooks.threadEnded = [endedA] { endedA->set_value(); };
    std::vector<std::unique_ptr<Backend>> aBackends;
    auto aPk = std::make_unique<FakeBackend>(loop, BackendKind::PackageKit);
    aPk->setInstalled({debInstalled("gimp")});
    aBackends.push_back(std::move(aPk));
    auto serviceA = std::make_unique<StoreService>(loop, *conn, std::move(a), std::move(aBackends));
    bool idle = false;
    serviceA->onIdle = [&] {
        conn->releaseName("org.cutefish.Store"); // as main.cpp
        idle = true;
    };
    ASSERT_TRUE(serviceA->start());
    // A's write is held, about to reach SQLite.
    ASSERT_TRUE(spin([&] { return reached.wait_for(0s) == std::future_status::ready; }));
    ASSERT_TRUE(spin([&] { return idle; }));
    const auto before = std::chrono::steady_clock::now();
    serviceA.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 1s); // no wait for the held write

    // Instance B, at once: the name is free, the write lock is A's.
    ServiceOptions b = instanceOptions(dir.path());
    b.inventoryIo.busyTimeout = 20ms;
    b.inventoryIo.retryDelays = std::vector<std::chrono::milliseconds>(100, 20ms);
    std::vector<std::unique_ptr<Backend>> bBackends;
    auto bPk = std::make_unique<FakeBackend>(loop, BackendKind::PackageKit);
    bPk->setInstalled({debInstalled("gitg")});
    bBackends.push_back(std::move(bPk));
    auto serviceB = std::make_unique<StoreService>(loop, *conn, std::move(b), std::move(bBackends));
    const auto startB = std::chrono::steady_clock::now();
    ASSERT_TRUE(serviceB->start()) << "the restart must not wait for the old write";
    EXPECT_LT(std::chrono::steady_clock::now() - startB, 1s);
    auto installed = [&] {
        auto out = std::make_shared<std::vector<std::string>>();
        auto done = std::make_shared<bool>(false);
        std::vector<dbus::PendingCall> calls;
        calls.push_back(dbus::Proxy(conn.get(), "org.cutefish.Store", "/org/cutefish/Store", "org.cutefish.Store1")
                                .callAsync("GetInstalled", [out, done](const dbus::Reply& r) {
                                    for (const auto& i : r.value<std::vector<dbus::VariantMap>>().value_or(
                                                 std::vector<dbus::VariantMap>{}))
                                        out->push_back(i.at("offer-id").value<std::string>());
                                    *done = true;
                                }, dbus::VariantMap{}));
        spin([&] { return *done; });
        return *out;
    };
    // B records its inventory while A's write is still held.
    ASSERT_TRUE(spin([&] { return installed() == std::vector<std::string>{"packagekit:gitg"}; }));
    ASSERT_NE(aEnded.wait_for(0s), std::future_status::ready);

    // Only now does A's thread go on: stopped, it writes nothing.
    openA->set_value();
    ASSERT_EQ(aEnded.wait_for(10s), std::future_status::ready);
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(installed(), std::vector<std::string>{"packagekit:gitg"});
    serviceB.reset();
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open((dir / "state.sqlite").c_str(), &db), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)), "ok");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

TEST(ServiceRestart, AnOldInstanceStillAliveAfterReleasingItsNameWritesNothing)
{
    // As in production: once A has released its name it still exists for a
    // moment (its loop ends, then it is destroyed), while B, another
    // process, may already run. A's held write goes on during that moment.
    privatebus::PrivateBus bus;
    base::EventLoop loop;
    catalogfixture::TempDir dir;
    catalogfixture::build(dir / "catalog.sqlite");
    auto connA = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);
    auto connB = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);
    ASSERT_TRUE(connA && connB);
    auto spin = [&](const std::function<bool()>& pred) {
        const auto end = std::chrono::steady_clock::now() + 10s;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            const int t = loop.startTimer(5ms, false, [&] { loop.quit(0); });
            loop.run();
            loop.stopTimer(t);
        }
        return true;
    };
    auto reachedA = std::make_shared<std::promise<void>>();
    auto openA = std::make_shared<std::promise<void>>();
    std::shared_future<void> reached = reachedA->get_future().share();
    std::shared_future<void> opened = openA->get_future().share();
    auto endedA = std::make_shared<std::promise<void>>();
    std::shared_future<void> aEnded = endedA->get_future().share();
    std::atomic<int> aWriteJobs = 0;
    ServiceOptions a = instanceOptions(dir.path());
    a.idleTimeout = 50ms;
    a.inventoryIo.hooks.beforeWrite = [reachedA, opened, &aWriteJobs] {
        if (++aWriteJobs != 1)
            return;
        reachedA->set_value();
        opened.wait();
    };
    a.inventoryIo.hooks.threadEnded = [endedA] { endedA->set_value(); };
    std::vector<std::unique_ptr<Backend>> aBackends;
    auto aPk = std::make_unique<FakeBackend>(loop, BackendKind::PackageKit);
    aPk->setInstalled({debInstalled("gimp")});
    aBackends.push_back(std::move(aPk));
    auto serviceA = std::make_unique<StoreService>(loop, *connA, std::move(a), std::move(aBackends));
    bool idle = false;
    serviceA->onIdle = [&] {
        connA->releaseName("org.cutefish.Store"); // as main.cpp
        idle = true;
    };
    ASSERT_TRUE(serviceA->start());
    ASSERT_TRUE(spin([&] { return reached.wait_for(0s) == std::future_status::ready; }));
    ASSERT_TRUE(spin([&] { return idle; }));

    // B, on its own connection, while A still exists.
    std::vector<std::unique_ptr<Backend>> bBackends;
    auto bPk = std::make_unique<FakeBackend>(loop, BackendKind::PackageKit);
    bPk->setInstalled({debInstalled("gitg")});
    bBackends.push_back(std::move(bPk));
    auto serviceB = std::make_unique<StoreService>(loop, *connB, instanceOptions(dir.path()), std::move(bBackends));
    ASSERT_TRUE(serviceB->start());
    auto installed = [&] {
        auto out = std::make_shared<std::vector<std::string>>();
        auto done = std::make_shared<bool>(false);
        std::vector<dbus::PendingCall> calls;
        calls.push_back(dbus::Proxy(connB.get(), "org.cutefish.Store", "/org/cutefish/Store", "org.cutefish.Store1")
                                .callAsync("GetInstalled", [out, done](const dbus::Reply& r) {
                                    for (const auto& i : r.value<std::vector<dbus::VariantMap>>().value_or(
                                                 std::vector<dbus::VariantMap>{}))
                                        out->push_back(i.at("offer-id").value<std::string>());
                                    *done = true;
                                }, dbus::VariantMap{}));
        spin([&] { return *done; });
        return *out;
    };
    ASSERT_TRUE(spin([&] { return installed() == std::vector<std::string>{"packagekit:gitg"}; }));

    // A's write goes on while A is alive: stopped at idle, it writes
    // nothing, and A's loop gets nothing to apply either.
    openA->set_value();
    ASSERT_TRUE(spin([&] { return aWriteJobs.load() >= 1; }));
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(installed(), std::vector<std::string>{"packagekit:gitg"});
    serviceA.reset();
    ASSERT_EQ(aEnded.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(installed(), std::vector<std::string>{"packagekit:gitg"});
    serviceB.reset();
}
