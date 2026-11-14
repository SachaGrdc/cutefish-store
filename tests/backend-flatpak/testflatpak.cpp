// Tests of the Flatpak backend: the pure translation logic, and the Backend
// driven by a deterministic double of FlatpakStore (fakestore.h). Nothing
// here touches libflatpak or a real installation: the real store
// (libflatpakstore.h) is compiled but not exercised by these tests.

#include "flatpakbackend.h"
#include "translate.h"
#include "fakestore.h"

#include <cutefish/store/confirmation.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cutefish::store;
using namespace cutefish::store::flatpak;
using namespace std::chrono_literals;

namespace {

FlatpakRef appRef(const char* name)
{
    return {RefKind::App, name, "x86_64", "stable"};
}

// --- Pure logic ---------------------------------------------------------------

TEST(FlatpakTranslate, ParsesApplicationAndRuntimeRefs)
{
    auto app = parseRef("app/org.gimp.GIMP/x86_64/stable");
    ASSERT_TRUE(app);
    EXPECT_EQ(app->kind, RefKind::App);
    EXPECT_EQ(app->name, "org.gimp.GIMP");
    EXPECT_EQ(app->arch, "x86_64");
    EXPECT_EQ(app->branch, "stable");
    EXPECT_EQ(app->full(), "app/org.gimp.GIMP/x86_64/stable");

    auto runtime = parseRef("runtime/org.freedesktop.Platform/x86_64/24.08");
    ASSERT_TRUE(runtime);
    EXPECT_EQ(runtime->kind, RefKind::Runtime);

    EXPECT_FALSE(parseRef("app/org.gimp.GIMP/x86_64"));     // three parts
    EXPECT_FALSE(parseRef("bad/org.gimp.GIMP/x86_64/stable")); // unknown kind
    EXPECT_FALSE(parseRef("app//x86_64/stable"));           // empty id
}

TEST(FlatpakTranslate, ClassifiesRemotesByUrlHost)
{
    EXPECT_EQ(remoteKind("flathub", "https://dl.flathub.org/repo/"), SourceKind::Flathub);
    EXPECT_EQ(remoteKind("flathub", "https://flathub.org/repo/"), SourceKind::Flathub);
    EXPECT_EQ(remoteKind("third-party", "https://example.org/repo/"), SourceKind::ThirdParty);
}

TEST(FlatpakTranslate, MapsFlatpakAndGioErrors)
{
    // FLATPAK_ERROR_OUT_OF_SPACE = 19
    EXPECT_EQ(mapFlatpakError("flatpak", 19, "disk full").backendFailure(), BackendFailure::NoSpace);
    // FLATPAK_ERROR_UNTRUSTED = 13
    EXPECT_EQ(mapFlatpakError("flatpak", 13, "unsigned").backendFailure(), BackendFailure::Untrusted);
    // FLATPAK_ERROR_ABORTED = 4
    EXPECT_EQ(mapFlatpakError("flatpak", 4, "aborted").code(), ErrorCode::Cancelled);
    // FLATPAK_ERROR_PERMISSION_DENIED = 23
    EXPECT_EQ(mapFlatpakError("flatpak", 23, "denied").backendFailure(), BackendFailure::AuthDenied);
    // G_IO_ERROR_CANCELLED = 19
    EXPECT_EQ(mapFlatpakError("g-io", 19, "cancelled").code(), ErrorCode::Cancelled);
    // G_IO_ERROR_HOST_NOT_FOUND = 26
    EXPECT_EQ(mapFlatpakError("g-io", 26, "host").backendFailure(), BackendFailure::Network);
}

TEST(FlatpakTranslate, NormalisesPermissions)
{
    EXPECT_EQ(normalisePermissions({"socket=x11", "shared=network", "socket=x11"}),
              (std::vector<std::string>{"shared=network", "socket=x11"}));
}

TEST(FlatpakTranslate, BuildsAPlanFromOperations)
{
    std::vector<FlatpakOp> ops;
    FlatpakOp app{.type = OperationType::Install,
                  .ref = FlatpakRef{RefKind::App, "org.gimp.GIMP", "x86_64", "stable"},
                  .remote = "flathub",
                  .commit = "commit-1",
                  .downloadSize = 1000,
                  .installedSize = 0,
                  .permissions = {"shared=network"},
                  .oldPermissions = {},
                  .requested = true,
                  .addsSource = false,
                  .rebaseFrom = {}};
    FlatpakOp runtime{.type = OperationType::Install,
                      .ref = FlatpakRef{RefKind::Runtime, "org.freedesktop.Platform", "x86_64", "24.08"},
                      .remote = "flathub",
                      .commit = {},
                      .downloadSize = 500,
                      .installedSize = 0,
                      .permissions = {},
                      .oldPermissions = {},
                      .requested = false,
                      .addsSource = false,
                      .rebaseFrom = {}};
    ops.push_back(app);
    ops.push_back(runtime);

    const std::vector<std::string> requested{"app/org.gimp.GIMP/x86_64/stable"};
    auto plan = buildPlan(TransactionKind::Install, Scope::User, ops, requested);
    ASSERT_TRUE(plan) << plan.error().message();
    EXPECT_EQ(plan->kind(), TransactionKind::Install);
    EXPECT_EQ(plan->backend(), BackendKind::Flatpak);
    EXPECT_EQ(plan->scope(), Scope::User);
    ASSERT_EQ(plan->items().size(), 2u);
    EXPECT_TRUE(plan->items()[0].requested());
    EXPECT_EQ(plan->items()[0].kind(), PlanItemKind::Application);
    EXPECT_FALSE(plan->items()[1].requested());
    EXPECT_EQ(plan->items()[1].kind(), PlanItemKind::Runtime);
    EXPECT_EQ(plan->items()[0].permissions().size(), 1u);
    EXPECT_EQ(plan->items()[0].permissions()[0], "shared=network");
}

// --- Backend with a fake store ------------------------------------------------

struct BackendFixture : public ::testing::Test
{
    BackendFixture()
        : store(std::make_unique<FakeStore>()), fake(static_cast<FakeStore*>(store.get())),
          backend({.post = [](std::function<void()> task) { task(); }}, std::move(store))
    {
    }

    template <typename T>
    Result<T> await(std::function<void(Completion<T>)> start)
    {
        std::promise<Result<T>> promise;
        auto future = promise.get_future();
        start([&promise](Result<T> result) { promise.set_value(std::move(result)); });
        return future.get();
    }

    OfferId installOffer() const
    {
        return OfferId::parse("flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable").value();
    }

    std::unique_ptr<FlatpakStore> store;
    FakeStore* fake;
    FlatpakBackend backend;
};

TEST_F(BackendFixture, ReportsBothInstallationsAsAvailable)
{
    auto result = await<void>([this](Completion<void> done) { backend.availability(std::move(done)); });
    EXPECT_TRUE(result) << result.error().message();
}

TEST_F(BackendFixture, ReportsNoBackendWhenNoInstallationOpens)
{
    fake->scopes_ = {};
    auto result = await<void>([this](Completion<void> done) { backend.availability(std::move(done)); });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().backendFailure(), BackendFailure::Unavailable);
}

TEST_F(BackendFixture, ListsInstalledApplicationsAndRuntimes)
{
    fake->installed_[Scope::User] = {
            {FlatpakRef{RefKind::App, "org.gimp.GIMP", "x86_64", "stable"}, "flathub"},
            {FlatpakRef{RefKind::Runtime, "org.freedesktop.Platform", "x86_64", "24.08"}, "flathub"}};
    auto result = await<std::vector<InstalledItem>>(
            [this](Completion<std::vector<InstalledItem>> done) { backend.queryInstalled(std::move(done)); });
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 2u);
    EXPECT_EQ(result->at(0).offer().str(), "flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable");
    EXPECT_EQ(result->at(0).backend(), BackendKind::Flatpak);
    EXPECT_EQ(result->at(0).scope(), Scope::User);
    EXPECT_EQ(result->at(1).offer().str(), "flatpak:user:flathub:runtime/org.freedesktop.Platform/x86_64/24.08");
}

TEST_F(BackendFixture, SeparatesApplicationAndRuntimeUpdates)
{
    fake->updates_[Scope::User] = {
            {FlatpakRef{RefKind::App, "org.gimp.GIMP", "x86_64", "stable"}, "flathub", "commit-2"},
            {FlatpakRef{RefKind::Runtime, "org.freedesktop.Platform", "x86_64", "24.08"}, "flathub", "commit-2"}};
    auto result = await<std::vector<AvailableUpdate>>(
            [this](Completion<std::vector<AvailableUpdate>> done) { backend.queryUpdates(std::move(done)); });
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 2u);
    EXPECT_EQ(result->at(0).kind, PlanItemKind::Application);
    EXPECT_EQ(result->at(1).kind, PlanItemKind::Runtime);
}

TEST_F(BackendFixture, ReturnsPermissionsFromMetadata)
{
    fake->metadata_.permissions = {"shared=network", "socket=x11"};
    auto offer = installOffer();
    auto result = await<std::vector<std::string>>(
            [&](Completion<std::vector<std::string>> done) { backend.permissions(offer, std::move(done)); });
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, (std::vector<std::string>{"shared=network", "socket=x11"}));
}

TEST_F(BackendFixture, ListsSourcesAndCatalogSources)
{
    fake->remotes_[Scope::User] = {
            {.name = "flathub", .url = "https://dl.flathub.org/repo/", .metadataPath = "/cache/flathub/appstream.xml.gz",
             .iconsPath = "/cache/flathub/icons"},
            {.name = "third-party", .url = "https://example.org/repo/", .disabled = false, .gpgVerify = true,
             .metadataPath = {}, .iconsPath = {}}};
    fake->installed_[Scope::User] = {{appRef("org.gimp.GIMP"), "flathub"},
                                     {{RefKind::Runtime, "org.gnome.Platform", "x86_64", "48"}, "flathub"}};
    auto sources = await<std::vector<BackendSource>>(
            [this](Completion<std::vector<BackendSource>> done) { backend.sources(std::move(done)); });
    ASSERT_TRUE(sources);
    ASSERT_EQ(sources->size(), 2u);
    EXPECT_EQ(sources->at(0).source.kind(), SourceKind::Flathub);
    EXPECT_EQ(sources->at(0).source.id().str(), "flatpak:user:flathub");
    EXPECT_EQ(sources->at(0).verifiesSignatures, std::optional<bool>(true));
    EXPECT_EQ(sources->at(0).installedItems, std::optional<std::uint32_t>(2)); // the runtime counts
    EXPECT_TRUE(sources->at(0).catalogueSources.empty());
    EXPECT_EQ(sources->at(1).source.kind(), SourceKind::ThirdParty);
    EXPECT_EQ(sources->at(1).installedItems, std::optional<std::uint32_t>(0));

    auto catalog = await<std::vector<SourceMetadata>>(
            [this](Completion<std::vector<SourceMetadata>> done) { backend.catalogSources(std::move(done)); });
    ASSERT_TRUE(catalog);
    ASSERT_EQ(catalog->size(), 1u); // only the remote with metadata
    EXPECT_EQ(catalog->at(0).files.metadata, "/cache/flathub/appstream.xml.gz");
}

// --- Source changes (H.5) -------------------------------------------------------

namespace {

SourceId sourceId(const char* id)
{
    return SourceId::parse(id).value();
}

} // namespace

TEST(FlatpakTranslate, SourceIdsNameARemoteOfAnInstallation)
{
    auto user = remoteOfSource(sourceId("flatpak:user:flathub"));
    ASSERT_TRUE(user);
    EXPECT_EQ(user->first, Scope::User);
    EXPECT_EQ(user->second, "flathub");
    auto system = remoteOfSource(sourceId("flatpak:system:fedora"));
    ASSERT_TRUE(system);
    EXPECT_EQ(system->first, Scope::System);
    for (const char* bad : {"packagekit:debian-trixie-main", "flatpak:user", "flatpak:user:", "flatpak:nowhere:x",
                            "flatpak:user:a:b"})
        EXPECT_FALSE(remoteOfSource(sourceId(bad))) << bad;
}

struct SourceFixture : BackendFixture
{
    SourceFixture()
    {
        fake->remotes_[Scope::User] = {
                {.name = "flathub", .url = "https://dl.flathub.org/repo/", .disabled = false, .gpgVerify = true, .metadataPath = {}, .iconsPath = {}},
                {.name = "unsigned", .url = "https://example.org/repo/", .disabled = true, .gpgVerify = false, .metadataPath = {}, .iconsPath = {}},
                {.name = "spare", .url = "https://example.net/repo/", .disabled = true, .gpgVerify = true, .metadataPath = {}, .iconsPath = {}}};
        fake->remotes_[Scope::System] = {
                {.name = "flathub", .url = "https://dl.flathub.org/repo/", .disabled = false, .gpgVerify = true, .metadataPath = {}, .iconsPath = {}}};
        fake->installed_[Scope::User] = {{appRef("org.gimp.GIMP"), "flathub"}};
    }

    Result<void> change(const char* id, SourceRequest::Action action)
    {
        return await<void>([&](Completion<void> done) {
            backend.changeSource("op", {.source = sourceId(id), .action = action}, std::move(done));
        });
    }

    const RemoteInfo* remote(Scope scope, const std::string& name) const
    {
        for (const RemoteInfo& r : fake->remotes_[scope])
            if (r.name == name)
                return &r;
        return nullptr;
    }
};

TEST_F(SourceFixture, EnablesAndDisablesAUserRemote)
{
    EXPECT_TRUE(backend.capabilities().userSourceChanges);
    ASSERT_TRUE(change("flatpak:user:spare", SourceRequest::Action::Enable));
    EXPECT_FALSE(remote(Scope::User, "spare")->disabled);
    // Already enabled: success, nothing changes.
    ASSERT_TRUE(change("flatpak:user:spare", SourceRequest::Action::Enable));
    ASSERT_TRUE(change("flatpak:user:spare", SourceRequest::Action::Disable));
    EXPECT_TRUE(remote(Scope::User, "spare")->disabled);
    ASSERT_TRUE(change("flatpak:user:spare", SourceRequest::Action::Disable));
    EXPECT_TRUE(remote(Scope::User, "spare")->disabled);
    // Disabling a remote something came from is allowed: nothing is removed.
    ASSERT_TRUE(change("flatpak:user:flathub", SourceRequest::Action::Disable));
    EXPECT_TRUE(remote(Scope::User, "flathub")->disabled);
    EXPECT_EQ(fake->installed_[Scope::User].size(), 1u);
}

TEST_F(SourceFixture, NeverEnablesARemoteWithoutSignatures)
{
    auto r = change("flatpak:user:unsigned", SourceRequest::Action::Enable);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::Backend);
    EXPECT_EQ(r.error().backendFailure(), BackendFailure::Untrusted);
    EXPECT_TRUE(remote(Scope::User, "unsigned")->disabled);
    // Disabling or removing it stays possible.
    EXPECT_TRUE(change("flatpak:user:unsigned", SourceRequest::Action::Remove));
    EXPECT_EQ(remote(Scope::User, "unsigned"), nullptr);
}

TEST_F(SourceFixture, RemovesOnlyAnUnusedUserRemote)
{
    auto used = change("flatpak:user:flathub", SourceRequest::Action::Remove);
    ASSERT_FALSE(used);
    EXPECT_EQ(used.error().code(), ErrorCode::NotAllowed);
    ASSERT_NE(remote(Scope::User, "flathub"), nullptr); // nothing removed, nothing uninstalled
    EXPECT_EQ(fake->installed_[Scope::User].size(), 1u);

    ASSERT_TRUE(change("flatpak:user:spare", SourceRequest::Action::Remove));
    EXPECT_EQ(remote(Scope::User, "spare"), nullptr);
}

TEST_F(SourceFixture, NeverChangesASystemRemote)
{
    for (const auto action : {SourceRequest::Action::Enable, SourceRequest::Action::Disable, SourceRequest::Action::Remove}) {
        auto r = change("flatpak:system:flathub", action);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code(), ErrorCode::NotAllowed);
    }
    ASSERT_NE(remote(Scope::System, "flathub"), nullptr);
    EXPECT_FALSE(remote(Scope::System, "flathub")->disabled);
}

TEST_F(SourceFixture, RefusesAnUnknownOrForeignSource)
{
    auto unknown = change("flatpak:user:nowhere", SourceRequest::Action::Disable);
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code(), ErrorCode::NotFound);
    auto foreign = change("packagekit:debian-trixie-main", SourceRequest::Action::Disable);
    ASSERT_FALSE(foreign);
    EXPECT_EQ(foreign.error().code(), ErrorCode::InvalidArgument);
    // Without a user installation, the backend cannot change its remotes.
    fake->scopes_ = {Scope::System};
    auto gone = change("flatpak:user:spare", SourceRequest::Action::Enable);
    ASSERT_FALSE(gone);
    EXPECT_EQ(gone.error().backendFailure(), BackendFailure::Unavailable);
}

TEST_F(BackendFixture, PlansAnInstallWithItsRuntime)
{
    auto plan = await<Plan>([this](Completion<Plan> done) {
        backend.plan("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), std::move(done));
    });
    ASSERT_TRUE(plan) << plan.error().message();
    ASSERT_EQ(plan->items().size(), 2u);
    EXPECT_TRUE(plan->items()[0].requested());
    EXPECT_FALSE(plan->items()[1].requested());
    EXPECT_EQ(plan->items()[1].kind(), PlanItemKind::Runtime);
}

TEST_F(BackendFixture, PlansARemove)
{
    auto plan = await<Plan>([this](Completion<Plan> done) {
        backend.plan("op", PlanRequest::create(TransactionKind::Remove, {installOffer()}).value(), std::move(done));
    });
    ASSERT_TRUE(plan) << plan.error().message();
    ASSERT_EQ(plan->items().size(), 1u);
    EXPECT_EQ(plan->items()[0].operation(), PlanOperation::Remove);
    EXPECT_TRUE(plan->items()[0].requested());
}

TEST_F(BackendFixture, PlansAnUpdateWithItsRuntime)
{
    auto plan = await<Plan>([this](Completion<Plan> done) {
        backend.plan("op", PlanRequest::create(TransactionKind::Update, {installOffer()}).value(), std::move(done));
    });
    ASSERT_TRUE(plan) << plan.error().message();
    ASSERT_EQ(plan->items().size(), 2u);
    EXPECT_EQ(plan->items()[0].operation(), PlanOperation::Update);
    EXPECT_TRUE(plan->items()[0].requested());
    EXPECT_EQ(plan->items()[1].kind(), PlanItemKind::Runtime);
}

TEST_F(BackendFixture, ExecutesACompatiblePlan)
{
    auto accepted = await<Plan>([this](Completion<Plan> done) {
        backend.plan("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), std::move(done));
    });
    ASSERT_TRUE(accepted);

    std::optional<Plan> started;
    std::vector<Progress> progress;
    OperationEvents events;
    events.started = [&](const Plan& plan) { started = plan; };
    events.progress = [&](const Progress& p) { progress.push_back(p); };

    auto result = await<ExecutionResult>([&](Completion<ExecutionResult> done) {
        backend.execute("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), *accepted,
                        std::move(events), std::move(done));
    });
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result->outcome, ExecutionResult::Outcome::Completed);
    ASSERT_TRUE(started) << "started was not emitted before running";
    EXPECT_EQ(started->items().size(), 2u);
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(fake->executes.load(), 1);
}

TEST_F(BackendFixture, AChangedPlanIsNeverExecuted)
{
    auto accepted = await<Plan>([this](Completion<Plan> done) {
        backend.plan("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), std::move(done));
    });
    ASSERT_TRUE(accepted);

    fake->planChanged = true;
    bool started = false;
    OperationEvents events;
    events.started = [&](const Plan&) { started = true; };

    auto result = await<ExecutionResult>([&](Completion<ExecutionResult> done) {
        backend.execute("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), *accepted,
                        std::move(events), std::move(done));
    });
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result->outcome, ExecutionResult::Outcome::PlanChanged);
    ASSERT_TRUE(result->freshPlan) << "a changed plan carries the fresh plan";
    EXPECT_FALSE(started); // nothing ran
    EXPECT_EQ(fake->executes.load(), 1);
}

TEST_F(BackendFixture, MapsExecutionErrors)
{
    auto accepted = await<Plan>([this](Completion<Plan> done) {
        backend.plan("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), std::move(done));
    });
    ASSERT_TRUE(accepted);

    fake->runError = Error::backend(BackendFailure::AuthDenied, "denied by policy");
    auto result = await<ExecutionResult>([&](Completion<ExecutionResult> done) {
        backend.execute("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), *accepted,
                        OperationEvents{}, std::move(done));
    });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().backendFailure(), BackendFailure::AuthDenied);
}

TEST_F(BackendFixture, CancelsARunningExecution)
{
    auto accepted = await<Plan>([this](Completion<Plan> done) {
        backend.plan("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), std::move(done));
    });
    ASSERT_TRUE(accepted);

    std::promise<Result<ExecutionResult>> promise;
    auto future = promise.get_future();
    backend.execute("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), *accepted,
                    OperationEvents{}, [&promise](Result<ExecutionResult> r) { promise.set_value(std::move(r)); });

    // Wait until the worker is inside the run, then cancel.
    for (int i = 0; i < 500 && !fake->executing.load(); ++i)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(fake->executing.load());
    EXPECT_TRUE(backend.cancel("op"));
    auto result = future.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(fake->cancels.load(), 1);
}

TEST_F(BackendFixture, RefusesATransactionThatWouldAddARemote)
{
    fake->remoteAddAttempted = true;
    auto result = await<Plan>([this](Completion<Plan> done) {
        backend.plan("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), std::move(done));
    });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::NotAllowed);
    EXPECT_NE(result.error().message().find("explicit consent"), std::string::npos);
}

TEST_F(BackendFixture, RefusesARemoteAddDuringExecution)
{
    auto accepted = await<Plan>([this](Completion<Plan> done) {
        backend.plan("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), std::move(done));
    });
    ASSERT_TRUE(accepted);

    fake->remoteAddAttempted = true;
    bool started = false;
    OperationEvents events;
    events.started = [&](const Plan&) { started = true; };
    auto result = await<ExecutionResult>([&](Completion<ExecutionResult> done) {
        backend.execute("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), *accepted,
                        std::move(events), std::move(done));
    });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::NotAllowed);
    EXPECT_FALSE(started); // nothing deployed
}

TEST(FlatpakStore, AddsAndRemovesARemoteOnlyExplicitly)
{
    FakeStore store;
    EXPECT_TRUE(store.addRemote(Scope::User, "flathub", "https://dl.flathub.org/repo/"));
    auto remotes = store.remotes(Scope::User);
    ASSERT_TRUE(remotes);
    ASSERT_EQ(remotes->size(), 1u);
    EXPECT_EQ(remotes->at(0).name, "flathub");

    EXPECT_TRUE(store.setRemoteDisabled(Scope::User, "flathub", true));
    EXPECT_TRUE(store.remotes(Scope::User)->at(0).disabled);

    EXPECT_TRUE(store.removeRemote(Scope::User, "flathub"));
    EXPECT_TRUE(store.remotes(Scope::User)->empty());
}

TEST(FlatpakTranslate, ANewPermissionChangesThePlan)
{
    // An install whose second pass adds a permission must not run without a
    // new confirmation (P-008): mayExecute() is the backend's check.
    std::vector<FlatpakOp> acceptedOps;
    FlatpakOp op{.type = OperationType::Install,
                 .ref = FlatpakRef{RefKind::App, "org.gimp.GIMP", "x86_64", "stable"},
                 .remote = "flathub",
                 .commit = "commit-1",
                 .downloadSize = 1000,
                 .installedSize = 0,
                 .permissions = {"shared=network"},
                 .oldPermissions = {},
                 .requested = true,
                 .addsSource = false,
                 .rebaseFrom = {}};
    acceptedOps.push_back(op);

    std::vector<FlatpakOp> freshOps = acceptedOps;
    freshOps[0].permissions = {"shared=network", "socket=x11"}; // a permission appears

    const std::vector<std::string> requested{"app/org.gimp.GIMP/x86_64/stable"};
    auto accepted = buildPlan(TransactionKind::Install, Scope::User, acceptedOps, requested);
    auto fresh = buildPlan(TransactionKind::Install, Scope::User, freshOps, requested);
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(fresh);
    EXPECT_FALSE(mayExecute(*accepted, *fresh));
}

TEST(FlatpakTranslate, ASizeOnlyChangeStillRuns)
{
    std::vector<FlatpakOp> acceptedOps;
    FlatpakOp op{.type = OperationType::Install,
                 .ref = FlatpakRef{RefKind::App, "org.gimp.GIMP", "x86_64", "stable"},
                 .remote = "flathub",
                 .commit = "commit-1",
                 .downloadSize = 1000,
                 .installedSize = 0,
                 .permissions = {"shared=network"},
                 .oldPermissions = {},
                 .requested = true,
                 .addsSource = false,
                 .rebaseFrom = {}};
    acceptedOps.push_back(op);

    std::vector<FlatpakOp> freshOps = acceptedOps;
    freshOps[0].downloadSize = 2000; // a size estimate only: compatible

    const std::vector<std::string> requested{"app/org.gimp.GIMP/x86_64/stable"};
    auto accepted = buildPlan(TransactionKind::Install, Scope::User, acceptedOps, requested);
    auto fresh = buildPlan(TransactionKind::Install, Scope::User, freshOps, requested);
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(fresh);
    EXPECT_TRUE(mayExecute(*accepted, *fresh));
}

TEST_F(BackendFixture, AnErrorDuringTheSecondPassIsReported)
{
    auto accepted = await<Plan>([this](Completion<Plan> done) {
        backend.plan("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), std::move(done));
    });
    ASSERT_TRUE(accepted);

    fake->executeError = Error::backend(BackendFailure::Dependency, "the runtime is not available");
    bool started = false;
    OperationEvents events;
    events.started = [&](const Plan&) { started = true; };
    auto result = await<ExecutionResult>([&](Completion<ExecutionResult> done) {
        backend.execute("op", PlanRequest::create(TransactionKind::Install, {installOffer()}).value(), *accepted,
                        std::move(events), std::move(done));
    });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().backendFailure(), BackendFailure::Dependency);
    EXPECT_FALSE(started);
}

TEST_F(BackendFixture, DisabledRemotesAreNotIndexed)
{
    fake->remotes_[Scope::User] = {
            {.name = "flathub", .url = "https://dl.flathub.org/repo/", .disabled = false, .gpgVerify = true,
             .metadataPath = "/cache/flathub/appstream.xml.gz", .iconsPath = "/cache/flathub/icons"},
            {.name = "old", .url = "https://example.org/repo/", .disabled = true, .gpgVerify = true,
             .metadataPath = "/cache/old/appstream.xml.gz", .iconsPath = "/cache/old/icons"}};
    auto catalog = await<std::vector<SourceMetadata>>(
            [this](Completion<std::vector<SourceMetadata>> done) { backend.catalogSources(std::move(done)); });
    ASSERT_TRUE(catalog);
    ASSERT_EQ(catalog->size(), 1u); // the disabled remote is left out
    EXPECT_EQ(catalog->at(0).source.origin(), "flathub");
}

} // namespace
