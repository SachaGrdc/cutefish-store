// cutefish-store-service with its PackageKit backend, against a simulated
// PackageKit on the test's private bus (fakepackagekit.h): the read-only
// integration of step F (Backends, the installed inventory in
// state.sqlite, GetUpdates, the backend's part of Refresh). The service
// runs as its own process, as in production; these tests never reach the
// system's packagekitd.

#include "busfixture.h"
#include "fakepackagekit.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace busfixture;
using namespace std::chrono_literals;
using fakepk::Answer;
using fakepk::Step;
namespace pk = cutefish::store::packagekit;

namespace {

const char* kGimpInstalled = "gimp;3.0.4-3;amd64;installed:debian-trixie-main";
const char* kLibInstalled = "libfoo1;1.0-1;amd64;installed:debian-trixie-main";
const char* kForeignInstalled = "libbar1;2.0-1;i386;installed:debian-trixie-main";
const char* kGimpApp = "org.gimp.GIMP";

pk::PkPackage pkg(pk::Info info, const std::string& id)
{
    return {static_cast<std::uint32_t>(info), id, "summary"};
}

Answer installed(const std::vector<std::string>& ids)
{
    Answer a;
    for (const auto& id : ids)
        a.packages.push_back(pkg(pk::Info::Installed, id));
    return a;
}

ServiceSetup withPackageKit()
{
    ServiceSetup s;
    s.packagekit = "session";
    return s;
}

/// The bus first (its address), then the simulated daemon on it.
struct PkBus
{
    explicit PkBus(ServiceSetup setup = withPackageKit()) : b(std::move(setup)) {}

    std::vector<dbus::VariantMap> backends() const
    {
        return b.property<std::vector<dbus::VariantMap>>("Backends").value_or(std::vector<dbus::VariantMap>{});
    }
    bool backendAvailable() const
    {
        const auto list = backends();
        return list.size() == 1 && list[0].at("available").value<bool>(false);
    }
    std::vector<dbus::VariantMap> installedItems() const
    {
        std::vector<dbus::VariantMap> items;
        b.store().call("GetInstalled", dbus::VariantMap{}).read(items);
        return items;
    }
    std::optional<dbus::VariantMap> installedOffer(const std::string& offer) const
    {
        for (const auto& i : installedItems())
            if (i.at("offer-id").value<std::string>() == offer)
                return i;
        return std::nullopt;
    }
    int calls(const std::string& method) const
    {
        const auto all = fake->calls();
        return static_cast<int>(std::count_if(all.begin(), all.end(), [&](const fakepk::Call& c) {
            return c.method == method && !c.simulate();
        }));
    }
    /// Nothing that changes the system was asked of PackageKit, not even
    /// simulated.
    bool noAction() const
    {
        for (const auto& c : fake->calls())
            if (c.method == "InstallPackages" || c.method == "RemovePackages" || c.method == "UpdatePackages")
                return false;
        return true;
    }

    Bus b;
    std::unique_ptr<fakepk::FakePackageKit> fake = std::make_unique<fakepk::FakePackageKit>();
};

std::string errorName(const dbus::Reply& r)
{
    return r.isError() ? r.error().name : std::string("<no error>");
}

std::string refresh(Bus& b)
{
    dbus::ObjectPath path;
    EXPECT_TRUE(b.store().call("Refresh", dbus::VariantMap{}).read(path));
    return path.path;
}

std::string transactionState(Bus& b, const std::string& path)
{
    return b.transaction(path).property<std::string>("State").value_or("<none>");
}

bool waitFinished(Bus& b, const std::string& path)
{
    return b.waitUntil([&] {
        const std::string s = transactionState(b, path);
        return s == "succeeded" || s == "failed" || s == "cancelled";
    });
}

} // namespace

TEST(ServicePackageKit, ABackendIsListedAndItsReadingMethodsExported)
{
    PkBus p;
    p.fake->answer("GetPackages", installed({kGimpInstalled}));
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.backendAvailable(); }));
    const auto entry = p.backends().at(0);
    EXPECT_EQ(entry.at("id").value<std::string>(), "packagekit");
    EXPECT_EQ(entry.at("scopes").value<std::vector<std::string>>(), std::vector<std::string>{"system"});
    EXPECT_EQ(entry.at("formats").value<std::vector<std::string>>(), std::vector<std::string>{"deb"});
    EXPECT_EQ(entry.at("reason").value<std::string>(), "");

    dbus::Proxy introspectable(p.b.client.get(), kName, kPath, "org.freedesktop.DBus.Introspectable");
    std::string xml;
    ASSERT_TRUE(introspectable.call("Introspect").read(xml));
    for (const char* member : {"\"GetUpdates\"", "\"UpdateCount\"", "\"UpdatesChanged\"", "\"InstalledChanged\""})
        EXPECT_NE(xml.find(member), std::string::npos) << member;
    // Read-only integration: no action yet (D-022).
    for (const char* member : {"\"Install\"", "\"Remove\"", "\"Update\"", "\"UpdateAll\"", "\"GetPermissions\"",
                               "\"EnableSource\"", "\"DisableSource\""})
        EXPECT_EQ(xml.find(member), std::string::npos) << member;
    EXPECT_EQ(p.b.property<std::string>("State"), std::optional<std::string>("ready"));
}

TEST(ServicePackageKit, TheInstalledInventoryIsRecordedAndLinked)
{
    PkBus p;
    p.fake->answer("GetPackages", installed({kGimpInstalled, kLibInstalled, kForeignInstalled}));
    std::vector<std::vector<std::string>> signals;
    auto match = p.b.client->subscribe(rule(kPath, kInterface, "InstalledChanged"), [&](const dbus::Message& m) {
        std::vector<std::string> apps;
        if (m.read(apps))
            signals.push_back(apps);
    });
    ASSERT_TRUE(p.b.waitReady());
    // Linked once the catalogue is in service (it may be read before).
    ASSERT_TRUE(p.b.waitUntil([&] {
        const auto gimp = p.installedOffer("packagekit:gimp");
        return gimp && gimp->at("app-id").value<std::string>() == kGimpApp;
    }));
    const auto items = p.installedItems();
    ASSERT_EQ(items.size(), 2u); // the foreign architecture is not listed
    const auto lib = p.installedOffer("packagekit:libfoo1");
    ASSERT_TRUE(lib);
    EXPECT_EQ(lib->at("app-id").value<std::string>(), ""); // no application in the catalogue
    EXPECT_EQ(lib->at("version").value<std::string>(), "1.0-1");
    EXPECT_EQ(lib->at("backend").value<std::string>(), "packagekit");
    EXPECT_EQ(lib->at("scope").value<std::string>(), "system");
    ASSERT_TRUE(p.b.waitUntil([&] {
        return std::any_of(signals.begin(), signals.end(), [](const auto& apps) {
            return std::find(apps.begin(), apps.end(), kGimpApp) != apps.end();
        });
    }));
    // Inventory only: GetPackages(installed;arch), nothing simulated.
    bool found = false;
    for (const auto& c : p.fake->calls())
        if (c.method == "GetPackages") {
            found = true;
            EXPECT_EQ(c.flags, pk::filter::kInstalled | pk::filter::kArch);
            EXPECT_NE(std::find(c.hints.begin(), c.hints.end(), "interactive=false"), c.hints.end());
        }
    EXPECT_TRUE(found);
    EXPECT_TRUE(p.noAction());
    EXPECT_EQ(p.calls("RefreshCache"), 0);
}

TEST(ServicePackageKit, UpdatesAreThoseOfTheCatalogueApplications)
{
    PkBus p;
    p.fake->answer("GetPackages", installed({kGimpInstalled, kLibInstalled}));
    Answer updates;
    updates.packages = {pkg(pk::Info::Security, "gimp;3.0.4-4;amd64;debian-trixie-security"),
                        pkg(pk::Info::Normal, "libfoo1;1.0-2;amd64;debian-trixie-main")};
    p.fake->answer("GetUpdates", updates);
    std::optional<std::uint32_t> signalled;
    auto match = p.b.client->subscribe(rule(kPath, kInterface, "UpdatesChanged"), [&](const dbus::Message& m) {
        std::uint32_t n = 0;
        if (m.read(n))
            signalled = n;
    });
    ASSERT_TRUE(p.b.waitReady());
    // D-010: the library is left out, the application is kept.
    ASSERT_TRUE(p.b.waitUntil([&] { return p.b.property<std::uint32_t>("UpdateCount") == std::optional<std::uint32_t>(1); }));
    std::vector<dbus::VariantMap> list;
    ASSERT_TRUE(p.b.store().call("GetUpdates").read(list));
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].at("offer-id").value<std::string>(), "packagekit:gimp");
    EXPECT_EQ(list[0].at("app-id").value<std::string>(), kGimpApp);
    EXPECT_EQ(list[0].at("kind").value<std::string>(), "application");
    EXPECT_EQ(list[0].at("from-version").value<std::string>(), "3.0.4-3");
    EXPECT_EQ(list[0].at("to-version").value<std::string>(), "3.0.4-4");
    EXPECT_EQ(list[0].at("backend").value<std::string>(), "packagekit");
    ASSERT_TRUE(p.b.waitUntil([&] { return signalled == std::optional<std::uint32_t>(1); }));
    EXPECT_TRUE(p.noAction());
}

TEST(ServicePackageKit, ChangesMadeOutsideTheStoreAreReconciled)
{
    PkBus p;
    p.fake->answer("GetPackages", installed({kGimpInstalled, kLibInstalled}));
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] {
        const auto gimp = p.installedOffer("packagekit:gimp");
        return gimp && gimp->at("app-id").value<std::string>() == kGimpApp;
    }));
    std::vector<std::vector<std::string>> signals;
    auto match = p.b.client->subscribe(rule(kPath, kInterface, "InstalledChanged"), [&](const dbus::Message& m) {
        std::vector<std::string> apps;
        if (m.read(apps))
            signals.push_back(apps);
    });
    // `apt remove gimp` in a terminal: packagekitd says InstalledChanged.
    p.fake->replace("GetPackages", installed({kLibInstalled}));
    p.fake->emitDaemon("InstalledChanged");
    ASSERT_TRUE(p.b.waitUntil([&] { return !p.installedOffer("packagekit:gimp"); }));
    ASSERT_TRUE(p.b.waitUntil([&] { return !signals.empty(); }));
    EXPECT_EQ(signals.back(), std::vector<std::string>{kGimpApp});
    EXPECT_TRUE(p.installedOffer("packagekit:libfoo1"));
}

TEST(ServicePackageKit, AnUnusableBackendDegradesButTheCatalogueReads)
{
    PkBus p;
    p.fake->distroId = "fedora;41;x86_64";
    ASSERT_TRUE(p.b.waitUntil([&] { return p.b.property<std::string>("State") == std::optional<std::string>("degraded"); }));
    ASSERT_TRUE(p.b.waitUntil([&] { return !p.backends().empty() && !p.backends()[0].at("reason").value<std::string>().empty(); }));
    const auto entry = p.backends().at(0);
    EXPECT_FALSE(entry.at("available").value<bool>(true));
    EXPECT_NE(entry.at("reason").value<std::string>().find("not supported"), std::string::npos);
    EXPECT_GT(p.b.property<std::uint64_t>("CatalogGeneration").value_or(0), 0u);
    const auto page = busfixture::page(p.b.store().call("Search", std::string("gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(page);
    EXPECT_GT(page->total, 0u);
    std::vector<dbus::VariantMap> updates;
    ASSERT_TRUE(p.b.store().call("GetUpdates").read(updates));
    EXPECT_TRUE(updates.empty());
    EXPECT_EQ(p.calls("GetPackages"), 0); // asked nothing else of it
}

TEST(ServicePackageKit, ADaemonThatComesBackIsUsedAfterARefresh)
{
    PkBus p;
    p.fake->answer("GetPackages", installed({kGimpInstalled}));
    p.fake->stop(); // org.freedesktop.PackageKit has no owner
    ASSERT_TRUE(p.b.waitUntil([&] { return p.b.property<std::string>("State") == std::optional<std::string>("degraded"); }));
    EXPECT_FALSE(p.backendAvailable());
    p.fake->start();
    const std::string t = refresh(p.b);
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(transactionState(p.b, t), "succeeded");
    ASSERT_TRUE(p.b.waitUntil([&] { return p.backendAvailable(); }));
    ASSERT_TRUE(p.b.waitUntil([&] { return p.b.property<std::string>("State") == std::optional<std::string>("ready"); }));
    ASSERT_TRUE(p.b.waitUntil([&] { return p.installedOffer("packagekit:gimp").has_value(); }));
}

TEST(ServicePackageKit, RefreshAsksPackageKitToRefreshFirst)
{
    PkBus p;
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.backendAvailable(); }));
    EXPECT_EQ(p.calls("RefreshCache"), 0); // never at startup
    const std::string t = refresh(p.b);
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(transactionState(p.b, t), "succeeded");
    ASSERT_EQ(p.calls("RefreshCache"), 1);
    for (const auto& c : p.fake->calls())
        if (c.method == "RefreshCache") {
            EXPECT_EQ(c.flags, 0u); // force = false
            EXPECT_NE(std::find(c.hints.begin(), c.hints.end(), "interactive=true"), c.hints.end());
            // Never `background`: such a transaction is cancelled by others (§6.6).
            for (const auto& h : c.hints)
                EXPECT_EQ(h.find("background"), std::string::npos) << h;
        }
    EXPECT_TRUE(p.noAction());
}

TEST(ServicePackageKit, AFailedPackageKitRefreshFailsTheRefresh)
{
    PkBus p;
    Answer failed;
    failed.exit = pk::Exit::Failed;
    failed.error = fakepk::errorCode("no-network");
    failed.errorDetails = "offline";
    p.fake->answer("RefreshCache", failed);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.backendAvailable(); }));
    const auto generation = p.b.property<std::uint64_t>("CatalogGeneration");
    const auto last = p.b.property<std::uint64_t>("LastRefresh");
    const std::string t = refresh(p.b);
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(transactionState(p.b, t), "failed");
    const auto error = p.b.transaction(t).property<dbus::VariantMap>("Error");
    ASSERT_TRUE(error);
    EXPECT_NE(error->at("details").value<std::string>().find("no-network"), std::string::npos);
    // The catalogue in service stays, and the refresh is not recorded.
    EXPECT_EQ(p.b.property<std::uint64_t>("CatalogGeneration"), generation);
    EXPECT_EQ(p.b.property<std::uint64_t>("LastRefresh"), last);
    EXPECT_EQ(p.b.property<std::string>("State"), std::optional<std::string>("ready"));
}

TEST(ServicePackageKit, ARefreshIsCancelledWhilePackageKitAllowsIt)
{
    PkBus p;
    Answer slow;
    for (int i = 0; i < 300; ++i) // about 4.5 s, cancellable all along
        slow.steps.push_back(Step{.status = pk::Status::DownloadRepository, .percentage = 0, .allowCancel = true});
    p.fake->answer("RefreshCache", slow);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.backendAvailable(); }));
    const auto generation = p.b.property<std::uint64_t>("CatalogGeneration");
    const std::string t = refresh(p.b);
    ASSERT_TRUE(p.b.waitUntil([&] { return p.b.transaction(t).property<bool>("Cancellable") == std::optional<bool>(true); }));
    const dbus::Reply r = p.b.transaction(t).call("Cancel");
    EXPECT_FALSE(r.isError()) << errorName(r);
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(transactionState(p.b, t), "cancelled");
    EXPECT_EQ(p.fake->cancels(), 1);
    EXPECT_EQ(p.b.property<std::uint64_t>("CatalogGeneration"), generation); // not reindexed
}

TEST(ServicePackageKit, CatalogueSourcesComeFromTheBackend)
{
    ServiceSetup setup = withPackageKit();
    setup.debianInFile = false; // only Flathub in sources.conf
    PkBus p(setup);
    // The distribution's DEP-11 file, where the Debian profile finds it.
    std::filesystem::copy_file(
            p.b.dir / "debian.yml",
            p.b.dir / "swcatalog" / "yaml" / "deb.debian.org_debian_dists_trixie_main_dep11_Components-amd64.yml");
    ASSERT_TRUE(p.b.waitReady());
    std::vector<dbus::VariantMap> sources;
    ASSERT_TRUE(p.b.store().call("ListSources").read(sources));
    std::vector<std::string> ids;
    for (const auto& s : sources)
        ids.push_back(s.at("id").value<std::string>());
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<std::string>{"flatpak:user:flathub", "packagekit:debian-trixie-main"}));
    for (const auto& s : sources)
        if (s.at("id").value<std::string>() == "packagekit:debian-trixie-main") {
            EXPECT_EQ(s.at("kind").value<std::string>(), "distribution");
            EXPECT_GT(s.at("offers").value<std::uint32_t>(), 0u);
        }
    const auto page = busfixture::page(p.b.store().call("Search", std::string("gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(page);
    EXPECT_GT(page->total, 0u);
}
