// The PackageKit logic on the data PackageKit 1.3.1 really sent
// (prototype 4, results/raw): package ids, plans, errors, inventory,
// updates, and the Debian profile.

#include "catalogfixture.h"
#include "debianprofile.h"
#include "packageid.h"
#include "pkenums.h"
#include "translate.h"

#include <cutefish/store/confirmation.h>

#include <gtest/gtest.h>

#include <fstream>

using namespace cutefish::store;
using namespace cutefish::store::packagekit;
using catalogfixture::TempDir;
using fixtures::unwrap;

namespace {

Profile debianProfile()
{
    return {.nativeArch = "amd64", .isProtected = [](std::string_view n) { return debian::isProtected(n); }};
}

PkResult success(std::vector<PkPackage> packages, std::uint64_t download = 0)
{
    PkResult r;
    r.exit = static_cast<std::uint32_t>(Exit::Success);
    r.packages = std::move(packages);
    r.downloadSizeRemaining = download;
    return r;
}

PkResult failure(const char* code)
{
    PkResult r;
    r.exit = static_cast<std::uint32_t>(Exit::Failed);
    for (std::uint32_t c = 0; c < 80; ++c)
        if (errorName(c) == code)
            r.errorCode = c;
    r.errorDetails = "details from APT";
    return r;
}

PlanRequest request(TransactionKind kind, std::vector<std::string> names)
{
    std::vector<OfferId> offers;
    for (const auto& n : names)
        offers.push_back(unwrap(OfferId::parse("packagekit:" + n)));
    return unwrap(PlanRequest::create(kind, std::move(offers)));
}

const PlanItem* item(const Plan& plan, std::string_view name) { return plan.find(name); }

} // namespace

// --- Package ids ------------------------------------------------------------------

TEST(PackageId, ParsesTheFourFields)
{
    auto id = unwrap(parsePackageId("zlib1g;1:1.3.dfsg+really1.3.1-1+deb13u1;i386;debian-stable-security-main"));
    EXPECT_EQ(id.name, "zlib1g");
    EXPECT_EQ(id.version, "1:1.3.dfsg+really1.3.1-1+deb13u1");
    EXPECT_EQ(id.arch, "i386");
    EXPECT_EQ(id.origin(), "debian-stable-security-main");
    EXPECT_EQ(id.qualifiedName("amd64"), "zlib1g:i386");
    EXPECT_EQ(id.str(), "zlib1g;1:1.3.dfsg+really1.3.1-1+deb13u1;i386;debian-stable-security-main");

    EXPECT_EQ(unwrap(parsePackageId("bash;5.2.37-2+b10;amd64;manual:debian-stable-main")).origin(), "debian-stable-main");
    EXPECT_EQ(unwrap(parsePackageId("libltdl7;2.5.4-4;amd64;+auto:debian-stable-main")).origin(), "debian-stable-main");
    EXPECT_EQ(unwrap(parsePackageId("cutefish-dm;1.0-1;amd64;auto:local")).origin(), "local");
    EXPECT_EQ(unwrap(parsePackageId("bash;5.2;amd64;installed")).origin(), "");
    EXPECT_EQ(unwrap(parsePackageId("fonts-urw-base35;20200910-8;all;+auto:x")).qualifiedName("amd64"), "fonts-urw-base35");

    EXPECT_FALSE(parsePackageId("bash;5.2;amd64"));
    EXPECT_FALSE(parsePackageId(";5.2;amd64;x"));
    EXPECT_FALSE(parsePackageId("a;b;c;d;e"));
}

// --- Results and errors -------------------------------------------------------------

TEST(PackageKitResult, ErrorsMapToTheCoreFailures)
{
    EXPECT_TRUE(checkResult(success({})));
    auto expect = [](const char* code, BackendFailure f) {
        auto r = checkResult(failure(code));
        ASSERT_FALSE(r) << code;
        EXPECT_EQ(r.error().code(), ErrorCode::Backend) << code;
        EXPECT_EQ(r.error().backendFailure(), f) << code;
        EXPECT_NE(r.error().message().find(code), std::string::npos);
        EXPECT_NE(r.error().message().find("details from APT"), std::string::npos);
    };
    expect("not-authorized", BackendFailure::AuthDenied);
    expect("cannot-get-lock", BackendFailure::Locked);
    expect("no-network", BackendFailure::Network);
    expect("bad-gpg-signature", BackendFailure::Untrusted);
    expect("dep-resolution-failed", BackendFailure::Dependency);
    expect("file-conflicts", BackendFailure::PackageConflict);
    expect("no-space-on-device", BackendFailure::NoSpace);
    expect("internal-error", BackendFailure::Unspecified);
    EXPECT_EQ(checkResult(failure("package-not-found")).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(checkResult(failure("transaction-cancelled")).error().code(), ErrorCode::Cancelled);

    PkResult cancelled;
    cancelled.exit = static_cast<std::uint32_t>(Exit::CancelledPriority);
    EXPECT_EQ(checkResult(cancelled).error().code(), ErrorCode::Cancelled);
    PkResult killed;
    killed.exit = static_cast<std::uint32_t>(Exit::Killed);
    EXPECT_EQ(checkResult(killed).error().backendFailure(), BackendFailure::Interrupted);

    // A D-Bus error of the method, without Finished (prototype 4).
    PkResult refused;
    refused.dbusError = "org.freedesktop.PackageKit.Transaction.InvalidState";
    refused.dbusErrorMessage = "only one method per transaction";
    EXPECT_EQ(checkResult(refused).error().code(), ErrorCode::Backend);
    PkResult gone;
    gone.dbusError = "org.freedesktop.DBus.Error.ServiceUnknown";
    EXPECT_EQ(checkResult(gone).error().backendFailure(), BackendFailure::Unavailable);
}

TEST(PackageKitResult, PhasesAndProgress)
{
    EXPECT_FALSE(phaseOf(static_cast<std::uint32_t>(Status::LoadingCache)));
    EXPECT_FALSE(phaseOf(static_cast<std::uint32_t>(Status::Setup)));
    EXPECT_EQ(phaseOf(static_cast<std::uint32_t>(Status::WaitingForAuth)), OperationPhase::WaitingForAuth);
    EXPECT_EQ(phaseOf(static_cast<std::uint32_t>(Status::WaitingForLock)), OperationPhase::WaitingForLock);
    EXPECT_EQ(phaseOf(static_cast<std::uint32_t>(Status::Download)), OperationPhase::Downloading);
    EXPECT_EQ(phaseOf(static_cast<std::uint32_t>(Status::Commit)), OperationPhase::Installing);
    EXPECT_EQ(phaseOf(static_cast<std::uint32_t>(Status::Remove)), OperationPhase::Removing);
    EXPECT_EQ(phaseOf(static_cast<std::uint32_t>(Status::Finished)), OperationPhase::Finishing);
    EXPECT_FALSE(phaseOf(999));

    const Progress p = progressOf({.status = static_cast<std::uint32_t>(Status::Download), .percentage = 40,
                                   .allowCancel = true, .downloadSizeRemaining = 30, .item = "gimp;3.0.4;amd64;x"},
                                  100);
    EXPECT_DOUBLE_EQ(p.fraction, 0.4);
    EXPECT_EQ(p.downloadedBytes, 70u);
    EXPECT_EQ(p.totalBytes, 100u);
    EXPECT_TRUE(p.cancellable);
    EXPECT_EQ(p.item, "gimp");
    EXPECT_DOUBLE_EQ(progressOf({.percentage = 101}, 0).fraction, -1.0); // unknown
    EXPECT_EQ(statusName(31), "waiting-for-auth");
    EXPECT_EQ(exitName(9), "cancelled-priority");
    EXPECT_EQ(errorName(48), "not-authorized");
}

// --- Inventory and updates ------------------------------------------------------------

TEST(PackageKitInventory, InstalledPackagesForTheNativeArchitecture)
{
    const auto items = unwrap(installedItems(
            success({{1, "bash;5.2.37-2+b10;amd64;manual:debian-stable-main", "GNU Bourne Again SHell"},
                     {1, "bash;5.2.37-2+b10;amd64;manual:debian-stable-main", "repeated id"},
                     {1, "zlib1g;1:1.3.dfsg+really1.3.1-1+b1;i386;auto:debian-stable-main", "foreign"},
                     {1, "cutefish-desktop;1.0;all;manual:cutefish-trixie-nightly-main", "all"},
                     {1, "cutefish-calculator;1.0~git20261104215447.9aa396e-1;amd64;manual:local", "local"}}),
            debianProfile()));
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].offer().str(), "packagekit:bash");
    EXPECT_EQ(items[0].version()->str(), "5.2.37-2+b10");
    EXPECT_EQ(items[0].origin(), "debian-stable-main");
    EXPECT_FALSE(items[0].app()); // the host links it to the catalogue
    EXPECT_EQ(items[1].offer().str(), "packagekit:cutefish-desktop");
    EXPECT_EQ(items[2].origin(), "local");
    EXPECT_FALSE(installedItems(failure("internal-error"), debianProfile()));
}

TEST(PackageKitInventory, UpdatesWithoutIdenticalVersions)
{
    const auto installed = unwrap(installedItems(
            success({{1, "cutefish-calculator;1.0~git20261104215447.9aa396e-1;amd64;auto:local", ""},
                     {1, "cutefish-dm;1.0~git20261028062042.77e1229-1;amd64;auto:local", ""}}),
            debianProfile()));
    // GetUpdates as PackageKit 1.3.1 answered: info 327685 (normal, severity
    // normal); cutefish-dm is listed at its installed version.
    const auto updates = unwrap(availableUpdates(
            success({{327685, "cutefish-calculator;1.0~git20261106141003.538e010-1;amd64;cutefish-trixie-nightly-main", ""},
                     {327685, "cutefish-dm;1.0~git20261028062042.77e1229-1;amd64;auto:local", ""},
                     {327685, "zlib1g;1:1.3.dfsg+really1.3.1-1+deb13u1;i386;debian-stable-security-main", ""}}),
            installed, debianProfile()));
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].offer.str(), "packagekit:cutefish-calculator");
    EXPECT_EQ(updates[0].fromVersion->str(), "1.0~git20261104215447.9aa396e-1");
    EXPECT_EQ(updates[0].toVersion->str(), "1.0~git20261106141003.538e010-1");
}

// --- Plans ----------------------------------------------------------------------------

TEST(PackageKitPlans, InstallWithDependencies)
{
    const auto plan = unwrap(planFromSimulation(
            request(TransactionKind::Install, {"gimp"}),
            success({{12, "fonts-urw-base35;20200910-8;all;+auto:debian-stable-main", ""},
                     {12, "ghostscript;10.05.1~dfsg-1+deb13u3;amd64;+auto:debian-stable-security-main", ""},
                     {12, "gimp;3.0.4-3+deb13u11;amd64;+manual:debian-stable-security-main", ""}},
                    52458144),
            debianProfile()));
    EXPECT_EQ(plan.downloadSize, 52458144u);
    EXPECT_EQ(plan.plan.downloadSize(), 52458144u); // what the user is shown
    ASSERT_EQ(plan.plan.items().size(), 3u);
    const PlanItem* gimp = item(plan.plan, "gimp");
    ASSERT_TRUE(gimp);
    EXPECT_TRUE(gimp->requested());
    EXPECT_EQ(gimp->kind(), PlanItemKind::Application);
    EXPECT_EQ(gimp->toVersion()->str(), "3.0.4-3+deb13u11"); // the version really planned
    EXPECT_EQ(gimp->origin(), "debian-stable-security-main");
    EXPECT_FALSE(item(plan.plan, "ghostscript")->requested());
    EXPECT_TRUE(checkPlanForRequest(request(TransactionKind::Install, {"gimp"}), plan.plan));
    EXPECT_FALSE(assessPlan(plan.plan).required()); // distribution packages: no confirmation
}

TEST(PackageKitPlans, AConflictRemovesPackagesAndAsksForConfirmation)
{
    // Installing pulseaudio removes two pipewire packages (prototype 4).
    const auto plan = unwrap(planFromSimulation(
            request(TransactionKind::Install, {"pulseaudio"}),
            success({{12, "libltdl7;2.5.4-4;amd64;+auto:debian-stable-main", ""},
                     {12, "pulseaudio;17.0+dfsg1-2+b1;amd64;+manual:debian-stable-main", ""},
                     {13, "pipewire-alsa;1.4.2-1;amd64;auto:debian-stable-main", ""},
                     {13, "pipewire-audio;1.4.2-1;all;manual:debian-stable-main", ""}}),
            debianProfile()));
    const PlanItem* removed = item(plan.plan, "pipewire-audio");
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->operation(), PlanOperation::Remove);
    EXPECT_FALSE(removed->requested());
    EXPECT_EQ(removed->fromVersion()->str(), "1.4.2-1");
    EXPECT_FALSE(removed->toVersion());
    const auto a = assessPlan(plan.plan);
    EXPECT_TRUE(a.reasons.contains(ConfirmationReason::RemovesPackages));
}

TEST(PackageKitPlans, ForeignArchitecturesAreSeparateItems)
{
    const auto plan = unwrap(planFromSimulation(
            request(TransactionKind::Update, {"zlib1g"}),
            success({{11, "zlib1g;1:1.3.dfsg+really1.3.1-1+deb13u1;amd64;debian-stable-security-main", ""},
                     {11, "zlib1g;1:1.3.dfsg+really1.3.1-1+deb13u1;i386;debian-stable-security-main", ""},
                     {11, "zlib1g;1:1.3.dfsg+really1.3.1-1+deb13u1;i386;debian-stable-security-main", "again"}}),
            debianProfile()));
    ASSERT_EQ(plan.plan.items().size(), 2u);
    EXPECT_TRUE(item(plan.plan, "zlib1g")->requested());
    ASSERT_TRUE(item(plan.plan, "zlib1g:i386"));
    EXPECT_FALSE(item(plan.plan, "zlib1g:i386")->requested());
}

TEST(PackageKitPlans, ProtectedPackagesAreNeverRemoved)
{
    // Removing cutefish-calculator takes cutefish-desktop along (prototype 4).
    auto r = planFromSimulation(request(TransactionKind::Remove, {"cutefish-calculator"}),
                                success({{13, "cutefish-calculator;1.0-1;amd64;manual:local", ""},
                                         {13, "cutefish-desktop;1.0;all;manual:cutefish-trixie-nightly-main", ""}}),
                                debianProfile());
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message().find("cutefish-desktop"), std::string::npos);
    EXPECT_FALSE(planFromSimulation(request(TransactionKind::Remove, {"cutefish-store"}),
                                    success({{13, "cutefish-store;0.1-1;amd64;manual:local", ""}}), debianProfile()));
}

TEST(PackageKitPlans, UnclassifiableOrEmptySimulationsAreRefused)
{
    auto unknown = planFromSimulation(request(TransactionKind::Install, {"gimp"}),
                                      success({{9, "gimp;3.0.4;amd64;+manual:debian-stable-main", ""}}), debianProfile());
    ASSERT_FALSE(unknown);
    auto nothing = planFromSimulation(request(TransactionKind::Install, {"bash"}), success({}), debianProfile());
    ASSERT_FALSE(nothing);
    EXPECT_EQ(nothing.error().code(), ErrorCode::Conflict);
    auto failed = planFromSimulation(request(TransactionKind::Remove, {"zlib1g"}), failure("dep-resolution-failed"),
                                     debianProfile());
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().backendFailure(), BackendFailure::Dependency);
}

TEST(PackageKitPlans, AVersionReplacedByPackageKitIsVisible)
{
    // Asked for gimp;0.1, PackageKit planned 3.0.4 (prototype 4): the plan
    // shows the version really planned, never the one asked for.
    const auto plan = unwrap(planFromSimulation(
            request(TransactionKind::Install, {"gimp"}),
            success({{12, "gimp;3.0.4-3+deb13u11;amd64;+manual:debian-stable-security-main", ""}}), debianProfile()));
    EXPECT_EQ(item(plan.plan, "gimp")->toVersion()->str(), "3.0.4-3+deb13u11");
}

TEST(PackageKitPlans, ADifferentSecondSimulationNeedsConfirmation)
{
    const auto request1 = request(TransactionKind::Install, {"gimp"});
    const auto first = unwrap(planFromSimulation(
            request1, success({{12, "gimp;3.0.4-3+deb13u11;amd64;+manual:debian-stable-main", ""}}), debianProfile()));
    // Same versions: may run. A new dependency, or another version: not.
    const auto same = unwrap(planFromSimulation(
            request1, success({{12, "gimp;3.0.4-3+deb13u11;amd64;+manual:debian-stable-main", ""}}), debianProfile()));
    EXPECT_TRUE(mayExecute(first.plan, same.plan));
    const auto newer = unwrap(planFromSimulation(
            request1, success({{12, "gimp;3.0.4-3+deb13u12;amd64;+manual:debian-stable-security-main", ""}}),
            debianProfile()));
    EXPECT_FALSE(mayExecute(first.plan, newer.plan));
    const auto more = unwrap(planFromSimulation(
            request1, success({{12, "gimp;3.0.4-3+deb13u11;amd64;+manual:debian-stable-main", ""},
                               {12, "libnew1;1.0-1;amd64;+auto:debian-stable-main", ""}}),
            debianProfile()));
    EXPECT_FALSE(mayExecute(first.plan, more.plan));
}

TEST(PackageKitPlans, IdsForTheRequest)
{
    const auto r = request(TransactionKind::Install, {"gimp", "fonts-urw-base35"});
    const auto ids = unwrap(idsFor(r,
                                   success({{2, "gimp;3.0.4-3+deb13u11;i386;debian-stable-main", ""},
                                            {2, "gimp;3.0.4-3+deb13u11;amd64;debian-stable-main", ""},
                                            {2, "fonts-urw-base35;20200910-8;all;debian-stable-main", ""}}),
                                   debianProfile()));
    EXPECT_EQ(ids, (std::vector<std::string>{"gimp;3.0.4-3+deb13u11;amd64;debian-stable-main",
                                             "fonts-urw-base35;20200910-8;all;debian-stable-main"}));
    auto missing = idsFor(request(TransactionKind::Install, {"nonexistent"}), success({}), debianProfile());
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

// --- Debian profile ---------------------------------------------------------------------

TEST(DebianProfile, ArchitectureAndApplicability)
{
    EXPECT_EQ(debian::dpkgArchitecture("debian;13;x86_64"), "amd64");
    EXPECT_EQ(debian::dpkgArchitecture("debian;13;aarch64"), "arm64");
    EXPECT_EQ(debian::dpkgArchitecture("debian;13;sparc"), "");
    EXPECT_TRUE(debian::applies("debian;13;x86_64"));
    EXPECT_FALSE(debian::applies("ubuntu;24.04;x86_64"));
    EXPECT_FALSE(debian::applies("fedora;41;x86_64"));
}

TEST(DebianProfile, RepositoriesClassifiedByHost)
{
    auto repo = debian::parseRepositoryId(
            "/etc/apt/sources.list:deb:http://deb.debian.org/debian/:trixie:main,contrib,non-free-firmware");
    ASSERT_TRUE(repo);
    EXPECT_EQ(repo->file, "/etc/apt/sources.list");
    EXPECT_EQ(repo->type, "deb");
    EXPECT_EQ(repo->uri, "http://deb.debian.org/debian/");
    EXPECT_EQ(repo->host, "deb.debian.org");
    EXPECT_EQ(repo->suite, "trixie");
    EXPECT_EQ(repo->components, "main,contrib,non-free-firmware");

    EXPECT_EQ(debian::classifyHost("deb.debian.org"), SourceKind::Distribution);
    EXPECT_EQ(debian::classifyHost("security.debian.org"), SourceKind::Distribution);
    EXPECT_EQ(debian::classifyHost("ftp.fr.debian.org"), SourceKind::Distribution);
    EXPECT_EQ(debian::classifyHost("download.cutefish.org"), SourceKind::ThirdParty);
    EXPECT_EQ(debian::classifyHost("notdebian.org"), SourceKind::ThirdParty);

    auto s = debian::repositorySource(
            "/etc/apt/sources.list:deb:http://security.debian.org/:trixie-security:main,contrib,non-free-firmware",
            "security.debian.org - Trixie security (main contrib non-free-firmware)", true);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->kind(), SourceKind::Distribution);
    EXPECT_EQ(s->scope(), Scope::System);
    EXPECT_EQ(s->url(), "http://security.debian.org/");
    auto third = debian::repositorySource(
            "/etc/apt/sources.list.d/cutefish.list:deb:https://download.cutefish.org/debian/:trixie-nightly:main",
            "download.cutefish.org/debian - Trixie nightly (main)", true);
    ASSERT_TRUE(third);
    EXPECT_EQ(third->kind(), SourceKind::ThirdParty);
    // Source repositories are not offered.
    EXPECT_FALSE(debian::repositorySource(
            "/etc/apt/sources.list:deb-src:http://deb.debian.org/debian/:trixie:main", "Sources", true));
    EXPECT_FALSE(debian::repositorySource("garbage", "x", true));
}

TEST(DebianProfile, ProtectedPackages)
{
    EXPECT_TRUE(debian::isProtected("cutefish-desktop"));
    EXPECT_TRUE(debian::isProtected("cutefish-store"));
    EXPECT_TRUE(debian::isProtected("cutefish-desktop:i386"));
    EXPECT_FALSE(debian::isProtected("cutefish-calculator"));
}

TEST(DebianProfile, CatalogueSourcesFromSwcatalog)
{
    TempDir dir;
    std::filesystem::create_directories(dir / "yaml");
    std::filesystem::create_directories(dir.path() / "icons" / "debian-trixie-main");
    for (const char* name : {"deb.debian.org_debian_dists_trixie_main_dep11_Components-amd64.yml.gz",
                             "deb.debian.org_debian_dists_trixie_contrib_dep11_Components-amd64.yml.gz",
                             "download.cutefish.org_debian_dists_trixie-nightly_main_dep11_Components-amd64.yml.gz",
                             "README", "weird.yml.gz"})
        std::ofstream(dir.path() / "yaml" / name) << "x";
    const auto sources = debian::catalogSources(dir.path());
    ASSERT_EQ(sources.size(), 3u);
    EXPECT_EQ(sources[0].source.id().str(), "packagekit:debian-trixie-contrib");
    EXPECT_TRUE(sources[0].files.icons.empty()); // no icon directory of that name
    EXPECT_EQ(sources[1].source.id().str(), "packagekit:debian-trixie-main");
    EXPECT_EQ(sources[1].source.kind(), SourceKind::Distribution);
    EXPECT_EQ(sources[1].files.icons, dir.path() / "icons" / "debian-trixie-main");
    EXPECT_EQ(sources[2].source.id().str(), "packagekit:debian-trixie-nightly-main");
    EXPECT_EQ(sources[2].source.kind(), SourceKind::ThirdParty);
    EXPECT_TRUE(debian::catalogSources(dir / "missing").empty());
}

// --- Other programs' transactions (H.2, SERVICE.md §22) --------------------------------

namespace {

PkTransactionInfo tx(const char* path, Role role, Status status = Status::Running, std::uint64_t flags = 0)
{
    return {path, static_cast<std::uint32_t>(role), static_cast<std::uint32_t>(status), flags};
}

} // namespace

TEST(PackageKitActivity, NothingRunningIsIdle)
{
    EXPECT_EQ(externalActivity({}).kind, ExternalActivity::Kind::None);
    EXPECT_TRUE(externalActivity({}).role.empty());
}

TEST(PackageKitActivity, QueriesAndSimulationsOnlyCheck)
{
    // Cutefish Software Update's check: RefreshCache, then GetUpdates.
    const std::vector<PkTransactionInfo> getUpdates{tx("/1", Role::GetUpdates, Status::Query)};
    EXPECT_EQ(externalActivity(getUpdates).kind, ExternalActivity::Kind::Checking);
    EXPECT_EQ(externalActivity(getUpdates).role, "get-updates");
    EXPECT_FALSE(externalActivity(getUpdates).blocksChanges());
    // A simulated install changes nothing.
    const std::vector<PkTransactionInfo> simulation{
            tx("/1", Role::InstallPackages, Status::DepResolve, flag::kOnlyTrusted | flag::kSimulate)};
    EXPECT_EQ(externalActivity(simulation).kind, ExternalActivity::Kind::Checking);
    EXPECT_EQ(externalActivity(simulation).role, "install-packages");
}

TEST(PackageKitActivity, RefreshesAndDownloadsChangeNoPackage)
{
    const std::vector<PkTransactionInfo> refresh{tx("/1", Role::RefreshCache, Status::RefreshCache)};
    EXPECT_EQ(externalActivity(refresh).kind, ExternalActivity::Kind::Refreshing);
    EXPECT_FALSE(externalActivity(refresh).blocksChanges());
    const std::vector<PkTransactionInfo> onlyDownload{
            tx("/1", Role::UpdatePackages, Status::Download, flag::kOnlyTrusted | flag::kOnlyDownload)};
    EXPECT_EQ(externalActivity(onlyDownload).kind, ExternalActivity::Kind::Refreshing);
}

TEST(PackageKitActivity, InstallsRemovalsAndUpdatesChangeTheSystem)
{
    // Cutefish Software Update's update(): UpdatePackages, only-trusted.
    const std::vector<PkTransactionInfo> update{tx("/1", Role::UpdatePackages, Status::Install, flag::kOnlyTrusted)};
    const ExternalActivity a = externalActivity(update);
    EXPECT_EQ(a.kind, ExternalActivity::Kind::Changing);
    EXPECT_TRUE(a.blocksChanges());
    EXPECT_EQ(a.role, "update-packages");
    EXPECT_EQ(a.status, "install");
    for (const Role r : {Role::InstallFiles, Role::InstallPackages, Role::InstallSignature, Role::RemovePackages,
                         Role::RepoEnable, Role::RepoSetData, Role::UpgradeSystem, Role::RepairSystem,
                         Role::RepoRemove}) {
        const std::vector<PkTransactionInfo> one{tx("/1", r, Status::Wait)};
        EXPECT_TRUE(externalActivity(one).blocksChanges()) << roleName(static_cast<std::uint32_t>(r));
    }
}

TEST(PackageKitActivity, NotStartedAndFinishedTransactionsAreLeftOut)
{
    const std::vector<PkTransactionInfo> list{tx("/1", Role::Unknown, Status::Wait),
                                              tx("/2", Role::UpdatePackages, Status::Finished),
                                              tx("/3", Role::Cancel, Status::Running)};
    EXPECT_EQ(externalActivity(list).kind, ExternalActivity::Kind::None);
}

TEST(PackageKitActivity, TheMostSignificantWinsDeterministically)
{
    const std::vector<PkTransactionInfo> a{tx("/3", Role::GetUpdates), tx("/2", Role::RemovePackages),
                                           tx("/1", Role::RefreshCache), tx("/4", Role::InstallPackages)};
    std::vector<PkTransactionInfo> b(a.rbegin(), a.rend());
    EXPECT_EQ(externalActivity(a), externalActivity(b));
    EXPECT_EQ(externalActivity(a).kind, ExternalActivity::Kind::Changing);
    EXPECT_EQ(externalActivity(a).role, "remove-packages"); // "/2" < "/4"
}
