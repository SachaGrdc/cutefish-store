// The unified inventory of several backends (H.2), without D-Bus: the
// normalisation of what a backend lists, the aggregation of the updates
// (what GetUpdates lists and UpdateCount counts), the installed state that
// two inventories record into one state.sqlite, their notifications, their
// destruction, and the concurrency of their writes with each other and with
// another connection. Scripted backends (fakebackend.h), the prebuilt
// fixture catalogue, real SQLite files; no package manager.
//
// Synchronisation: every wait is on a condition (spin() runs the loop until
// it holds, with a generous deadline that only turns a hang into a
// failure). The two fixed delays below are not used to order anything: they
// are how long a lock is held, and the assertions hold whatever its length.

#include "backends.h"
#include "fakebackend.h"
#include "inventory.h"

#include "catalogfixture.h"
#include "fixtures.h"

#include <cutefish/store/statestore.h>

#include <base/event_loop.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace cutefish::store;
using namespace cutefish::store::service;
using namespace cutefish::store::service::testutil;
namespace base = cutefish::base;
using fixtures::unwrap;
using namespace std::chrono_literals;

namespace {

constexpr const char* kGimpFlathub = "flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable";
constexpr const char* kQgisLts = "flatpak:user:flathub:app/org.qgis.qgis/x86_64/lts";
constexpr const char* kQgisStable = "flatpak:user:flathub:app/org.qgis.qgis/x86_64/stable";
constexpr const char* kPlatformUser = "flatpak:user:flathub:runtime/org.freedesktop.Platform/x86_64/24.08";
constexpr const char* kPlatformSystem = "flatpak:system:flathub:runtime/org.freedesktop.Platform/x86_64/24.08";

OfferId offer(const std::string& id)
{
    return unwrap(OfferId::parse(id));
}

InstalledItem debItem(const std::string& package, const char* version = "1.0-1")
{
    return unwrap(InstalledItem::create({.offer = offer("packagekit:" + package),
                                         .backend = BackendKind::PackageKit,
                                         .scope = Scope::System,
                                         .version = unwrap(Version::debian(version)),
                                         .origin = "debian-stable-main"}));
}

InstalledItem flatpakItem(Scope scope, const std::string& remote, const std::string& ref)
{
    return unwrap(InstalledItem::create({.offer = offer("flatpak:" + std::string(toString(scope)) + ":" + remote
                                                        + ":" + ref),
                                         .backend = BackendKind::Flatpak,
                                         .scope = scope,
                                         .origin = remote}));
}

AvailableUpdate debUpdate(const std::string& package, const char* from = "1.0-1", const char* to = "1.1-1")
{
    return {.offer = offer("packagekit:" + package),
            .kind = PlanItemKind::Application,
            .fromVersion = unwrap(Version::debian(from)),
            .toVersion = unwrap(Version::debian(to))};
}

AvailableUpdate flatpakUpdate(const std::string& id, PlanItemKind kind = PlanItemKind::Application,
                              const char* to = nullptr)
{
    AvailableUpdate u{.offer = offer(id), .kind = kind};
    if (to)
        u.toVersion = unwrap(Version::upstream(to));
    return u;
}

std::vector<std::string> offers(const std::vector<InstalledItem>& items)
{
    std::vector<std::string> out;
    for (const InstalledItem& i : items)
        out.push_back(i.offer().str());
    return out;
}

std::vector<std::string> offers(const std::vector<BackendUpdate>& updates)
{
    std::vector<std::string> out;
    for (const BackendUpdate& u : updates)
        out.push_back(u.update->offer.str());
    return out;
}

std::string rawQuery(const std::filesystem::path& path, const char* sql)
{
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    std::string out;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        out = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

// --- Normalisation (pure) ------------------------------------------------------------

TEST(InventoryNormalisation, InstalledItemsOfAnotherBackendAreLeftOut)
{
    std::vector<InstalledItem> items{debItem("gimp"), flatpakItem(Scope::User, "flathub", "app/org.gimp.GIMP/x86_64/stable")};
    EXPECT_EQ(offers(normalizeInstalled(BackendKind::PackageKit, items)), std::vector<std::string>{"packagekit:gimp"});
    EXPECT_EQ(offers(normalizeInstalled(BackendKind::Flatpak, items)), std::vector<std::string>{kGimpFlathub});
}

TEST(InventoryNormalisation, AnInstalledOfferListedTwiceIsKeptOnceWhateverTheOrder)
{
    // Multiarch: the same package name twice, one offer id.
    const std::vector<InstalledItem> a{debItem("libfoo1", "2.0-1"), debItem("gimp"), debItem("libfoo1", "1.0-1")};
    const std::vector<InstalledItem> b{debItem("libfoo1", "1.0-1"), debItem("libfoo1", "2.0-1"), debItem("gimp")};
    const auto x = normalizeInstalled(BackendKind::PackageKit, a);
    const auto y = normalizeInstalled(BackendKind::PackageKit, b);
    EXPECT_EQ(offers(x), (std::vector<std::string>{"packagekit:gimp", "packagekit:libfoo1"}));
    ASSERT_EQ(x.size(), y.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        EXPECT_EQ(x[i].offer(), y[i].offer());
        EXPECT_EQ(x[i].version(), y[i].version()) << "the kept item depends on the backend's order";
    }
}

TEST(InventoryNormalisation, DistinctInstallationsOfOneApplicationAreAllKept)
{
    // Same AppStream id, two scopes, two remotes, two branches: four offers.
    const std::vector<InstalledItem> items{
            flatpakItem(Scope::User, "flathub", "app/org.gimp.GIMP/x86_64/stable"),
            flatpakItem(Scope::System, "flathub", "app/org.gimp.GIMP/x86_64/stable"),
            flatpakItem(Scope::User, "gnome-nightly", "app/org.gimp.GIMP/x86_64/stable"),
            flatpakItem(Scope::User, "flathub", "app/org.gimp.GIMP/x86_64/beta")};
    EXPECT_EQ(normalizeInstalled(BackendKind::Flatpak, items).size(), 4u);
}

TEST(InventoryNormalisation, UpdatesAreOnePerOfferSortedAndOfTheirBackend)
{
    const std::vector<AvailableUpdate> a{debUpdate("gitg"), debUpdate("gimp", "1.0-1", "2.0-1"),
                                         debUpdate("gimp", "1.0-1", "1.5-1"), flatpakUpdate(kGimpFlathub)};
    const auto x = normalizeUpdates(BackendKind::PackageKit, a);
    ASSERT_EQ(x.size(), 2u);
    EXPECT_EQ(x[0].offer.str(), "packagekit:gimp");
    EXPECT_EQ(x[1].offer.str(), "packagekit:gitg");
    // The same list in another order keeps the same entry for gimp.
    const std::vector<AvailableUpdate> b{a[2], a[3], a[1], a[0]};
    const auto y = normalizeUpdates(BackendKind::PackageKit, b);
    ASSERT_EQ(y.size(), 2u);
    EXPECT_EQ(y[0].toVersion, x[0].toVersion);
    EXPECT_EQ(normalizeUpdates(BackendKind::Flatpak, a).size(), 1u);
}

// --- Two inventories, one state.sqlite ----------------------------------------------

class InventoryFixture : public ::testing::Test
{
protected:
    void SetUp() override { catalogfixture::build(m_dir / "catalog.sqlite"); }

    /// Registers the two fakes (either may be omitted), links with the
    /// fixture catalogue (generation 1), and wires counting callbacks.
    void start(bool packagekit = true, bool flatpak = true)
    {
        m_backends = std::make_unique<Backends>(m_dir / "catalog.sqlite", statePath(), m_io);
        if (packagekit) {
            auto b = std::make_unique<FakeBackend>(m_loop, BackendKind::PackageKit);
            m_pk = b.get();
            m_pk->setInstalled(m_pkInstalled);
            m_pk->setUpdates(m_pkUpdates);
            m_backends->add(std::move(b));
        }
        if (flatpak) {
            auto b = std::make_unique<FakeBackend>(m_loop, BackendKind::Flatpak);
            m_fp = b.get();
            m_fp->setInstalled(m_fpInstalled);
            m_fp->setUpdates(m_fpUpdates);
            m_backends->add(std::move(b));
        }
        for (const BackendKind kind : m_backends->kinds()) {
            Inventory* inventory = m_backends->inventory(kind);
            Counts& c = m_counts[kind == BackendKind::PackageKit ? 0 : 1];
            inventory->onAvailabilityChanged = [&c] { ++c.availability; };
            inventory->onInstalledChanged = [&c](const std::vector<std::string>& apps) {
                ++c.installed;
                c.lastApps = apps;
            };
            inventory->onUpdatesChanged = [&c] { ++c.updates; };
            inventory->setGeneration(1);
        }
    }

    void checkAll()
    {
        for (const BackendKind kind : m_backends->kinds())
            m_backends->inventory(kind)->check();
    }

    std::filesystem::path statePath() const { return m_dir / "state.sqlite"; }

    bool spin(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 10s)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            turn();
        }
        return true;
    }

    /// Runs the loop for one short turn.
    void turn()
    {
        const int t = m_loop.startTimer(2ms, false, [this] { m_loop.quit(0); });
        m_loop.run();
        m_loop.stopTimer(t);
    }

    /// Runs the loop for `d`: only to observe that something does NOT
    /// happen (a signal), never to wait for something.
    void idle(std::chrono::milliseconds d)
    {
        const auto end = std::chrono::steady_clock::now() + d;
        while (std::chrono::steady_clock::now() < end)
            turn();
    }

    std::vector<InstalledItem> recorded(std::optional<BackendKind> backend = std::nullopt)
    {
        auto s = SqliteInstalledState::open(statePath());
        if (!s)
            return {};
        auto items = s->items(backend);
        return items ? std::move(items).value() : std::vector<InstalledItem>{};
    }

    std::optional<UnixSeconds> reconciled(BackendKind backend, Scope scope)
    {
        auto s = StateStore::open(statePath(), 0);
        if (!s)
            return std::nullopt;
        auto r = s->lastReconciled(backend, scope);
        return r ? *r : std::nullopt;
    }

    bool bothReconciled()
    {
        return reconciled(BackendKind::PackageKit, Scope::System) && reconciled(BackendKind::Flatpak, Scope::User)
                && reconciled(BackendKind::Flatpak, Scope::System);
    }

    struct Counts
    {
        int availability = 0;
        int installed = 0;
        int updates = 0;
        std::vector<std::string> lastApps;
    };

    catalogfixture::TempDir m_dir;
    base::EventLoop m_loop;
    InventoryIo::Options m_io;
    std::vector<InstalledItem> m_pkInstalled;
    std::vector<InstalledItem> m_fpInstalled;
    std::vector<AvailableUpdate> m_pkUpdates;
    std::vector<AvailableUpdate> m_fpUpdates;
    std::unique_ptr<Backends> m_backends;
    FakeBackend* m_pk = nullptr;
    FakeBackend* m_fp = nullptr;
    Counts m_counts[2]; // PackageKit, Flatpak
};

// --- Installed -----------------------------------------------------------------------

TEST_F(InventoryFixture, RecordsPackageKitAlone)
{
    m_pkInstalled = {debItem("gimp"), debItem("libfoo1")};
    start(true, false);
    checkAll();
    ASSERT_TRUE(spin([&] { return reconciled(BackendKind::PackageKit, Scope::System).has_value(); }));
    const auto items = recorded();
    EXPECT_EQ(offers(items), (std::vector<std::string>{"packagekit:gimp", "packagekit:libfoo1"}));
    ASSERT_TRUE(items[0].app());
    EXPECT_EQ(items[0].app()->str(), "org.gimp.GIMP"); // linked with the catalogue
}

TEST_F(InventoryFixture, RecordsFlatpakAlone)
{
    m_fpInstalled = {flatpakItem(Scope::User, "flathub", "app/org.gimp.GIMP/x86_64/stable")};
    start(false, true);
    checkAll();
    ASSERT_TRUE(spin([&] { return reconciled(BackendKind::Flatpak, Scope::User).has_value(); }));
    const auto items = recorded();
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].offer().str(), kGimpFlathub);
    EXPECT_EQ(items[0].backend(), BackendKind::Flatpak);
    EXPECT_EQ(items[0].scope(), Scope::User);
    EXPECT_EQ(items[0].origin(), "flathub");
}

TEST_F(InventoryFixture, RecordsBothBackendsWithoutMergingOneApplication)
{
    // GIMP four times: deb, Flathub user, Flathub system, another remote.
    m_pkInstalled = {debItem("gimp")};
    m_fpInstalled = {flatpakItem(Scope::User, "flathub", "app/org.gimp.GIMP/x86_64/stable"),
                     flatpakItem(Scope::System, "flathub", "app/org.gimp.GIMP/x86_64/stable"),
                     flatpakItem(Scope::User, "gnome-nightly", "app/org.gimp.GIMP/x86_64/stable")};
    start();
    checkAll();
    ASSERT_TRUE(spin([&] { return bothReconciled(); }));
    const auto items = recorded();
    // Sorted by offer id: deterministic, whatever answered first.
    EXPECT_EQ(offers(items), (std::vector<std::string>{
                                     "flatpak:system:flathub:app/org.gimp.GIMP/x86_64/stable",
                                     "flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable",
                                     "flatpak:user:gnome-nightly:app/org.gimp.GIMP/x86_64/stable",
                                     "packagekit:gimp"}));
    EXPECT_EQ(recorded(BackendKind::PackageKit).size(), 1u);
    EXPECT_EQ(recorded(BackendKind::Flatpak).size(), 3u);
    // Each keeps its provenance.
    EXPECT_EQ(items[0].scope(), Scope::System);
    EXPECT_EQ(items[1].scope(), Scope::User);
    EXPECT_EQ(items[2].origin(), "gnome-nightly");
    EXPECT_EQ(items[3].backend(), BackendKind::PackageKit);
}

TEST_F(InventoryFixture, ADuplicateOrForeignItemDoesNotLoseTheInventory)
{
    // Before H.2 the whole PackageKit inventory was refused by
    // replaceInstalled (duplicate offer, item of another backend).
    m_pkInstalled = {debItem("gimp"), debItem("libfoo1", "1.0-1"), debItem("libfoo1", "2.0-1"),
                     flatpakItem(Scope::User, "flathub", "app/org.gimp.GIMP/x86_64/stable")};
    start(true, false);
    checkAll();
    ASSERT_TRUE(spin([&] { return reconciled(BackendKind::PackageKit, Scope::System).has_value(); }));
    EXPECT_EQ(offers(recorded()), (std::vector<std::string>{"packagekit:gimp", "packagekit:libfoo1"}));
}

TEST_F(InventoryFixture, AnUnavailableBackendKeepsItsLastInventoryAndDoesNotBlockTheOther)
{
    m_pkInstalled = {debItem("gimp")};
    m_fpInstalled = {flatpakItem(Scope::User, "flathub", "app/org.gimp.GIMP/x86_64/stable")};
    start();
    checkAll();
    ASSERT_TRUE(spin([&] { return bothReconciled(); }));

    // Flatpak goes away; PackageKit changes meanwhile.
    m_fp->setAvailability(Error::backend(BackendFailure::Unavailable, "no flatpak"));
    m_fp->setInstalled({});
    m_pk->setInstalled({debItem("gimp"), debItem("gitg")});
    const int installedBefore = m_counts[0].installed;
    checkAll();
    ASSERT_TRUE(spin([&] { return m_backends->inventory(BackendKind::Flatpak)->available() == false; }));
    ASSERT_TRUE(spin([&] { return m_counts[0].installed > installedBefore; }));
    // The last Flatpak inventory stays recorded (the last one seen), the
    // PackageKit one is the new one.
    EXPECT_EQ(offers(recorded()), (std::vector<std::string>{kGimpFlathub, "packagekit:gimp", "packagekit:gitg"}));
    EXPECT_FALSE(m_backends->allUnavailable());
}

// --- Updates -------------------------------------------------------------------------

TEST_F(InventoryFixture, UpdatesOfPackageKitAlone)
{
    m_pkUpdates = {debUpdate("gitg"), debUpdate("gimp"), debUpdate("libfoo1")}; // libfoo1: no application
    start(true, false);
    checkAll();
    ASSERT_TRUE(spin([&] { return m_counts[0].updates > 0; }));
    EXPECT_EQ(offers(m_backends->updates()), (std::vector<std::string>{"packagekit:gimp", "packagekit:gitg"}));
    EXPECT_EQ(m_backends->updateCount(), 2u);
}

TEST_F(InventoryFixture, UpdatesOfFlatpakAlone)
{
    m_fpUpdates = {flatpakUpdate(kGimpFlathub), flatpakUpdate(kPlatformUser, PlanItemKind::Runtime)};
    start(false, true);
    checkAll();
    ASSERT_TRUE(spin([&] { return m_counts[1].updates > 0; }));
    EXPECT_EQ(offers(m_backends->updates()), (std::vector<std::string>{kGimpFlathub, kPlatformUser}));
    EXPECT_EQ(m_backends->updateCount(), 2u);
}

TEST_F(InventoryFixture, UpdatesOfBothBackendsAreDistinctOffersSortedAndCountedOnce)
{
    // GIMP from two backends (same AppStream id, two offers), QGIS on two
    // branches (two versions), one runtime in two scopes, and duplicates.
    m_pkUpdates = {debUpdate("gimp"), debUpdate("gimp")};
    m_fpUpdates = {flatpakUpdate(kQgisStable, PlanItemKind::Application, "3.42"),
                   flatpakUpdate(kGimpFlathub),
                   flatpakUpdate(kQgisLts, PlanItemKind::Application, "3.34.2"),
                   flatpakUpdate(kPlatformSystem, PlanItemKind::Runtime),
                   flatpakUpdate(kPlatformUser, PlanItemKind::Runtime),
                   flatpakUpdate(kGimpFlathub)};
    start();
    checkAll();
    ASSERT_TRUE(spin([&] { return m_counts[0].updates > 0 && m_counts[1].updates > 0; }));
    const auto all = m_backends->updates();
    EXPECT_EQ(offers(all), (std::vector<std::string>{kPlatformSystem, kGimpFlathub, kQgisLts, kQgisStable,
                                                     kPlatformUser, "packagekit:gimp"}));
    EXPECT_EQ(m_backends->updateCount(), all.size());
    // Provenance and versions are kept.
    EXPECT_EQ(all.back().backend, BackendKind::PackageKit);
    EXPECT_EQ(all[0].backend, BackendKind::Flatpak);
    ASSERT_TRUE(all[2].update->toVersion && all[3].update->toVersion);
    EXPECT_EQ(all[2].update->toVersion->str(), "3.34.2");
    EXPECT_EQ(all[3].update->toVersion->str(), "3.42");
    ASSERT_TRUE(all[1].update->app && all[5].update->app);
    EXPECT_EQ(all[1].update->app->str(), "org.gimp.GIMP");
    EXPECT_EQ(all[5].update->app->str(), "org.gimp.GIMP");
}

TEST_F(InventoryFixture, UpdatesAreDeterministicWhateverTheBackendsOrder)
{
    m_pkUpdates = {debUpdate("gitg"), debUpdate("gimp")};
    m_fpUpdates = {flatpakUpdate(kQgisStable), flatpakUpdate(kGimpFlathub), flatpakUpdate(kQgisLts)};
    start();
    checkAll();
    ASSERT_TRUE(spin([&] { return m_counts[0].updates > 0 && m_counts[1].updates > 0; }));
    const auto first = offers(m_backends->updates());

    // The same content in another order...
    std::reverse(m_pkUpdates.begin(), m_pkUpdates.end());
    std::reverse(m_fpUpdates.begin(), m_fpUpdates.end());
    m_pk->setUpdates(m_pkUpdates);
    m_fp->setUpdates(m_fpUpdates);
    m_pk->emitChange({.updates = true});
    m_fp->emitChange({.updates = true});
    // ...then, while that run is on its way, a real change: it runs after
    // it (one run at a time), so its notification proves the first one
    // ended, without one.
    m_pk->setUpdates({debUpdate("gimp")});
    m_fp->setUpdates({flatpakUpdate(kGimpFlathub)});
    m_pk->emitChange({.updates = true});
    m_fp->emitChange({.updates = true});
    ASSERT_TRUE(spin([&] { return m_counts[0].updates == 2 && m_counts[1].updates == 2; }));
    EXPECT_EQ(m_pk->updatesQueries(), 3);
    EXPECT_EQ(m_fp->updatesQueries(), 3);
    EXPECT_EQ(first, (std::vector<std::string>{kGimpFlathub, kQgisLts, kQgisStable, "packagekit:gimp",
                                               "packagekit:gitg"}));
    EXPECT_EQ(offers(m_backends->updates()), (std::vector<std::string>{kGimpFlathub, "packagekit:gimp"}));
}

TEST_F(InventoryFixture, AnUnavailableBackendHasNoUpdatesAndTheOtherKeepsThem)
{
    m_pkUpdates = {debUpdate("gimp")};
    m_fpUpdates = {flatpakUpdate(kGimpFlathub)};
    start();
    checkAll();
    ASSERT_TRUE(spin([&] { return m_backends->updateCount() == 2; }));

    m_fp->setAvailability(Error::backend(BackendFailure::Unavailable, "no flatpak"));
    m_backends->inventory(BackendKind::Flatpak)->check();
    ASSERT_TRUE(spin([&] { return m_backends->inventory(BackendKind::Flatpak)->available() == false; }));
    EXPECT_EQ(offers(m_backends->updates()), std::vector<std::string>{"packagekit:gimp"});
    EXPECT_EQ(m_backends->updateCount(), 1u);
    EXPECT_EQ(m_counts[1].updates, 2); // listed, then cleared
    EXPECT_EQ(m_counts[1].availability, 2); // usable, then not
    EXPECT_FALSE(m_backends->allUnavailable());
}

TEST_F(InventoryFixture, ALateUpdateListDoesNotComeBackOnceTheBackendIsUnavailable)
{
    m_fpUpdates = {flatpakUpdate(kGimpFlathub)};
    start(false, true);
    m_fp->hold();
    checkAll();
    m_fp->release(); // availability: usable
    m_fp->hold();
    ASSERT_TRUE(spin([&] { return m_fp->heldAnswers() == 2; })); // installed and updates held
    // The backend goes away while its update list is still on its way.
    m_fp->setAvailability(Error::backend(BackendFailure::Unavailable, "no flatpak"));
    m_backends->inventory(BackendKind::Flatpak)->check();
    ASSERT_EQ(m_fp->heldAnswers(), 3u); // ...and the new availability
    m_fp->release();
    ASSERT_TRUE(spin([&] { return m_backends->inventory(BackendKind::Flatpak)->available() == false; }));
    idle(50ms);
    EXPECT_EQ(m_backends->updateCount(), 0u);
}

// --- Notifications --------------------------------------------------------------------

TEST_F(InventoryFixture, AChangeFromEachBackendIsNotifiedByItsInventory)
{
    start();
    checkAll();
    ASSERT_TRUE(spin([&] { return bothReconciled(); }));
    ASSERT_TRUE(m_pk->listening() && m_fp->listening());

    // `apt install gimp` outside the store.
    m_pk->setInstalled({debItem("gimp")});
    m_pk->emitChange({.installed = true});
    ASSERT_TRUE(spin([&] { return m_counts[0].installed == 1; }));
    EXPECT_EQ(m_counts[0].lastApps, std::vector<std::string>{"org.gimp.GIMP"});
    EXPECT_EQ(m_counts[1].installed, 0);

    // `flatpak install --user flathub org.qgis.qgis//lts` outside the store.
    m_fp->setInstalled({flatpakItem(Scope::User, "flathub", "app/org.qgis.qgis/x86_64/lts")});
    m_fp->emitChange({.installed = true});
    ASSERT_TRUE(spin([&] { return m_counts[1].installed == 1; }));
    EXPECT_EQ(m_counts[1].lastApps, std::vector<std::string>{"org.qgis.qgis"});
    EXPECT_EQ(m_counts[0].installed, 1);
    EXPECT_EQ(offers(recorded()), (std::vector<std::string>{kQgisLts, "packagekit:gimp"}));
}

TEST_F(InventoryFixture, NoNotificationWithoutAChangeAndRepeatedRequestsCoalesce)
{
    m_pkInstalled = {debItem("gimp")};
    m_pkUpdates = {debUpdate("gimp")};
    start(true, false);
    checkAll();
    ASSERT_TRUE(spin([&] { return m_counts[0].installed == 1 && m_counts[0].updates == 1; }));
    const int installedQueries = m_pk->installedQueries();

    // Four reports while one reconciliation runs: one more run, not four.
    // The first run sees what was installed (no change, no notification);
    // the second one the new state (one notification).
    m_pk->hold();
    m_pk->emitChange({.installed = true});
    m_pk->emitChange({.installed = true});
    m_pk->emitChange({.installed = true});
    m_pk->setInstalled({debItem("gimp"), debItem("gitg")});
    m_pk->emitChange({.installed = true});
    m_pk->release();
    ASSERT_TRUE(spin([&] { return m_counts[0].installed == 2; }));
    EXPECT_EQ(m_pk->installedQueries(), installedQueries + 2);
    EXPECT_EQ(m_counts[0].lastApps, std::vector<std::string>{"org.gnome.gitg"});
    EXPECT_EQ(m_counts[0].availability, 1);
}

TEST_F(InventoryFixture, AnAvailabilityChangeIsNotifiedOnce)
{
    start(true, false);
    checkAll();
    ASSERT_TRUE(spin([&] { return m_counts[0].availability == 1; }));
    checkAll(); // still usable: nothing to tell
    ASSERT_TRUE(spin([&] { return m_pk->installedQueries() >= 2; }));
    EXPECT_EQ(m_counts[0].availability, 1);
    m_pk->setAvailability(Error::backend(BackendFailure::Unavailable, "gone"));
    checkAll();
    ASSERT_TRUE(spin([&] { return m_counts[0].availability == 2; }));
    EXPECT_EQ(m_backends->inventory(BackendKind::PackageKit)->reason(), "gone");
    EXPECT_TRUE(m_backends->allUnavailable());
    m_pk->setAvailability(Result<void>::success());
    m_pk->emitChange({.installed = true}); // a daemon that came back
    ASSERT_TRUE(spin([&] { return m_counts[0].availability == 3; }));
    EXPECT_FALSE(m_backends->allUnavailable());
}

TEST_F(InventoryFixture, ADestroyedInventoryNeverNotifies)
{
    m_pkInstalled = {debItem("gimp")};
    m_fpInstalled = {flatpakItem(Scope::User, "flathub", "app/org.gimp.GIMP/x86_64/stable")};
    m_pkUpdates = {debUpdate("gimp")};
    start();
    bool calledAfter = false;
    // Answers on their way when the registry goes: availability first,
    // then the inventories, whose results would be written and notified.
    checkAll();
    ASSERT_TRUE(spin([&] { return m_counts[0].availability == 1 && m_counts[1].availability == 1; }));
    for (const BackendKind kind : m_backends->kinds()) {
        Inventory* inventory = m_backends->inventory(kind);
        inventory->onInstalledChanged = [&](const std::vector<std::string>&) { calledAfter = true; };
        inventory->onUpdatesChanged = [&] { calledAfter = true; };
        inventory->onAvailabilityChanged = [&] { calledAfter = true; };
    }
    m_backends.reset(); // waits for the write that started, drops the results
    m_pk = m_fp = nullptr;
    idle(100ms);
    EXPECT_FALSE(calledAfter);
    EXPECT_EQ(rawQuery(statePath(), "PRAGMA integrity_check"), "ok");
}

// --- Concurrency of the writes ---------------------------------------------------------

TEST_F(InventoryFixture, SimultaneousReconciliationsOfTwoBackendsLoseNothing)
{
    // Large inventories answered in the same loop iteration, several rounds,
    // while another connection (as the main thread's journal does) writes
    // to the same file without pause.
    auto debs = [](int round) {
        std::vector<InstalledItem> out;
        for (int i = 0; i < 1500; ++i)
            out.push_back(debItem("pkg" + std::to_string(i) + "-r" + std::to_string(round)));
        return out;
    };
    auto refs = [](int round) {
        std::vector<InstalledItem> out;
        for (int i = 0; i < 750; ++i) {
            const std::string ref = "app/org.example.App" + std::to_string(i) + "R" + std::to_string(round)
                    + "/x86_64/stable";
            out.push_back(flatpakItem(Scope::User, "flathub", ref));
            out.push_back(flatpakItem(Scope::System, "flathub", ref));
        }
        return out;
    };
    start();
    auto journal = unwrap(StateStore::open(statePath(), 1));
    std::atomic<bool> stop = false;
    std::atomic<int> journalWrites = 0;
    std::atomic<int> journalFailures = 0;
    std::thread other([&] {
        for (int i = 0; !stop; ++i) {
            TransactionRecord r{.id = "tx-" + std::to_string(i),
                                .backend = BackendKind::Flatpak,
                                .scope = Scope::User,
                                .offers = {{.offer = offer(kGimpFlathub)}},
                                .snapshot = {.kind = TransactionKind::Install, .state = TransactionState::Cancelled},
                                .created = 1,
                                .updated = 1,
                                .finished = 1};
            if (journal.saveTransaction(r))
                ++journalWrites;
            else
                ++journalFailures;
            // Load shaping, not synchronisation: the main thread writes its
            // journal a few times per transaction, not in a tight loop. A
            // writer that never pauses starves SQLite's polling busy
            // handler (see AWriteDroppedAfterTheBusyTimeoutIsTriedAgain).
            std::this_thread::sleep_for(1ms);
        }
    });

    constexpr int kRounds = 4;
    for (int round = 0; round < kRounds; ++round) {
        m_pk->setInstalled(debs(round));
        m_fp->setInstalled(refs(round));
        const int before0 = m_counts[0].installed;
        const int before1 = m_counts[1].installed;
        m_pk->hold();
        m_fp->hold();
        if (round == 0) {
            checkAll();
        } else {
            m_pk->emitChange({.installed = true});
            m_fp->emitChange({.installed = true});
        }
        ASSERT_TRUE(spin([&] {
            // availability (first round), then the inventory: release
            // both backends' answers together each time.
            if (m_pk->heldAnswers() > 0 && m_fp->heldAnswers() > 0) {
                m_pk->release();
                m_fp->release();
                m_pk->hold();
                m_fp->hold();
            }
            return m_counts[0].installed > before0 && m_counts[1].installed > before1;
        }));
        m_pk->release();
        m_fp->release();
        // Every row of this round, none of the previous one.
        ASSERT_EQ(recorded(BackendKind::PackageKit).size(), 1500u) << "round " << round;
        ASSERT_EQ(recorded(BackendKind::Flatpak).size(), 1500u) << "round " << round;
        const auto pk = recorded(BackendKind::PackageKit);
        EXPECT_TRUE(std::all_of(pk.begin(), pk.end(), [&](const InstalledItem& i) {
            return i.offer().str().ends_with("-r" + std::to_string(round));
        }));
    }
    stop = true;
    other.join();
    EXPECT_EQ(journalFailures.load(), 0);
    EXPECT_GT(journalWrites.load(), 0);
    EXPECT_EQ(rawQuery(statePath(), "PRAGMA integrity_check"), "ok");
    EXPECT_EQ(rawQuery(statePath(), "SELECT count(*) FROM installed"), "3000");
    // The journal writes are all there too (kept within the D-018 limit).
    EXPECT_EQ(rawQuery(statePath(), "SELECT count(*) FROM transactions"),
              std::to_string(std::min<int>(journalWrites.load(), static_cast<int>(kMaxFinishedTransactions))));
}

TEST_F(InventoryFixture, AReconciliationWaitsForALockHeldByAnotherConnection)
{
    m_pkInstalled = {debItem("gimp")};
    m_fpInstalled = {flatpakItem(Scope::User, "flathub", "app/org.gimp.GIMP/x86_64/stable")};
    start();
    // The file exists (as the service creates it before its inventories).
    { auto s = unwrap(StateStore::open(statePath(), 1)); }
    sqlite3* holder = nullptr;
    ASSERT_EQ(sqlite3_open(statePath().c_str(), &holder), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(holder, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);

    checkAll();
    ASSERT_TRUE(spin([&] { return m_pk->installedQueries() == 1 && m_fp->installedQueries() == 1; }));
    // While the other connection holds the write lock, no inventory can be
    // recorded: nothing may be notified, whatever the time it is held.
    idle(300ms);
    EXPECT_EQ(m_counts[0].installed, 0);
    EXPECT_EQ(m_counts[1].installed, 0);
    // The main loop was not blocked meanwhile: it kept turning (idle()).
    ASSERT_EQ(sqlite3_exec(holder, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(holder);

    // Released: both writes go through, at once or at their next attempt.
    ASSERT_TRUE(spin([&] { return m_counts[0].installed == 1 && m_counts[1].installed == 1; }));
    EXPECT_EQ(offers(recorded()), (std::vector<std::string>{kGimpFlathub, "packagekit:gimp"}));
    EXPECT_EQ(rawQuery(statePath(), "PRAGMA integrity_check"), "ok");
}

// --- H.2 finalisation: writes that never block, and never get lost ---------------------

/// Another connection holding the write lock of state.sqlite, as the main
/// thread's journal or another process would.
class Lock
{
public:
    explicit Lock(const std::filesystem::path& path)
    {
        EXPECT_EQ(sqlite3_open(path.c_str(), &m_db), SQLITE_OK);
        EXPECT_EQ(sqlite3_exec(m_db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    ~Lock() { release(); }
    void release()
    {
        if (!m_db)
            return;
        sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
        sqlite3_close(m_db);
        m_db = nullptr;
    }

private:
    sqlite3* m_db = nullptr;
};

void createState(const std::filesystem::path& path)
{
    auto s = unwrap(StateStore::open(path, 1));
}

TEST_F(InventoryFixture, AWriteNeverBlocksTheMainLoop)
{
    m_io.busyTimeout = 20ms;
    m_io.retryDelays = std::vector<std::chrono::milliseconds>(200, 20ms);
    m_pkInstalled = {debItem("gimp")};
    start(true, false);
    createState(statePath());
    Lock lock(statePath());
    int ticks = 0;
    const int timer = m_loop.startTimer(1ms, true, [&] { ++ticks; });
    checkAll();
    // Each new attempt is scheduled by a timer of this loop: several
    // attempts mean the loop kept running while the writes failed.
    ASSERT_TRUE(spin([&] { return m_backends->io().writes() >= 4; }));
    m_loop.stopTimer(timer);
    EXPECT_GT(ticks, 0);
    EXPECT_EQ(m_counts[0].installed, 0);
    EXPECT_TRUE(m_backends->inventory(BackendKind::PackageKit)->recordError().empty()); // still trying
    lock.release();
    ASSERT_TRUE(spin([&] { return m_counts[0].installed == 1; }));
    EXPECT_EQ(offers(recorded()), std::vector<std::string>{"packagekit:gimp"});
    EXPECT_EQ(m_pk->installedQueries(), 1); // tried again from the same answer
}

TEST_F(InventoryFixture, ATemporaryFailureIsRetriedAndAppliedOnce)
{
    m_io.busyTimeout = 10ms;
    m_io.retryDelays = std::vector<std::chrono::milliseconds>(500, 10ms);
    m_pkInstalled = {debItem("gimp"), debItem("gitg")};
    m_fpInstalled = {flatpakItem(Scope::User, "flathub", "app/org.gimp.GIMP/x86_64/stable")};
    start();
    createState(statePath());
    {
        Lock lock(statePath());
        checkAll();
        // Both inventories failed at least once (first attempts, then retries).
        ASSERT_TRUE(spin([&] { return m_backends->io().writes() >= 4; }));
    }
    ASSERT_TRUE(spin([&] { return m_counts[0].installed == 1 && m_counts[1].installed == 1; }));
    EXPECT_EQ(offers(recorded()),
              (std::vector<std::string>{kGimpFlathub, "packagekit:gimp", "packagekit:gitg"}));
    EXPECT_EQ(rawQuery(statePath(), "PRAGMA integrity_check"), "ok");
    // Applied once: a reconciliation of the same state changes nothing and
    // tells nothing; the rows are there once.
    m_pk->emitChange({.installed = true});
    m_pk->setInstalled({debItem("gimp"), debItem("gitg"), debItem("libfoo1")});
    m_pk->emitChange({.installed = true}); // runs after the first: proves it ended
    ASSERT_TRUE(spin([&] { return m_counts[0].installed == 2; }));
    EXPECT_EQ(m_counts[1].installed, 1);
    EXPECT_EQ(rawQuery(statePath(), "SELECT count(*) FROM installed"), "4");
}

TEST_F(InventoryFixture, AWriteThatFailsEveryAttemptIsReportedAndStaysRecoverable)
{
    m_io.busyTimeout = 10ms;
    m_io.retryDelays = {10ms, 10ms}; // three attempts
    m_pkInstalled = {debItem("gimp")};
    start(true, false);
    createState(statePath());
    Inventory* inventory = m_backends->inventory(BackendKind::PackageKit);
    {
        Lock lock(statePath());
        checkAll();
        ASSERT_TRUE(spin([&] { return !inventory->recordError().empty(); }));
        EXPECT_EQ(m_backends->io().writes(), 3);
        // Bounded: no other attempt while the file stays locked.
        idle(100ms);
        EXPECT_EQ(m_backends->io().writes(), 3);
        EXPECT_NE(inventory->recordError().find("locked"), std::string::npos) << inventory->recordError();
        EXPECT_EQ(m_counts[0].installed, 0);
        // Reported as a change of the backend's state (the Backends property).
        EXPECT_EQ(m_counts[0].availability, 2);
    }
    // The next reconciliation (a change, a Refresh) records it and clears
    // the error.
    m_pk->emitChange({.installed = true});
    ASSERT_TRUE(spin([&] { return m_counts[0].installed == 1; }));
    EXPECT_TRUE(inventory->recordError().empty());
    EXPECT_EQ(m_counts[0].availability, 3);
    EXPECT_EQ(offers(recorded()), std::vector<std::string>{"packagekit:gimp"});
}

TEST_F(InventoryFixture, APermanentErrorIsReportedAtOnceWithoutRetrying)
{
    m_io.retryDelays = std::vector<std::chrono::milliseconds>(10, 10ms);
    m_pkInstalled = {debItem("gimp")};
    {
        std::ofstream junk(statePath(), std::ios::binary);
        junk << std::string(4096, 'x'); // not a database: permanent
    }
    start(true, false);
    Inventory* inventory = m_backends->inventory(BackendKind::PackageKit);
    checkAll();
    ASSERT_TRUE(spin([&] { return !inventory->recordError().empty(); }));
    EXPECT_NE(inventory->recordError().find("not a state database"), std::string::npos) << inventory->recordError();
    idle(100ms);
    EXPECT_EQ(m_backends->io().writes(), 1);
    EXPECT_EQ(m_counts[0].installed, 0);

    // Once the file is fixed (moved aside, as the host may), it recovers.
    std::filesystem::remove(statePath());
    checkAll();
    ASSERT_TRUE(spin([&] { return m_counts[0].installed == 1; }));
    EXPECT_TRUE(inventory->recordError().empty());
    EXPECT_EQ(offers(recorded()), std::vector<std::string>{"packagekit:gimp"});
}

TEST_F(InventoryFixture, DestroyingTheRegistryDuringAWriteDoesNotWaitForIt)
{
    m_io.busyTimeout = 3s; // the write in progress waits this long
    m_pkInstalled = {debItem("gimp")};
    start(true, false);
    createState(statePath());
    Lock lock(statePath());
    Inventory* inventory = m_backends->inventory(BackendKind::PackageKit);
    inventory->check();
    ASSERT_TRUE(spin([&] { return m_backends->io().writes() == 1; })); // the write is waiting for the lock
    // From now on, nothing may be told any more.
    bool calledAfter = false;
    inventory->onInstalledChanged = [&](const std::vector<std::string>&) { calledAfter = true; };
    inventory->onAvailabilityChanged = [&] { calledAfter = true; };
    inventory->onUpdatesChanged = [&] { calledAfter = true; };
    const std::shared_future<void> stopped = m_backends->io().stopped();
    const auto before = std::chrono::steady_clock::now();
    m_backends.reset();
    m_pk = nullptr;
    const auto took = std::chrono::steady_clock::now() - before;
    // It would take the 3 s of the busy timeout if it waited for the write.
    EXPECT_LT(took, 1s);
    // The thread ends on its own (its write fails), its result dropped.
    lock.release();
    ASSERT_EQ(stopped.wait_for(10s), std::future_status::ready);
    idle(50ms);
    EXPECT_FALSE(calledAfter);
    EXPECT_EQ(rawQuery(statePath(), "PRAGMA integrity_check"), "ok");
}

TEST_F(InventoryFixture, DestroyingAnIdleRegistryEndsItsThread)
{
    start();
    checkAll();
    ASSERT_TRUE(spin([&] { return bothReconciled(); }));
    const std::shared_future<void> stopped = m_backends->io().stopped();
    m_backends.reset();
    m_pk = m_fp = nullptr;
    // Joined: already ended when the destructor returned.
    EXPECT_EQ(stopped.wait_for(0s), std::future_status::ready);
}

// --- H.2 final audit: stopping, then a new instance at once -----------------------------
//
// The scenario: instance A of the service has an inventory write in
// progress; A stops (bus name released, then destroyed) while the write is
// still going; instance B starts at once on the same state.sqlite and
// records its own, newer inventory; A's thread may only then reach SQLite.
// Every step is ordered by barriers on A's I/O thread (InventoryIoOptions::
// Hooks), never by a delay.

/// Blocks the I/O thread at a hook, the first time only, until opened.
struct Gate
{
    std::promise<void> reachedPromise;
    std::shared_future<void> reached = reachedPromise.get_future().share();
    std::promise<void> openPromise;
    std::shared_future<void> opened = openPromise.get_future().share();
    std::atomic<bool> used = false;

    std::function<void()> hook()
    {
        return [this] {
            if (used.exchange(true))
                return;
            reachedPromise.set_value();
            opened.wait();
        };
    }
    void open() { openPromise.set_value(); }
};

int threadCount()
{
    int n = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator("/proc/self/task"))
        ++n;
    return n;
}

/// One instance of the service's inventory part: a registry with a
/// PackageKit backend listing `installed`.
struct Generation
{
    Generation(base::EventLoop& loop, const std::filesystem::path& dir, std::vector<InstalledItem> installed,
               InventoryIo::Options io)
    {
        stopped = std::make_shared<std::promise<void>>();
        ended = stopped->get_future().share();
        io.hooks.threadEnded = [p = stopped] { p->set_value(); };
        backends = std::make_unique<Backends>(dir / "catalog.sqlite", dir / "state.sqlite", std::move(io));
        auto pk = std::make_unique<FakeBackend>(loop, BackendKind::PackageKit);
        pk->setInstalled(std::move(installed));
        backends->add(std::move(pk));
        Inventory* inventory = backends->inventory(BackendKind::PackageKit);
        inventory->onInstalledChanged = [this](const std::vector<std::string>&) { ++recorded; };
        inventory->onAvailabilityChanged = [this] { ++availability; };
        inventory->setGeneration(1);
    }

    /// As the service does on idle (stopInventoryWrites, then the name is
    /// released), then destroyed. Returns how long the destruction took.
    std::chrono::steady_clock::duration stopAndDestroy()
    {
        backends->stopWriting();
        const auto before = std::chrono::steady_clock::now();
        backends.reset();
        return std::chrono::steady_clock::now() - before;
    }

    std::unique_ptr<Backends> backends;
    std::shared_ptr<std::promise<void>> stopped;
    std::shared_future<void> ended;
    int recorded = 0;
    int availability = 0;
};

TEST_F(InventoryFixture, AStoppedInstanceNeverWritesOverTheNextOne)
{
    // A is stopped while its write waits to start; B records its inventory;
    // then A's thread goes on. It must write nothing.
    const int threadsBefore = threadCount();
    createState(statePath());
    Gate gate;
    InventoryIo::Options aIo;
    aIo.hooks.beforeWrite = gate.hook();
    auto a = std::make_unique<Generation>(m_loop, m_dir.path(), std::vector<InstalledItem>{debItem("gimp")}, aIo);
    a->backends->inventory(BackendKind::PackageKit)->check();
    ASSERT_TRUE(spin([&] { return gate.reached.wait_for(0s) == std::future_status::ready; }));

    // A stops and goes: at once, although its thread is held (a join would
    // never return here).
    const std::shared_future<void> aEnded = a->ended;
    int aRecordedAfter = 0;
    a->backends->inventory(BackendKind::PackageKit)->onInstalledChanged =
            [&](const std::vector<std::string>&) { ++aRecordedAfter; };
    EXPECT_LT(a->stopAndDestroy(), 1s);
    a.reset();

    // B starts at once on the same file, without waiting for A's thread.
    Generation b(m_loop, m_dir.path(), {debItem("gitg")}, {});
    b.backends->inventory(BackendKind::PackageKit)->check();
    ASSERT_TRUE(spin([&] { return b.recorded == 1; }));
    EXPECT_EQ(offers(recorded()), std::vector<std::string>{"packagekit:gitg"});

    // A's thread goes on now: it finds itself stopped and writes nothing.
    gate.open();
    ASSERT_EQ(aEnded.wait_for(10s), std::future_status::ready);
    idle(50ms); // a late result would be on the loop by now
    EXPECT_EQ(aRecordedAfter, 0);
    EXPECT_EQ(offers(recorded()), std::vector<std::string>{"packagekit:gitg"});
    EXPECT_EQ(rawQuery(statePath(), "PRAGMA integrity_check"), "ok");

    // No thread and no connection left: once B is gone too, every
    // connection is closed (the last one removes the WAL file).
    const std::shared_future<void> bEnded = b.ended;
    b.stopAndDestroy();
    ASSERT_EQ(bEnded.wait_for(10s), std::future_status::ready);
    // A detached thread signals its end just before it returns: it leaves
    // the process's task list a moment later.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (threadCount() != threadsBefore && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    EXPECT_EQ(threadCount(), threadsBefore);
    EXPECT_FALSE(std::filesystem::exists(m_dir / "state.sqlite-wal"));
}

TEST_F(InventoryFixture, AWriteOfAStoppedInstanceInsideItsTransactionIsRolledBack)
{
    // A is stopped while its write is inside its SQLite transaction (it
    // holds the write lock, at its commit hook). B's write waits for the
    // lock meanwhile. A's commit must become a rollback; B's then commits.
    createState(statePath());
    Gate gateA;
    InventoryIo::Options aIo;
    aIo.hooks.beforeCommit = gateA.hook();
    auto a = std::make_unique<Generation>(m_loop, m_dir.path(), std::vector<InstalledItem>{debItem("gimp")}, aIo);
    a->backends->inventory(BackendKind::PackageKit)->check();
    ASSERT_TRUE(spin([&] { return gateA.reached.wait_for(0s) == std::future_status::ready; }));
    const std::shared_future<void> aEnded = a->ended;
    EXPECT_LT(a->stopAndDestroy(), 1s);
    a.reset();

    // B: its first attempt meets A's lock (Conflict); its next one waits
    // at a gate of its own, so that what A leaves can be looked at alone.
    Gate gateB;
    std::atomic<int> bWrites = 0;
    InventoryIo::Options bIo;
    bIo.busyTimeout = 20ms;
    bIo.retryDelays = std::vector<std::chrono::milliseconds>(50, 20ms);
    auto gateBHook = gateB.hook();
    bIo.hooks.beforeWrite = [&bWrites, gateBHook] {
        if (++bWrites == 2)
            gateBHook();
    };
    Generation b(m_loop, m_dir.path(), {debItem("gitg")}, bIo);
    b.backends->inventory(BackendKind::PackageKit)->check();
    ASSERT_TRUE(spin([&] { return gateB.reached.wait_for(0s) == std::future_status::ready; }));
    EXPECT_EQ(b.recorded, 0); // its first attempt failed on A's lock

    // A goes on: its commit is refused, the transaction rolled back.
    gateA.open();
    ASSERT_EQ(aEnded.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(recorded().empty()) << "the stopped instance committed its inventory";
    EXPECT_EQ(rawQuery(statePath(), "PRAGMA integrity_check"), "ok");

    // B's next attempt goes through.
    gateB.open();
    ASSERT_TRUE(spin([&] { return b.recorded == 1; }));
    EXPECT_EQ(offers(recorded()), std::vector<std::string>{"packagekit:gitg"});
    EXPECT_TRUE(b.backends->inventory(BackendKind::PackageKit)->recordError().empty());
    EXPECT_EQ(rawQuery(statePath(), "PRAGMA integrity_check"), "ok");
}

TEST_F(InventoryFixture, TwoSuccessiveInstancesEachRecordTheirOwnInventory)
{
    createState(statePath());
    auto a = std::make_unique<Generation>(m_loop, m_dir.path(),
                                          std::vector<InstalledItem>{debItem("gimp"), debItem("libfoo1")},
                                          InventoryIo::Options{});
    a->backends->inventory(BackendKind::PackageKit)->check();
    ASSERT_TRUE(spin([&] { return a->recorded == 1; }));
    EXPECT_EQ(offers(recorded()), (std::vector<std::string>{"packagekit:gimp", "packagekit:libfoo1"}));
    const std::shared_future<void> aEnded = a->ended;
    a->stopAndDestroy();
    ASSERT_EQ(aEnded.wait_for(0s), std::future_status::ready); // idle: joined
    a.reset();

    Generation b(m_loop, m_dir.path(), {debItem("gitg")}, {});
    b.backends->inventory(BackendKind::PackageKit)->check();
    ASSERT_TRUE(spin([&] { return b.recorded == 1; }));
    EXPECT_EQ(offers(recorded()), std::vector<std::string>{"packagekit:gitg"});
    EXPECT_EQ(rawQuery(statePath(), "PRAGMA integrity_check"), "ok");
}

TEST_F(InventoryFixture, AStoppedWriterTakesNoMoreWork)
{
    m_pkInstalled = {debItem("gimp")};
    start(true, false);
    m_backends->stopWriting();
    EXPECT_TRUE(m_backends->io().stopping());
    checkAll();
    ASSERT_TRUE(spin([&] { return m_pk->installedQueries() == 1; }));
    idle(50ms);
    EXPECT_EQ(m_backends->io().writes(), 0); // dropped, never started
    EXPECT_EQ(m_counts[0].installed, 0);
    EXPECT_EQ(m_backends->io().stopped().wait_for(5s), std::future_status::ready);
}

} // namespace
