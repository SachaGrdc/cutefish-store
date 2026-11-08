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
#include <fstream>

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

TEST(ServicePackageKit, ABackendIsListedAndItsMethodsExported)
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
    for (const char* member : {"\"GetUpdates\"", "\"UpdateCount\"", "\"UpdatesChanged\"", "\"InstalledChanged\"",
                               "\"Install\"", "\"Remove\"", "\"Update\"", "\"UpdateAll\""})
        EXPECT_NE(xml.find(member), std::string::npos) << member;
    // Still nothing that cannot work (D-022): no source toggling, no permissions.
    for (const char* member : {"\"GetPermissions\"", "\"EnableSource\"", "\"DisableSource\""})
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

TEST(ServicePackageKit, AFailedRepositoryRefreshStillReindexesTheLocalMetadata)
{
    // D-028: RefreshCache fails (offline), the metadata on disk is still
    // reindexed, and the refresh as a whole reports the failure.
    ServiceSetup setup = withPackageKit();
    setup.debianInFile = false; // Flathub only, until a DEP-11 file appears
    PkBus p(setup);
    Answer failed;
    failed.exit = pk::Exit::Failed;
    failed.error = fakepk::errorCode("no-network");
    failed.errorDetails = "offline";
    p.fake->answer("RefreshCache", failed);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.backendAvailable(); }));
    const auto generation = p.b.property<std::uint64_t>("CatalogGeneration").value_or(0);
    const auto last = p.b.property<std::uint64_t>("LastRefresh");
    // New metadata on disk (an `apt update` made earlier, say).
    std::filesystem::copy_file(
            p.b.dir / "debian.yml",
            p.b.dir / "swcatalog" / "yaml" / "deb.debian.org_debian_dists_trixie_main_dep11_Components-amd64.yml");
    const std::string t = refresh(p.b);
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(p.calls("RefreshCache"), 1);
    EXPECT_EQ(transactionState(p.b, t), "failed");
    const auto error = p.b.transaction(t).property<dbus::VariantMap>("Error");
    ASSERT_TRUE(error);
    const std::string details = error->at("details").value<std::string>();
    EXPECT_NE(details.find("no-network"), std::string::npos) << details;
    EXPECT_NE(details.find("local metadata was reindexed"), std::string::npos) << details;
    // The local catalogue was refreshed all the same...
    EXPECT_GT(p.b.property<std::uint64_t>("CatalogGeneration").value_or(0), generation);
    std::vector<dbus::VariantMap> sources;
    ASSERT_TRUE(p.b.store().call("ListSources").read(sources));
    EXPECT_TRUE(std::any_of(sources.begin(), sources.end(), [](const dbus::VariantMap& s) {
        return s.at("id").value<std::string>() == "packagekit:debian-trixie-main";
    }));
    // ...but the refresh asked for is not recorded as done.
    EXPECT_EQ(p.b.property<std::uint64_t>("LastRefresh"), last);
    EXPECT_EQ(p.b.property<std::string>("State"), std::optional<std::string>("ready"));
}

TEST(ServicePackageKit, ALocalRefreshNeverAsksPackageKit)
{
    PkBus p;
    Answer failed; // would fail if it were called
    failed.exit = pk::Exit::Failed;
    failed.error = fakepk::errorCode("no-network");
    p.fake->answer("RefreshCache", failed);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.backendAvailable(); }));
    dbus::ObjectPath path;
    ASSERT_TRUE(p.b.store().call("Refresh", dbus::VariantMap{{"repositories", false}}).read(path));
    ASSERT_TRUE(waitFinished(p.b, path.path));
    EXPECT_EQ(transactionState(p.b, path.path), "succeeded");
    EXPECT_EQ(p.calls("RefreshCache"), 0);
    EXPECT_TRUE(p.noAction());
    const dbus::Reply wrong = p.b.store().call("Refresh", dbus::VariantMap{{"repositories", std::string("no")}});
    EXPECT_EQ(errorName(wrong), "org.freedesktop.DBus.Error.InvalidArgs");
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

TEST(ServicePackageKit, AMetadataFileListedTwiceIsIndexedOnce)
{
    // A sources.conf written before the backend listed the distribution's
    // DEP-11 file (D-020) names the same file under an id of its own.
    PkBus p;
    const auto dep11 =
            p.b.dir / "swcatalog" / "yaml" / "deb.debian.org_debian_dists_trixie_main_dep11_Components-amd64.yml";
    std::filesystem::copy_file(p.b.dir / "debian.yml", dep11);
    {
        std::ofstream out(p.b.dir / "sources.conf", std::ios::trunc);
        out << "id=packagekit:my-debian,backend=packagekit,scope=system,kind=distribution,origin=my-debian,"
               "priority=10,file="
            << dep11.string() << "\n";
    }
    ASSERT_TRUE(p.b.waitReady());
    std::vector<dbus::VariantMap> sources;
    ASSERT_TRUE(p.b.store().call("ListSources").read(sources));
    std::vector<std::string> ids;
    for (const auto& s : sources)
        ids.push_back(s.at("id").value<std::string>());
    // The file's entry wins; the backend's reference to the same file is
    // ignored (with a warning in the service's log).
    EXPECT_EQ(ids, std::vector<std::string>{"packagekit:my-debian"});
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_GT(sources[0].at("offers").value<std::uint32_t>(), 0u);
}

// --- Transactions (step F2) ---------------------------------------------------------------

namespace {

const char* kGimpAvailableId = "gimp;3.0.4-3+deb13u11;amd64;debian-stable-security-main";

Answer resolved(pk::Info info, const std::string& id)
{
    Answer a;
    a.packages = {pkg(info, id)};
    return a;
}

std::string action(Bus&, const char* method, const dbus::Reply& r)
{
    dbus::ObjectPath path;
    EXPECT_TRUE(r.read(path)) << method << ": " << errorName(r);
    return path.path;
}

bool waitState(Bus& b, const std::string& path, const std::string& state)
{
    return b.waitUntil([&] { return transactionState(b, path) == state; });
}

} // namespace

TEST(ServiceTransactions, AnInstallWithoutConfirmationRuns)
{
    PkBus p;
    p.fake->answer("Resolve", resolved(pk::Info::Available, kGimpAvailableId));
    Answer sim;
    sim.packages = {pkg(pk::Info::Installing, "gimp;3.0.4-3+deb13u11;amd64;+manual:debian-stable-security-main"),
                    pkg(pk::Info::Installing, "libgimp-3.0-0;3.0.4-3;amd64;+auto:debian-stable-main")};
    sim.downloadSize = 52458144;
    // The fake sends DownloadSizeRemaining with a progress step (halved).
    sim.steps = {Step{.status = pk::Status::Query, .percentage = 50, .allowCancel = true}};
    p.fake->answer("InstallPackages", sim, true);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.backendAvailable(); }));
    const std::string t = action(p.b, "Install", p.b.store().call("Install", std::string("packagekit:gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(transactionState(p.b, t), "succeeded");
    EXPECT_EQ(p.b.transaction(t).property<std::string>("Kind"), std::optional<std::string>("install"));
    const auto plan = p.b.transaction(t).property<dbus::VariantMap>("Plan");
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->at("download-size").value<std::uint64_t>(), 52458144u / 2);
    EXPECT_EQ(plan->at("operations").value<std::vector<dbus::VariantMap>>().size(), 2u);
    // Two simulations (plan, then the check before executing), one real
    // transaction, interactive, ONLY_TRUSTED (D-017, SERVICE §6.3).
    int simulations = 0;
    int real = 0;
    for (const auto& c : p.fake->calls())
        if (c.method == "InstallPackages") {
            (c.simulate() ? simulations : real)++;
            if (!c.simulate()) {
                EXPECT_EQ(c.flags, pk::flag::kOnlyTrusted);
                EXPECT_NE(std::find(c.hints.begin(), c.hints.end(), "interactive=true"), c.hints.end());
            }
        }
    EXPECT_EQ(simulations, 2);
    EXPECT_EQ(real, 1);
    EXPECT_EQ(p.fake->maxConcurrent(), 1);
    std::vector<dbus::VariantMap> history;
    ASSERT_TRUE(p.b.store().call("GetTransactionHistory", std::uint32_t(10)).read(history));
    ASSERT_EQ(history.size(), 1u);
}

TEST(ServiceTransactions, ACascadedRemovalWaitsForConfirmation)
{
    PkBus p;
    p.fake->answer("GetPackages", installed({kGimpInstalled, kLibInstalled}));
    p.fake->answer("Resolve", resolved(pk::Info::Installed, kGimpInstalled));
    Answer sim;
    sim.packages = {pkg(pk::Info::Removing, kGimpInstalled), pkg(pk::Info::Removing, kLibInstalled)};
    p.fake->answer("RemovePackages", sim, true);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.installedOffer("packagekit:gimp").has_value(); }));
    const std::string t = action(p.b, "Remove", p.b.store().call("Remove", std::string("packagekit:gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(waitState(p.b, t, "awaiting-confirmation"));
    const auto plan = p.b.transaction(t).property<dbus::VariantMap>("Plan");
    ASSERT_TRUE(plan);
    EXPECT_TRUE(plan->at("requires-confirmation").value<bool>(false));
    const auto reasons = plan->at("reasons").value<std::vector<std::string>>();
    EXPECT_NE(std::find(reasons.begin(), reasons.end(), "removes-packages"), reasons.end());
    EXPECT_GT(p.b.transaction(t).property<std::uint64_t>("ConfirmationDeadline").value_or(0), 0u);
    EXPECT_EQ(p.calls("RemovePackages"), 0); // nothing removed yet
    // The same offer cannot be engaged twice (Busy).
    const dbus::Reply again = p.b.store().call("Remove", std::string("packagekit:gimp"), dbus::VariantMap{});
    EXPECT_EQ(errorName(again), "org.cutefish.Store1.Error.Busy");
    const dbus::Reply ok = p.b.transaction(t).call("Confirm");
    EXPECT_FALSE(ok.isError()) << errorName(ok);
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(transactionState(p.b, t), "succeeded");
    EXPECT_EQ(p.calls("RemovePackages"), 1);
    // Confirm is refused outside awaiting-confirmation.
    EXPECT_EQ(errorName(p.b.transaction(t).call("Confirm")), "org.cutefish.Store1.Error.NotAllowed");
}

TEST(ServiceTransactions, AProtectedPackageIsRefusedWhilePlanning)
{
    PkBus p;
    p.fake->answer("GetPackages", installed({kGimpInstalled}));
    p.fake->answer("Resolve", resolved(pk::Info::Installed, kGimpInstalled));
    Answer sim;
    sim.packages = {pkg(pk::Info::Removing, kGimpInstalled),
                    pkg(pk::Info::Removing, "cutefish-desktop;1.0;all;manual:cutefish-trixie-nightly-main")};
    p.fake->answer("RemovePackages", sim, true);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.installedOffer("packagekit:gimp").has_value(); }));
    const std::string t = action(p.b, "Remove", p.b.store().call("Remove", std::string("packagekit:gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(transactionState(p.b, t), "failed");
    const auto error = p.b.transaction(t).property<dbus::VariantMap>("Error");
    ASSERT_TRUE(error);
    EXPECT_EQ(error->at("code").value<std::string>(), "not-allowed"); // D-032
    EXPECT_EQ(p.calls("RemovePackages"), 0);
}

TEST(ServiceTransactions, AChangedPlanIsNeverExecutedWithoutConfirmation)
{
    PkBus p;
    p.fake->answer("Resolve", resolved(pk::Info::Available, kGimpAvailableId));
    Answer first;
    first.packages = {pkg(pk::Info::Installing, "gimp;3.0.4-3+deb13u11;amd64;+manual:debian-stable-security-main")};
    Answer second = first; // the check before executing: now removes a package
    second.packages.push_back(pkg(pk::Info::Removing, kLibInstalled));
    p.fake->answer("InstallPackages", first, true);
    p.fake->answer("InstallPackages", second, true);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.backendAvailable(); }));
    const std::string t = action(p.b, "Install", p.b.store().call("Install", std::string("packagekit:gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(waitState(p.b, t, "awaiting-confirmation"));
    const auto reasons = p.b.transaction(t).property<dbus::VariantMap>("Plan")->at("reasons").value<std::vector<std::string>>();
    EXPECT_NE(std::find(reasons.begin(), reasons.end(), "plan-changed"), reasons.end());
    EXPECT_EQ(p.calls("InstallPackages"), 0); // no real transaction
    EXPECT_FALSE(p.b.transaction(t).call("Cancel").isError());
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(transactionState(p.b, t), "cancelled");
}

TEST(ServiceTransactions, AnUnansweredConfirmationExpires)
{
    ServiceSetup setup = withPackageKit();
    setup.confirmationTimeout = 500ms; // D-019 is 10 minutes
    PkBus p(setup);
    p.fake->answer("GetPackages", installed({kGimpInstalled, kLibInstalled}));
    p.fake->answer("Resolve", resolved(pk::Info::Installed, kGimpInstalled));
    Answer sim;
    sim.packages = {pkg(pk::Info::Removing, kGimpInstalled), pkg(pk::Info::Removing, kLibInstalled)};
    p.fake->answer("RemovePackages", sim, true);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.installedOffer("packagekit:gimp").has_value(); }));
    const std::string t = action(p.b, "Remove", p.b.store().call("Remove", std::string("packagekit:gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(waitState(p.b, t, "awaiting-confirmation"));
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(transactionState(p.b, t), "cancelled");
    EXPECT_EQ(p.calls("RemovePackages"), 0);
}

TEST(ServiceTransactions, AWaitingConfirmationDoesNotBlockTheBackend)
{
    // D-030: the lane is released while the user decides.
    PkBus p;
    p.fake->answer("GetPackages", installed({kGimpInstalled, kLibInstalled}));
    p.fake->answer("Resolve", resolved(pk::Info::Installed, kGimpInstalled));
    Answer cascade;
    cascade.packages = {pkg(pk::Info::Removing, kGimpInstalled), pkg(pk::Info::Removing, kLibInstalled)};
    p.fake->answer("RemovePackages", cascade, true);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.installedOffer("packagekit:gimp").has_value(); }));
    const std::string removal = action(p.b, "Remove", p.b.store().call("Remove", std::string("packagekit:gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(waitState(p.b, removal, "awaiting-confirmation"));
    // A Refresh takes the lane meanwhile and completes.
    const std::string r = refresh(p.b);
    ASSERT_TRUE(waitFinished(p.b, r));
    EXPECT_EQ(transactionState(p.b, r), "succeeded");
    EXPECT_EQ(transactionState(p.b, removal), "awaiting-confirmation");
}

TEST(ServiceTransactions, AuthorizationRefusedFailsTheTransaction)
{
    PkBus p;
    p.fake->answer("Resolve", resolved(pk::Info::Available, kGimpAvailableId));
    Answer sim;
    sim.packages = {pkg(pk::Info::Installing, "gimp;3.0.4-3+deb13u11;amd64;+manual:debian-stable-security-main")};
    p.fake->answer("InstallPackages", sim, true);
    Answer denied;
    denied.exit = pk::Exit::Failed;
    denied.error = fakepk::errorCode("not-authorized");
    p.fake->answer("InstallPackages", denied, false);
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.backendAvailable(); }));
    const std::string t = action(p.b, "Install", p.b.store().call("Install", std::string("packagekit:gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(waitFinished(p.b, t));
    EXPECT_EQ(transactionState(p.b, t), "failed");
    EXPECT_EQ(p.b.transaction(t).property<dbus::VariantMap>("Error")->at("code").value<std::string>(), "auth-denied");
}

TEST(ServiceTransactions, ActionsCheckTheirInputs)
{
    PkBus p;
    p.fake->answer("GetPackages", installed({kGimpInstalled}));
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.installedOffer("packagekit:gimp").has_value(); }));
    auto install = [&](const std::string& offer, dbus::VariantMap options = {}) {
        return errorName(p.b.store().call("Install", offer, options));
    };
    EXPECT_EQ(install("packagekit:no-such-package"), "org.cutefish.Store1.Error.NotFound");
    EXPECT_EQ(install("packagekit:gimp"), "org.cutefish.Store1.Error.NotAllowed"); // already installed
    EXPECT_EQ(install("packagekit:gimp", {{"force", true}}), "org.freedesktop.DBus.Error.InvalidArgs");
    EXPECT_EQ(install("not an offer"), "org.freedesktop.DBus.Error.InvalidArgs");
    EXPECT_EQ(install("flatpak:user:flathub:app/org.example.App/x86_64/stable"),
              "org.cutefish.Store1.Error.BackendUnavailable");
    EXPECT_EQ(errorName(p.b.store().call("Remove", std::string("packagekit:example-tool"), dbus::VariantMap{})),
              "org.cutefish.Store1.Error.NotFound"); // not installed
    EXPECT_EQ(errorName(p.b.store().call("Update", std::vector<std::string>{"packagekit:gimp"}, dbus::VariantMap{})),
              "org.cutefish.Store1.Error.NotFound"); // no update
    EXPECT_EQ(errorName(p.b.store().call("UpdateAll", dbus::VariantMap{})), "org.cutefish.Store1.Error.NotFound");
    EXPECT_TRUE(p.noAction());
}

TEST(ServiceTransactions, UpdateAllIsAParentOfOneChildPerBackend)
{
    PkBus p;
    p.fake->answer("GetPackages", installed({kGimpInstalled}));
    Answer updates;
    updates.packages = {pkg(pk::Info::Security, "gimp;3.0.4-4;amd64;debian-trixie-security")};
    p.fake->answer("GetUpdates", updates);
    Answer sim;
    sim.packages = {pkg(pk::Info::Updating, "gimp;3.0.4-4;amd64;debian-trixie-security")};
    p.fake->answer("UpdatePackages", sim, true);
    std::vector<std::string> added;
    auto match = p.b.client->subscribe(rule(kPath, kInterface, "TransactionAdded"), [&](const dbus::Message& m) {
        dbus::ObjectPath path;
        if (m.read(path))
            added.push_back(path.path);
    });
    ASSERT_TRUE(p.b.waitReady());
    ASSERT_TRUE(p.b.waitUntil([&] { return p.b.property<std::uint32_t>("UpdateCount") == std::optional<std::uint32_t>(1); }));
    const std::string parent = action(p.b, "UpdateAll", p.b.store().call("UpdateAll", dbus::VariantMap{}));
    ASSERT_TRUE(p.b.waitUntil([&] { return added.size() == 2; }));
    EXPECT_EQ(p.b.transaction(parent).property<std::string>("Kind"), std::optional<std::string>("update-all"));
    EXPECT_EQ(p.b.transaction(parent).property<std::string>("Backend"), std::optional<std::string>("multiple"));
    const std::string child = added[0] == parent ? added[1] : added[0];
    EXPECT_EQ(p.b.transaction(child).property<dbus::ObjectPath>("Parent")->path, parent);
    ASSERT_TRUE(waitFinished(p.b, child));
    EXPECT_EQ(transactionState(p.b, child), "succeeded");
    ASSERT_TRUE(waitFinished(p.b, parent));
    EXPECT_EQ(transactionState(p.b, parent), "succeeded");
    EXPECT_EQ(p.calls("UpdatePackages"), 1);
}
