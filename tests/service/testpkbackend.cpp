// PackageKitBackend against a simulated PackageKit (fakepackagekit.h) on a
// private bus: the Backend contract (backend.h) through the real D-Bus
// transport. These tests never reach the system's packagekitd.

#include "fakepackagekit.h"
#include "privatebus.h"

#include "pkbackend.h"

#include <cutefish/store/confirmation.h>
#include <cutefish/store/transaction.h>

#include <gtest/gtest.h>

#include <fstream>

using namespace cutefish::store;
using namespace cutefish::store::service;
using namespace std::chrono_literals;
using fakepk::Answer;
using fakepk::Step;
namespace pk = cutefish::store::packagekit;
using fixtures::unwrap;

namespace {

pk::PkPackage pkg(pk::Info info, const char* id) { return {static_cast<std::uint32_t>(info), id, "summary"}; }

PlanRequest request(TransactionKind kind, std::vector<std::string> names)
{
    std::vector<OfferId> offers;
    for (const auto& n : names)
        offers.push_back(unwrap(OfferId::parse("packagekit:" + n)));
    return unwrap(PlanRequest::create(kind, std::move(offers)));
}

const char* kGimpAvailable = "gimp;3.0.4-3+deb13u11;amd64;debian-stable-security-main";

Answer gimpInstallPlan(const char* gimpVersion = "3.0.4-3+deb13u11")
{
    Answer a;
    a.packages = {pkg(pk::Info::Installing, "libgimp-3.0-0;3.0.4-3;amd64;+auto:debian-stable-main"),
                  pkg(pk::Info::Installing, (std::string("gimp;") + gimpVersion + ";amd64;+manual:debian-stable-main").c_str())};
    a.downloadSize = 52458144;
    return a;
}

class PkBackendTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        resolveGimp();
        backend = std::make_unique<PackageKitBackend>(
                loop, PackageKitBackend::Options{.bus = PkTransport::Bus::Session,
                                                 .swcatalog = bus.dir() / "swcatalog",
                                                 .changeDelay = 150ms,
                                                 .queryTimeout = 3s,
                                                 .transactionTimeout = 5s});
    }

    void resolveGimp()
    {
        Answer resolve;
        resolve.packages = {pkg(pk::Info::Available, kGimpAvailable)};
        fake->answer("Resolve", resolve);
    }

    /// Runs the loop until pred() or the timeout.
    bool runUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 10s)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            const int t = loop.startTimer(10ms, false, [this] { loop.quit(0); });
            loop.run();
            loop.stopTimer(t);
        }
        return true;
    }

    template <typename T>
    std::optional<Result<T>> wait(const std::function<void(Completion<T>)>& start)
    {
        std::optional<Result<T>> out;
        start([&](Result<T> r) { out.emplace(std::move(r)); });
        EXPECT_FALSE(out) << "a completion must never run inside the call";
        runUntil([&] { return out.has_value(); });
        return out;
    }

    std::vector<fakepk::Call> realCalls(const std::string& method)
    {
        std::vector<fakepk::Call> out;
        for (const auto& c : fake->calls())
            if (c.method == method && !c.simulate())
                out.push_back(c);
        return out;
    }

    privatebus::PrivateBus bus; // first: the address the others use
    std::unique_ptr<fakepk::FakePackageKit> fake = std::make_unique<fakepk::FakePackageKit>();
    cutefish::base::EventLoop loop;
    std::unique_ptr<PackageKitBackend> backend;
};

bool hasHint(const fakepk::Call& c, const std::string& hint)
{
    return std::find(c.hints.begin(), c.hints.end(), hint) != c.hints.end();
}

} // namespace

// --- Availability and capabilities -------------------------------------------------------

TEST_F(PkBackendTest, AvailableWithTheDebianAptBackend)
{
    auto r = wait<void>([&](Completion<void> d) { backend->availability(std::move(d)); });
    ASSERT_TRUE(r && *r) << (r ? r->error().message() : "no answer");
    const auto caps = backend->capabilities();
    EXPECT_TRUE(caps.systemScope);
    EXPECT_FALSE(caps.userScope);
    EXPECT_FALSE(caps.permissions);
    EXPECT_FALSE(caps.cancelAnytime);
    EXPECT_TRUE(caps.reattach);
    EXPECT_FALSE(caps.verifiesExecutedPlan); // D-017: a new simulation only
    EXPECT_EQ(backend->kind(), BackendKind::PackageKit);
}

TEST_F(PkBackendTest, UnavailableReasons)
{
    auto expectUnavailable = [&](const char* fragment) {
        auto r = wait<void>([&](Completion<void> d) { backend->availability(std::move(d)); });
        ASSERT_TRUE(r && !*r);
        EXPECT_EQ(r->error().backendFailure(), BackendFailure::Unavailable);
        EXPECT_NE(r->error().message().find(fragment), std::string::npos) << r->error().message();
    };
    fake->distroId = "fedora;41;x86_64";
    expectUnavailable("not supported");
    fake->distroId = "debian;13;x86_64";
    fake->roles = ~pk::bit(pk::Role::RemovePackages);
    expectUnavailable("remove-packages");
    fake->roles = ~std::uint64_t(0);
    fake->stop(); // no daemon at all
    expectUnavailable("not reachable");
}

// --- Queries -------------------------------------------------------------------------------

TEST_F(PkBackendTest, InstalledPackages)
{
    Answer a;
    a.packages = {pkg(pk::Info::Installed, "bash;5.2.37-2+b10;amd64;manual:debian-stable-main"),
                  pkg(pk::Info::Installed, "zlib1g;1:1.3.dfsg-1+b1;i386;auto:debian-stable-main"),
                  pkg(pk::Info::Installed, "gimp;3.0.4-3;amd64;manual:debian-stable-main")};
    fake->answer("GetPackages", a);
    auto r = wait<std::vector<InstalledItem>>([&](auto d) { backend->queryInstalled(std::move(d)); });
    ASSERT_TRUE(r && *r) << r->error().message();
    ASSERT_EQ((*r)->size(), 2u); // the foreign architecture is left out
    EXPECT_EQ((**r)[0].offer().str(), "packagekit:bash");
    const auto calls = fake->calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].method, "GetPackages");
    EXPECT_EQ(calls[0].flags, pk::filter::kInstalled | pk::filter::kArch);
    EXPECT_TRUE(hasHint(calls[0], "interactive=false")); // a query never asks polkit
    EXPECT_TRUE(hasHint(calls[0], "supports-plural-signals=true"));
}

TEST_F(PkBackendTest, SourcesAndCatalogueSources)
{
    Answer repos;
    repos.repos = {{"/etc/apt/sources.list:deb:http://deb.debian.org/debian/:trixie:main", "Debian Trixie (main)", true},
                   {"/etc/apt/sources.list:deb-src:http://deb.debian.org/debian/:trixie:main", "Sources", true},
                   {"/etc/apt/sources.list.d/cutefish.list:deb:https://download.cutefish.org/debian/:trixie-nightly:main",
                    "Cutefish", false}};
    fake->answer("GetRepoList", repos);
    auto r = wait<std::vector<BackendSource>>([&](auto d) { backend->sources(std::move(d)); });
    ASSERT_TRUE(r && *r);
    ASSERT_EQ((*r)->size(), 2u); // deb-src left out
    EXPECT_EQ((**r)[0].source.kind(), SourceKind::Distribution);
    EXPECT_EQ((**r)[1].source.kind(), SourceKind::ThirdParty);
    EXPECT_FALSE((**r)[1].source.enabled()); // a disabled repository is listed
    // The DEP-11 source each repository feeds; nothing PackageKit cannot tell.
    ASSERT_EQ((**r)[0].catalogueSources.size(), 1u);
    EXPECT_EQ((**r)[0].catalogueSources[0].str(), "packagekit:debian-trixie-main");
    EXPECT_FALSE((**r)[0].verifiesSignatures);
    EXPECT_FALSE((**r)[0].installedItems);

    std::filesystem::create_directories(bus.dir() / "swcatalog" / "yaml");
    std::ofstream(bus.dir() / "swcatalog" / "yaml" / "deb.debian.org_debian_dists_trixie_main_dep11_Components-amd64.yml.gz")
            << "x";
    auto c = wait<std::vector<SourceMetadata>>([&](auto d) { backend->catalogSources(std::move(d)); });
    ASSERT_TRUE(c && *c);
    ASSERT_EQ((*c)->size(), 1u);
    EXPECT_EQ((**c)[0].source.id().str(), "packagekit:debian-trixie-main");
}

TEST_F(PkBackendTest, RefusesEveryRepositoryChange)
{
    // D-039: the distribution's repositories are read-only. Nothing is sent
    // to packagekitd (no RepoEnable, no RepoRemove), whatever the change.
    EXPECT_FALSE(backend->capabilities().userSourceChanges);
    const std::size_t before = fake->calls().size();
    const SourceId repo = SourceId::parse("packagekit:/etc/apt/sources.list:deb:http://deb.debian.org/debian/:trixie:main").value();
    for (const auto action : {SourceRequest::Action::Enable, SourceRequest::Action::Disable, SourceRequest::Action::Remove}) {
        auto r = wait<void>([&](auto d) { backend->changeSource("op", {.source = repo, .action = action}, std::move(d)); });
        ASSERT_TRUE(r);
        ASSERT_FALSE(*r);
        EXPECT_EQ(r->error().code(), ErrorCode::NotSupported);
    }
    EXPECT_EQ(fake->calls().size(), before);
}

TEST_F(PkBackendTest, UpdatesWithoutIdenticalVersions)
{
    Answer installed;
    installed.packages = {pkg(pk::Info::Installed, "gimp;3.0.4-3;amd64;manual:debian-stable-main"),
                          pkg(pk::Info::Installed, "cutefish-dm;1.0-1;amd64;auto:local")};
    fake->answer("GetPackages", installed);
    Answer updates;
    updates.packages = {{327685, "gimp;3.0.4-3+deb13u11;amd64;debian-stable-security-main", ""},
                        {327685, "cutefish-dm;1.0-1;amd64;auto:local", ""}};
    fake->answer("GetUpdates", updates);
    auto r = wait<std::vector<AvailableUpdate>>([&](auto d) { backend->queryUpdates(std::move(d)); });
    ASSERT_TRUE(r && *r) << r->error().message();
    ASSERT_EQ((*r)->size(), 1u);
    EXPECT_EQ((**r)[0].fromVersion->str(), "3.0.4-3");
    EXPECT_EQ((**r)[0].toVersion->str(), "3.0.4-3+deb13u11");
}

TEST_F(PkBackendTest, PermissionsAreNotSupported)
{
    auto r = wait<std::vector<std::string>>(
            [&](auto d) { backend->permissions(unwrap(OfferId::parse("packagekit:gimp")), std::move(d)); });
    ASSERT_TRUE(r && !*r);
    EXPECT_EQ(r->error().code(), ErrorCode::NotSupported);
}

// --- Plans -----------------------------------------------------------------------------------

TEST_F(PkBackendTest, PlanIsASimulationThatChangesNothing)
{
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    auto r = wait<Plan>([&](auto d) { backend->plan("tx-1", request(TransactionKind::Install, {"gimp"}), std::move(d)); });
    ASSERT_TRUE(r && *r) << r->error().message();
    EXPECT_EQ((*r)->items().size(), 2u);
    EXPECT_TRUE((*r)->find("gimp")->requested());
    EXPECT_TRUE(checkPlanForRequest(request(TransactionKind::Install, {"gimp"}), **r));

    const auto calls = fake->calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].method, "Resolve");
    EXPECT_EQ(calls[0].flags, pk::filter::kNotInstalled | pk::filter::kArch);
    EXPECT_EQ(calls[0].values, std::vector<std::string>{"gimp"});
    EXPECT_EQ(calls[1].method, "InstallPackages");
    EXPECT_EQ(calls[1].flags, pk::flag::kOnlyTrusted | pk::flag::kSimulate);
    EXPECT_EQ(calls[1].values, std::vector<std::string>{kGimpAvailable});
    EXPECT_TRUE(hasHint(calls[1], "interactive=false"));
    EXPECT_TRUE(realCalls("InstallPackages").empty());
}

TEST_F(PkBackendTest, RemovalPlanWithCascadeAndProtection)
{
    Answer installed;
    installed.packages = {pkg(pk::Info::Installed, "cutefish-calculator;1.0-1;amd64;manual:local")};
    fake->replace("Resolve", installed);
    Answer cascade;
    cascade.packages = {pkg(pk::Info::Removing, "cutefish-calculator;1.0-1;amd64;manual:local"),
                        pkg(pk::Info::Removing, "cutefish-desktop;1.0;all;manual:cutefish-trixie-nightly-main")};
    fake->answer("RemovePackages", cascade, true);
    auto r = wait<Plan>(
            [&](auto d) { backend->plan("tx-1", request(TransactionKind::Remove, {"cutefish-calculator"}), std::move(d)); });
    ASSERT_TRUE(r && !*r); // cutefish-desktop is protected
    EXPECT_NE(r->error().message().find("cutefish-desktop"), std::string::npos);
    const auto calls = fake->calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].flags, pk::filter::kInstalled | pk::filter::kArch);
    EXPECT_EQ(calls[1].method, "RemovePackages");
    EXPECT_TRUE(calls[1].simulate());
}

TEST_F(PkBackendTest, PlanningErrors)
{
    Answer failed;
    failed.exit = pk::Exit::Failed;
    failed.error = fakepk::errorCode("dep-resolution-failed");
    failed.errorDetails = "unmet dependencies";
    fake->answer("InstallPackages", failed, true);
    auto r = wait<Plan>([&](auto d) { backend->plan("tx-1", request(TransactionKind::Install, {"gimp"}), std::move(d)); });
    ASSERT_TRUE(r && !*r);
    EXPECT_EQ(r->error().backendFailure(), BackendFailure::Dependency);

    Answer unknown; // Resolve finds nothing
    fake->answer("Resolve", unknown);
    fake->answer("Resolve", unknown);
    auto missing = wait<Plan>([&](auto d) { backend->plan("tx-2", request(TransactionKind::Install, {"nothing"}), std::move(d)); });
    ASSERT_TRUE(missing && !*missing);
    EXPECT_EQ(missing->error().code(), ErrorCode::NotFound);
}

TEST_F(PkBackendTest, AMethodRefusedOnDBus)
{
    Answer refused;
    refused.dbusError = "org.freedesktop.PackageKit.Transaction.InvalidState";
    fake->answer("GetPackages", refused);
    auto r = wait<std::vector<InstalledItem>>([&](auto d) { backend->queryInstalled(std::move(d)); });
    ASSERT_TRUE(r && !*r);
    EXPECT_NE(r->error().message().find("InvalidState"), std::string::npos);
}

TEST_F(PkBackendTest, AHangingDaemonTimesOut)
{
    Answer hang;
    hang.hang = true;
    fake->answer("GetPackages", hang);
    auto r = wait<std::vector<InstalledItem>>([&](auto d) { backend->queryInstalled(std::move(d)); });
    ASSERT_TRUE(r && !*r);
    EXPECT_NE(r->error().message().find("Timeout"), std::string::npos) << r->error().message();
}

// --- Execution -------------------------------------------------------------------------------

TEST_F(PkBackendTest, ExecutionAfterAVerifiedNewSimulation)
{
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    Answer real;
    real.steps = {{pk::Status::WaitingForAuth, 101, true},
                  {pk::Status::Download, 30, true, "gimp;3.0.4-3+deb13u11;amd64;x"},
                  {pk::Status::Install, 80, false, "gimp;3.0.4-3+deb13u11;amd64;x"}};
    real.downloadSize = 52458144;
    fake->answer("InstallPackages", real, false);

    // Driven as the service will: plan, transaction, execution.
    const PlanRequest req = request(TransactionKind::Install, {"gimp"});
    Transaction t(TransactionKind::Install);
    ASSERT_TRUE(t.startPlanning());
    auto plan = wait<Plan>([&](auto d) { backend->plan("tx-1", req, std::move(d)); });
    ASSERT_TRUE(plan && *plan);
    ASSERT_TRUE(t.planReady(**plan));
    ASSERT_EQ(t.state(), TransactionState::Planning); // no confirmation needed

    std::vector<std::string> events;
    std::vector<Progress> progress;
    OperationEvents ev;
    ev.started = [&](const Plan& p) {
        events.push_back("started");
        ASSERT_EQ(unwrap(t.verifyExecutionPlan(p)), PlanVerdict::Identical);
    };
    ev.handle = [&](const std::string& h) { events.push_back("handle:" + h); };
    ev.progress = [&](const Progress& p) {
        events.push_back("progress");
        progress.push_back(p);
    };
    auto r = wait<ExecutionResult>([&](auto d) { backend->execute("tx-1", req, *t.acceptedPlan(), ev, std::move(d)); });
    ASSERT_TRUE(r && *r) << r->error().message();
    EXPECT_EQ((*r)->outcome, ExecutionResult::Outcome::Completed);
    ASSERT_TRUE(t.succeed());

    ASSERT_GE(events.size(), 3u);
    EXPECT_EQ(events[0], "started");
    EXPECT_EQ(events[1].rfind("handle:/", 0), 0u);
    ASSERT_FALSE(progress.empty());
    bool sawAuth = false, sawDownload = false, sawInstall = false;
    for (const Progress& p : progress) {
        sawAuth = sawAuth || p.phase == OperationPhase::WaitingForAuth;
        sawDownload = sawDownload || p.phase == OperationPhase::Downloading;
        sawInstall = sawInstall || p.phase == OperationPhase::Installing;
    }
    EXPECT_TRUE(sawAuth && sawDownload && sawInstall);

    // Resolve + simulation for the plan, then Resolve + new simulation +
    // the real transaction, interactive, without SIMULATE.
    const auto real2 = realCalls("InstallPackages");
    ASSERT_EQ(real2.size(), 1u);
    EXPECT_EQ(real2[0].flags, pk::flag::kOnlyTrusted);
    EXPECT_TRUE(hasHint(real2[0], "interactive=true"));
    int simulations = 0;
    for (const auto& c : fake->calls())
        simulations += c.method == "InstallPackages" && c.simulate();
    EXPECT_EQ(simulations, 2);
    EXPECT_EQ(fake->maxConcurrent(), 1); // one transaction at a time (P-009)
}

TEST_F(PkBackendTest, ADifferentNewSimulationExecutesNothing)
{
    fake->answer("InstallPackages", gimpInstallPlan("3.0.4-3+deb13u11"), true);
    fake->answer("InstallPackages", gimpInstallPlan("3.0.4-3+deb13u12"), true); // a new version meanwhile
    const PlanRequest req = request(TransactionKind::Install, {"gimp"});
    auto plan = wait<Plan>([&](auto d) { backend->plan("tx-1", req, std::move(d)); });
    ASSERT_TRUE(plan && *plan);
    bool started = false;
    OperationEvents ev;
    ev.started = [&](const Plan&) { started = true; };
    auto r = wait<ExecutionResult>([&](auto d) { backend->execute("tx-1", req, **plan, ev, std::move(d)); });
    ASSERT_TRUE(r && *r);
    EXPECT_EQ((*r)->outcome, ExecutionResult::Outcome::PlanChanged);
    ASSERT_TRUE((*r)->freshPlan);
    EXPECT_EQ((*r)->freshPlan->find("gimp")->toVersion()->str(), "3.0.4-3+deb13u12");
    EXPECT_FALSE(started);
    EXPECT_TRUE(realCalls("InstallPackages").empty()) << "nothing executed";
}

TEST_F(PkBackendTest, ExecutionErrorsArePackageKitsOwn)
{
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    Answer denied;
    denied.exit = pk::Exit::Failed;
    denied.error = fakepk::errorCode("not-authorized");
    denied.errorDetails = "polkit said no";
    fake->answer("InstallPackages", denied, false);
    const PlanRequest req = request(TransactionKind::Install, {"gimp"});
    auto plan = wait<Plan>([&](auto d) { backend->plan("tx-1", req, std::move(d)); });
    auto r = wait<ExecutionResult>([&](auto d) { backend->execute("tx-1", req, **plan, {}, std::move(d)); });
    ASSERT_TRUE(r && !*r);
    EXPECT_EQ(r->error().backendFailure(), BackendFailure::AuthDenied);
    EXPECT_NE(r->error().message().find("polkit said no"), std::string::npos);
}

// --- Cancellation, interruption, reattach ----------------------------------------------------

TEST_F(PkBackendTest, CancelOnlyWhenPackageKitAllowsIt)
{
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    Answer real;
    for (int i = 0; i < 6; ++i)
        real.steps.push_back({pk::Status::Download, std::uint32_t(10 * i), i >= 3});
    fake->answer("InstallPackages", real, false);
    const PlanRequest req = request(TransactionKind::Install, {"gimp"});
    auto plan = wait<Plan>([&](auto d) { backend->plan("tx-1", req, std::move(d)); });

    std::vector<Progress> progress;
    std::optional<Result<ExecutionResult>> result;
    OperationEvents ev;
    ev.progress = [&](const Progress& p) { progress.push_back(p); };
    backend->execute("tx-1", req, **plan, ev, [&](Result<ExecutionResult> r) { result.emplace(std::move(r)); });
    ASSERT_TRUE(runUntil([&] { return !progress.empty() && !progress.back().cancellable; }));
    auto refused = backend->cancel("tx-1");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::NotSupported);
    ASSERT_TRUE(runUntil([&] { return !progress.empty() && progress.back().cancellable; }));
    ASSERT_TRUE(backend->cancel("tx-1"));
    ASSERT_TRUE(runUntil([&] { return result.has_value(); }));
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code(), ErrorCode::Cancelled);
    EXPECT_GE(fake->cancels(), 1);
    EXPECT_EQ(backend->cancel("tx-1").error().code(), ErrorCode::NotFound); // finished
}

TEST_F(PkBackendTest, CancelWhilePlanning)
{
    Answer slow = gimpInstallPlan();
    for (int i = 0; i < 10; ++i)
        slow.steps.push_back({pk::Status::Query, 101, true});
    fake->answer("InstallPackages", slow, true);
    std::optional<Result<Plan>> result;
    backend->plan("tx-1", request(TransactionKind::Install, {"gimp"}), [&](Result<Plan> r) { result.emplace(std::move(r)); });
    ASSERT_TRUE(runUntil([&] { return fake->calls().size() >= 2; }));
    ASSERT_TRUE(backend->cancel("tx-1"));
    ASSERT_TRUE(runUntil([&] { return result.has_value(); }));
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code(), ErrorCode::Cancelled);
}

TEST_F(PkBackendTest, TheDaemonGoingAwayInterruptsTheTransaction)
{
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    Answer real;
    real.hang = true;
    fake->answer("InstallPackages", real, false);
    const PlanRequest req = request(TransactionKind::Install, {"gimp"});
    auto plan = wait<Plan>([&](auto d) { backend->plan("tx-1", req, std::move(d)); });
    std::optional<Result<ExecutionResult>> result;
    std::string handle;
    OperationEvents ev;
    ev.handle = [&](const std::string& h) { handle = h; };
    backend->execute("tx-1", req, **plan, ev, [&](Result<ExecutionResult> r) { result.emplace(std::move(r)); });
    ASSERT_TRUE(runUntil([&] { return !realCalls("InstallPackages").empty(); }));
    fake->stop();
    ASSERT_TRUE(runUntil([&] { return result.has_value(); }));
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().backendFailure(), BackendFailure::Interrupted);
    EXPECT_FALSE(handle.empty()); // what the journal keeps to re-attach
}

TEST_F(PkBackendTest, ReattachToARunningTransaction)
{
    Answer running;
    for (int i = 0; i < 12; ++i) // long enough to be followed from its middle
        running.steps.push_back({pk::Status::Install, std::uint32_t(5 + 7 * i), false});
    const std::string path = fake->startDetached("InstallPackages", running);
    std::vector<Progress> progress;
    OperationEvents ev;
    ev.progress = [&](const Progress& p) { progress.push_back(p); };
    auto r = wait<ExecutionResult>([&](auto d) { backend->reattach("tx-1", path, ev, std::move(d)); });
    ASSERT_TRUE(r && *r) << r->error().message();
    EXPECT_EQ((*r)->outcome, ExecutionResult::Outcome::Completed);
    EXPECT_FALSE(progress.empty());

    auto lost = wait<ExecutionResult>([&](auto d) { backend->reattach("tx-2", "/999_gone", {}, std::move(d)); });
    ASSERT_TRUE(lost && !*lost);
    EXPECT_EQ(lost->error().code(), ErrorCode::NotFound); // the host interrupts it
}

TEST_F(PkBackendTest, ReattachToATransactionThatJustEnded)
{
    // Still listed, already finished: the outcome cannot be known.
    Answer quick;
    const std::string path = fake->startDetached("InstallPackages", quick);
    std::this_thread::sleep_for(100ms); // it finishes; the fake keeps the object
    auto r = wait<ExecutionResult>([&](auto d) { backend->reattach("tx-1", path, {}, std::move(d)); });
    ASSERT_TRUE(r);
    if (*r) {
        // Listed no more by GetTransactionList: NotFound is also correct.
        FAIL() << "a finished transaction cannot complete";
    }
    EXPECT_TRUE(r->error().code() == ErrorCode::NotFound
                || r->error().backendFailure() == BackendFailure::Interrupted)
            << r->error().message();
}

// --- Concurrency and changes ---------------------------------------------------------------

TEST_F(PkBackendTest, OneModifyingOperationAndOneTransactionAtATime)
{
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    Answer real;
    for (int i = 0; i < 5; ++i)
        real.steps.push_back({pk::Status::Download, std::uint32_t(20 * i), true});
    fake->answer("InstallPackages", real, false);
    Answer installed;
    installed.packages = {pkg(pk::Info::Installed, "bash;5.2;amd64;manual:debian-stable-main")};
    fake->answer("GetPackages", installed);
    const PlanRequest req = request(TransactionKind::Install, {"gimp"});
    auto plan = wait<Plan>([&](auto d) { backend->plan("tx-1", req, std::move(d)); });

    std::optional<Result<ExecutionResult>> first, second;
    std::optional<Result<void>> refresh;
    std::optional<Result<std::vector<InstalledItem>>> query;
    backend->execute("tx-1", req, **plan, {}, [&](auto r) { first.emplace(std::move(r)); });
    backend->execute("tx-2", req, **plan, {}, [&](auto r) { second.emplace(std::move(r)); });
    backend->refresh("tx-3", {}, [&](auto r) { refresh.emplace(std::move(r)); });
    backend->queryInstalled([&](auto r) { query.emplace(std::move(r)); });
    ASSERT_TRUE(runUntil([&] { return first && second && refresh && query; }));
    EXPECT_TRUE(*first);
    EXPECT_EQ(second->error().code(), ErrorCode::Conflict);
    EXPECT_EQ(refresh->error().code(), ErrorCode::Conflict);
    EXPECT_TRUE(*query); // queries go on, queued behind the transaction
    EXPECT_EQ(fake->maxConcurrent(), 1);
}

TEST_F(PkBackendTest, RefreshWithoutBackgroundHint)
{
    auto r = wait<void>([&](Completion<void> d) { backend->refresh("tx-1", {}, std::move(d)); });
    ASSERT_TRUE(r && *r);
    const auto calls = realCalls("RefreshCache");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].flags, 0u); // force = false
    for (const auto& h : calls[0].hints)
        EXPECT_EQ(h.find("background"), std::string::npos); // SERVICE.md §6.6, Q-10
}

TEST_F(PkBackendTest, ChangesMadeOutsideAreGrouped)
{
    std::vector<BackendChanges> seen;
    backend->setChangeListener([&](const BackendChanges& c) { seen.push_back(c); });
    ASSERT_TRUE(runUntil([] { return true; }, 200ms)); // the subscription is set up
    std::this_thread::sleep_for(100ms);
    fake->emitDaemon("InstalledChanged");
    fake->emitDaemon("UpdatesChanged");
    fake->emitDaemon("InstalledChanged");
    ASSERT_TRUE(runUntil([&] { return !seen.empty(); }, 3s));
    runUntil([] { return false; }, 400ms);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_TRUE(seen[0].installed);
    EXPECT_TRUE(seen[0].updates);
    EXPECT_FALSE(seen[0].sources);
}

// --- Other programs' transactions (H.2, SERVICE.md §22) ---------------------------------
//
// Cutefish Software Update (cutefish-core, org.cutefish.Services.SoftwareUpdate)
// drives packagekitd itself: check() is RefreshCache then GetUpdates,
// update() is UpdatePackages with only-trusted. The fake plays that other
// program with startExternal(); nothing here talks to Cutefish's service.

namespace {

class PkCoexistenceTest : public PkBackendTest
{
protected:
    void SetUp() override
    {
        PkBackendTest::SetUp();
        backend->setChangeListener([this](const BackendChanges& c) {
            if (c.activity)
                ++activityChanges;
        });
    }

    bool activityIs(ExternalActivity::Kind kind)
    {
        return runUntil([&] { return backend->externalActivity().kind == kind; }, 5s);
    }

    Answer held(std::vector<Step> steps = {})
    {
        Answer a;
        a.hold = true;
        a.steps = std::move(steps);
        return a;
    }

    std::optional<Result<Plan>> planGimp(const std::string& id = "tx-1")
    {
        return wait<Plan>([&](auto d) { backend->plan(id, request(TransactionKind::Install, {"gimp"}), std::move(d)); });
    }

    int storeTransactions()
    {
        int n = 0;
        for (const auto& c : fake->calls())
            n += c.method == "Resolve" || c.method == "InstallPackages" || c.method == "RefreshCache";
        return n;
    }

    int activityChanges = 0;
};

void expectLocked(const Error& e)
{
    EXPECT_EQ(e.code(), ErrorCode::Backend);
    EXPECT_EQ(e.backendFailure(), BackendFailure::Locked) << e.message();
    EXPECT_NE(e.message().find("update-packages"), std::string::npos) << e.message();
    EXPECT_NE(e.message().find("try again"), std::string::npos) << e.message();
}

} // namespace

TEST_F(PkCoexistenceTest, IdlePackageKitBlocksNothingAndTheStoresOwnTransactionsDoNotCount)
{
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    Answer real;
    real.steps = {{pk::Status::Download, 30, true}, {pk::Status::Install, 80, false}};
    fake->answer("InstallPackages", real, false);
    EXPECT_EQ(backend->externalActivity().kind, ExternalActivity::Kind::None);
    auto plan = planGimp();
    ASSERT_TRUE(plan && *plan) << plan->error().message();
    const PlanRequest req = request(TransactionKind::Install, {"gimp"});
    auto r = wait<ExecutionResult>([&](auto d) { backend->execute("tx-1", req, **plan, {}, std::move(d)); });
    ASSERT_TRUE(r && *r) << r->error().message();
    runUntil([] { return false; }, 200ms); // observation window for a wrong notification
    EXPECT_EQ(activityChanges, 0);
    EXPECT_EQ(backend->externalActivity().kind, ExternalActivity::Kind::None);
}

TEST_F(PkCoexistenceTest, ACheckForUpdatesByAnotherProgramBlocksNothing)
{
    const std::string path = fake->startExternal("GetUpdates", held(), pk::filter::kNone);
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::Checking));
    EXPECT_EQ(backend->externalActivity().role, "get-updates");
    EXPECT_EQ(activityChanges, 1);
    // The store's request goes to PackageKit (which queues it as it likes):
    // the fake runs it at once.
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    auto plan = planGimp();
    ASSERT_TRUE(plan && *plan) << plan->error().message();
    ASSERT_TRUE(fake->finish(path));
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::None));
    EXPECT_EQ(fake->cancels(), 0);
}

TEST_F(PkCoexistenceTest, ACacheRefreshByAnotherProgramBlocksNothing)
{
    const std::string path = fake->startExternal("RefreshCache", held({{pk::Status::RefreshCache, 40, true}}), 0);
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::Refreshing));
    EXPECT_EQ(backend->externalActivity().role, "refresh-cache");
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    auto plan = planGimp();
    ASSERT_TRUE(plan && *plan) << plan->error().message();
    ASSERT_TRUE(fake->finish(path));
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::None));
}

TEST_F(PkCoexistenceTest, ASystemUpdateFromSoftwareUpdateBlocksChangesUntilItEnds)
{
    // A plan made before, while PackageKit was idle.
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    auto accepted = planGimp("tx-0");
    ASSERT_TRUE(accepted && *accepted);
    const std::string path = fake->startExternal(
            "UpdatePackages", held({{pk::Status::Download, 20, true}, {pk::Status::Install, 60, false}}));
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::Changing));
    EXPECT_EQ(backend->externalActivity().role, "update-packages");
    EXPECT_EQ(activityChanges, 1);
    const int before = storeTransactions();

    // Plan, execute and refresh are refused at once, with a temporary
    // failure; nothing is sent to PackageKit.
    auto plan = planGimp();
    ASSERT_TRUE(plan && !*plan);
    expectLocked(plan->error());
    const PlanRequest req = request(TransactionKind::Install, {"gimp"});
    auto exec = wait<ExecutionResult>([&](auto d) { backend->execute("tx-2", req, **accepted, {}, std::move(d)); });
    ASSERT_TRUE(exec && !*exec);
    expectLocked(exec->error());
    auto refresh = wait<void>([&](auto d) { backend->refresh("tx-3", {}, std::move(d)); });
    ASSERT_TRUE(refresh && !*refresh);
    expectLocked(refresh->error());
    EXPECT_EQ(storeTransactions(), before);
    // Queries still go (inventory, updates).
    auto installed = wait<std::vector<InstalledItem>>([&](auto d) { backend->queryInstalled(std::move(d)); });
    ASSERT_TRUE(installed && *installed);

    // Its steps are followed without new notifications.
    fake->setStatus(path, pk::Status::Commit);
    ASSERT_TRUE(runUntil([&] { return backend->externalActivity().status == "commit"; }, 5s));
    EXPECT_EQ(activityChanges, 1);

    // It ends: the store works again (the user retries).
    ASSERT_TRUE(fake->finish(path));
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::None));
    EXPECT_EQ(activityChanges, 2);
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    auto again = planGimp("tx-4");
    ASSERT_TRUE(again && *again) << again->error().message();
    // The store never asked to cancel the other program's update.
    EXPECT_EQ(fake->cancels(), 0);
}

TEST_F(PkCoexistenceTest, AnUpdateAlreadyRunningAtStartupIsSeen)
{
    const std::string path = fake->startExternal("UpdatePackages", held());
    // A new backend, as a service started while Settings updates.
    backend = std::make_unique<PackageKitBackend>(
            loop, PackageKitBackend::Options{.bus = PkTransport::Bus::Session, .swcatalog = bus.dir() / "swcatalog"});
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::Changing));
    auto plan = planGimp();
    ASSERT_TRUE(plan && !*plan);
    expectLocked(plan->error());
    ASSERT_TRUE(fake->finish(path));
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::None));
}

TEST_F(PkCoexistenceTest, AnExternalUpdateCancelledOrFailedFreesTheBackend)
{
    std::string path = fake->startExternal("UpdatePackages", held());
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::Changing));
    // Cancelled from Settings (its cancel(), PackageKit's Cancel): the end.
    ASSERT_TRUE(fake->finish(path, pk::Exit::Cancelled, fakepk::errorCode("transaction-cancelled")));
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::None));
    path = fake->startExternal("InstallPackages", held());
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::Changing));
    EXPECT_EQ(backend->externalActivity().role, "install-packages");
    // A PackageKit error ends it too.
    ASSERT_TRUE(fake->finish(path, pk::Exit::Failed, fakepk::errorCode("package-failed-to-install")));
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::None));
    EXPECT_EQ(activityChanges, 4);
    EXPECT_EQ(fake->cancels(), 0); // never the store's doing
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    auto plan = planGimp();
    ASSERT_TRUE(plan && *plan);
}

TEST_F(PkCoexistenceTest, PackageKitGoingAwayEndsTheActivity)
{
    fake->startExternal("UpdatePackages", held());
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::Changing));
    fake->stop();
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::None));
    auto r = wait<void>([&](Completion<void> d) { backend->availability(std::move(d)); });
    ASSERT_TRUE(r && !*r);
    EXPECT_EQ(r->error().backendFailure(), BackendFailure::Unavailable);
    // It comes back idle: usable again.
    fake->start();
    auto back = wait<void>([&](Completion<void> d) { backend->availability(std::move(d)); });
    ASSERT_TRUE(back && *back) << back->error().message();
    EXPECT_EQ(backend->externalActivity().kind, ExternalActivity::Kind::None);
}

TEST_F(PkCoexistenceTest, AnUpdateStartedDuringTheSimulationStopsTheExecutionBeforeAnyChange)
{
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    auto plan = planGimp();
    ASSERT_TRUE(plan && *plan);
    // The new simulation of the execution stays running until released.
    Answer sim = gimpInstallPlan();
    sim.hold = true;
    fake->replace("InstallPackages", sim, true);
    std::optional<Result<ExecutionResult>> r;
    const PlanRequest req = request(TransactionKind::Install, {"gimp"});
    backend->execute("tx-1", req, **plan, {}, [&](Result<ExecutionResult> x) { r.emplace(std::move(x)); });
    std::string simPath;
    ASSERT_TRUE(runUntil([&] {
        int simulations = 0;
        for (const auto& c : fake->calls())
            simulations += c.method == "InstallPackages" && c.simulate();
        const auto running = fake->runningTransactions();
        if (simulations >= 2 && running.size() == 1)
            simPath = running.front();
        return !simPath.empty();
    }));
    // Settings starts the system update meanwhile.
    const std::string update = fake->startExternal("UpdatePackages", held());
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::Changing));
    ASSERT_TRUE(fake->finish(simPath));
    ASSERT_TRUE(runUntil([&] { return r.has_value(); }));
    ASSERT_FALSE(*r);
    expectLocked(r->error());
    EXPECT_TRUE(realCalls("InstallPackages").empty()); // the real transaction never started
    ASSERT_TRUE(fake->finish(update));
}

TEST_F(PkCoexistenceTest, AnUpdateStartedWhileTheStoreInstallsLeavesTheStoresTransactionAlone)
{
    // The reverse scenario: the store's install runs, then Settings starts
    // the system update. packagekitd queues it (one transaction at a time);
    // the store neither cancels it nor is cancelled, and refuses a new
    // change until it is over.
    fake->answer("InstallPackages", gimpInstallPlan(), true);
    Answer real;
    real.hold = true;
    real.steps = {{pk::Status::Install, 50, false}};
    fake->answer("InstallPackages", real, false);
    auto plan = planGimp();
    ASSERT_TRUE(plan && *plan);
    std::optional<Result<ExecutionResult>> r;
    const PlanRequest req = request(TransactionKind::Install, {"gimp"});
    std::string own;
    OperationEvents ev;
    ev.handle = [&](const std::string& h) { own = h; };
    backend->execute("tx-1", req, **plan, ev, [&](Result<ExecutionResult> x) { r.emplace(std::move(x)); });
    ASSERT_TRUE(runUntil([&] { return !own.empty(); }));
    EXPECT_EQ(backend->externalActivity().kind, ExternalActivity::Kind::None); // its own: not counted
    const std::string update = fake->startExternal("UpdatePackages", held({{pk::Status::Wait, 101, true}}));
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::Changing));
    auto second = planGimp("tx-2");
    ASSERT_TRUE(second && !*second);
    expectLocked(second->error());
    // The store's transaction ends normally.
    ASSERT_TRUE(fake->finish(own));
    ASSERT_TRUE(runUntil([&] { return r.has_value(); }));
    ASSERT_TRUE(*r) << r->error().message();
    EXPECT_EQ(fake->cancels(), 0);
    ASSERT_TRUE(fake->finish(update));
    ASSERT_TRUE(activityIs(ExternalActivity::Kind::None));
}
