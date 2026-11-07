// The extended Backend contract (step D), exercised with scriptable fake
// backends and a small driver that runs the Transaction state machine the
// way the service will (SERVICE.md §5.2).

#include "fakes.h"
#include "planfixtures.h"

#include <cutefish/store/backend.h>
#include <cutefish/store/transaction.h>

#include <gtest/gtest.h>

using namespace cutefish::store;
using namespace planfixtures;
using fakes::FakeBackend;
using fakes::PostQueue;

namespace {

using S = TransactionState;
using R = ConfirmationReason;

const std::string kUserApp = "flatpak:user:flathub:" + std::string(kApp);
const std::string kSystemApp = "flatpak:system:flathub:" + std::string(kApp);

OfferId offer(const std::string& id) { return unwrap(OfferId::parse(id)); }

PlanRequest request(TransactionKind kind, const std::string& id)
{
    return unwrap(PlanRequest::create(kind, {offer(id)}));
}

Plan systemUpdate(bool withNewRuntime)
{
    std::vector<PlanItemSpec> items{flatpakApp(PlanOperation::Update)};
    if (withNewRuntime)
        items.push_back(flatpakRuntime(PlanOperation::Install));
    return flatpakPlan(TransactionKind::Update, Scope::System, std::move(items));
}

/// What the service will do with one transaction: plan, check the plan
/// against the request, wait for a confirmation when needed, execute, and
/// map every outcome onto the state machine.
class Driver
{
public:
    Driver(Backend& backend, PostQueue& queue, PlanRequest req, std::string id = "tx-1")
        : t(req.kind()), m_backend(backend), m_queue(queue), m_request(std::move(req)), m_id(std::move(id))
    {
    }

    void start()
    {
        ASSERT_TRUE(t.startPlanning());
        m_backend.plan(m_id, m_request, [this](Result<Plan> r) { onPlan(std::move(r)); });
    }

    void confirm()
    {
        ASSERT_TRUE(t.confirm());
        execute();
    }

    Result<void> cancel()
    {
        if (t.state() == S::Queued || t.state() == S::AwaitingConfirmation)
            return t.cancel();
        return m_backend.cancel(m_id);
    }

    void run() { m_queue.runAll(); }

    Transaction t;
    std::vector<std::string> events; // "started", "progress", "handle:..."
    std::vector<Progress> progress;
    int completions = 0;
    bool eventAfterCompletion = false;

private:
    void onPlan(Result<Plan> r)
    {
        if (!r)
            return finishWithError(std::move(r).error());
        if (auto ok = checkPlanForRequest(m_request, *r); !ok)
            return (void) t.fail(std::move(ok).error());
        ASSERT_TRUE(t.planReady(std::move(r).value()));
        if (t.state() == S::Planning) // accepted as is: go on
            execute();
    }

    void execute()
    {
        OperationEvents ev;
        ev.started = [this](const Plan& p) {
            note("started");
            auto v = t.verifyExecutionPlan(p);
            ASSERT_TRUE(v);
            ASSERT_NE(*v, PlanVerdict::NeedsConfirmation) << "backend and transaction disagree";
        };
        ev.progress = [this](const Progress& p) {
            note("progress");
            progress.push_back(p);
        };
        ev.handle = [this](const std::string& h) { note("handle:" + h); };
        m_backend.execute(m_id, m_request, *t.acceptedPlan(), std::move(ev), [this](Result<ExecutionResult> r) {
            ++completions;
            if (!r)
                return finishWithError(std::move(r).error());
            if (r->outcome == ExecutionResult::Outcome::PlanChanged) {
                auto v = t.verifyExecutionPlan(std::move(*r->freshPlan));
                ASSERT_TRUE(v);
                EXPECT_EQ(*v, PlanVerdict::NeedsConfirmation) << "backend and transaction disagree";
                return;
            }
            ASSERT_TRUE(t.succeed());
        });
    }

    void finishWithError(Error e)
    {
        if (e.code() == ErrorCode::Cancelled)
            (void) t.cancel();
        else if (e.code() == ErrorCode::Backend && e.backendFailure() == BackendFailure::Interrupted)
            (void) t.interrupt();
        else
            (void) t.fail(std::move(e));
    }

    void note(std::string e)
    {
        if (completions > 0)
            eventAfterCompletion = true;
        events.push_back(std::move(e));
    }

    Backend& m_backend;
    PostQueue& m_queue;
    PlanRequest m_request;
    std::string m_id;
};

Progress step(OperationPhase phase, double fraction, bool cancellable)
{
    return {.phase = phase, .fraction = fraction, .downloadedBytes = 0, .totalBytes = 0, .cancellable = cancellable};
}

} // namespace

// --- Requests and offer ids --------------------------------------------------------

TEST(OfferAddress, ReadsBackWhatOfferCreateBuilds)
{
    const Offer deb = fixtures::debOffer("firefox-esr");
    EXPECT_EQ(unwrap(decodeOfferId(deb.id())),
              (OfferAddress{BackendKind::PackageKit, Scope::System, "", "firefox-esr"}));
    const Offer fp = fixtures::flatpakOffer("app/org.mozilla.firefox/x86_64/stable", Scope::System);
    EXPECT_EQ(unwrap(decodeOfferId(fp.id())),
              (OfferAddress{BackendKind::Flatpak, Scope::System, "flathub", "app/org.mozilla.firefox/x86_64/stable"}));
    // A remote name containing ':' still splits correctly: refs have none.
    EXPECT_EQ(unwrap(decodeOfferId(offer("flatpak:user:a:b:runtime/org.x.Y/x86_64/1"))).origin, "a:b");
    for (const char* bad : {"packagekit:", "packagekit:Bad_Name", "flatpak:user:flathub", "flatpak:nobody:r:app/a/b/c",
                            "flatpak:user::app/org.x/x86_64/stable", "flatpak:user:r:notaref", "apt:gimp"})
        EXPECT_FALSE(decodeOfferId(offer(bad))) << bad;
}

TEST(PlanRequest, ValidatesItsOffers)
{
    auto ok = unwrap(PlanRequest::create(TransactionKind::Install, {offer(kUserApp)}));
    EXPECT_EQ(ok.backend(), BackendKind::Flatpak);
    EXPECT_EQ(ok.scope(), Scope::User);
    EXPECT_EQ(ok.targets()[0].package, kApp);

    EXPECT_FALSE(PlanRequest::create(TransactionKind::Install, {}));
    EXPECT_FALSE(PlanRequest::create(TransactionKind::Install, {offer(kUserApp), offer(kUserApp)}));
    EXPECT_FALSE(PlanRequest::create(TransactionKind::Install, {offer(kUserApp), offer(kSystemApp)}));
    EXPECT_FALSE(PlanRequest::create(TransactionKind::Install, {offer(kUserApp), offer("packagekit:gimp")}));
    EXPECT_FALSE(PlanRequest::create(TransactionKind::Install, {offer("unknown:thing")}));
    std::vector<OfferId> many;
    for (int i = 0; i < 201; ++i)
        many.push_back(offer("packagekit:pkg" + std::to_string(i)));
    EXPECT_FALSE(PlanRequest::create(TransactionKind::Update, many));
    many.pop_back();
    EXPECT_TRUE(PlanRequest::create(TransactionKind::Update, many));
}

TEST(PlanRequest, PlansThatDoNotAnswerTheRequestAreRefused)
{
    const PlanRequest install = request(TransactionKind::Install, kUserApp);
    auto refuse = [&](const PlanRequest& r, const Plan& p) {
        auto ok = checkPlanForRequest(r, p);
        ASSERT_FALSE(ok);
        EXPECT_EQ(ok.error().code(), ErrorCode::InvalidArgument);
    };
    EXPECT_TRUE(checkPlanForRequest(install, flatpakPlan(TransactionKind::Install, Scope::User,
                                                         {flatpakApp(PlanOperation::Install), flatpakRuntime()})));
    // Another installation, another kind, another requested package.
    refuse(install, flatpakPlan(TransactionKind::Install, Scope::System, {flatpakApp(PlanOperation::Install)}));
    refuse(install, flatpakPlan(TransactionKind::Remove, Scope::User, {flatpakApp(PlanOperation::Remove)}));
    refuse(install, flatpakPlan(TransactionKind::Install, Scope::User,
                                {flatpakApp(PlanOperation::Install, "app/org.other.App/x86_64/stable")}));

    // An install must contain every requested package...
    const PlanRequest two = unwrap(PlanRequest::create(
            TransactionKind::Install, {offer(kUserApp), offer("flatpak:user:flathub:app/org.b.B/x86_64/stable")}));
    refuse(two, flatpakPlan(TransactionKind::Install, Scope::User, {flatpakApp(PlanOperation::Install)}));
    // ...an update may leave out one already up to date, not all of them.
    const PlanRequest update = unwrap(PlanRequest::create(
            TransactionKind::Update, {offer(kSystemApp), offer("flatpak:system:flathub:app/org.b.B/x86_64/stable")}));
    EXPECT_TRUE(checkPlanForRequest(update, systemUpdate(false)));
    refuse(update, flatpakPlan(TransactionKind::Update, Scope::System,
                               {flatpakRuntime(PlanOperation::Update), flatpakApp(PlanOperation::Update, "app/org.c.C/x86_64/stable")}));
}

// --- Capabilities and queries ------------------------------------------------------------

TEST(BackendContract, CapabilitiesDifferBetweenBackends)
{
    PostQueue q;
    FakeBackend pk(BackendKind::PackageKit, q);
    FakeBackend fp(BackendKind::Flatpak, q);
    const auto a = pk.capabilities();
    const auto b = fp.capabilities();
    EXPECT_FALSE(a.userScope);
    EXPECT_TRUE(a.systemScope);
    EXPECT_TRUE(b.userScope && b.systemScope);
    EXPECT_FALSE(a.permissions);
    EXPECT_TRUE(b.permissions);
    EXPECT_FALSE(a.cancelAnytime);
    EXPECT_TRUE(b.cancelAnytime);
    EXPECT_TRUE(a.reattach);
    EXPECT_FALSE(b.reattach);
    EXPECT_FALSE(a.verifiesExecutedPlan); // D-017: a new simulation only
    EXPECT_TRUE(b.verifiesExecutedPlan);

    // What the capabilities exclude is refused, through the completion.
    std::optional<Error> perm;
    pk.permissions(offer("packagekit:gimp"), [&](Result<std::vector<std::string>> r) { perm = r.error(); });
    std::optional<Error> again;
    fp.reattach("tx", "/1_x", {}, [&](Result<ExecutionResult> r) { again = r.error(); });
    EXPECT_FALSE(perm);
    EXPECT_FALSE(again);
    q.runAll();
    ASSERT_TRUE(perm && again);
    EXPECT_EQ(perm->code(), ErrorCode::NotSupported);
    EXPECT_EQ(again->code(), ErrorCode::NotSupported);

    fp.permissionsAnswer = std::vector<std::string>{"shared=network"};
    std::vector<std::string> granted;
    fp.permissions(offer(kUserApp), [&](Result<std::vector<std::string>> r) { granted = *r; });
    q.runAll();
    EXPECT_EQ(granted, std::vector<std::string>{"shared=network"});
}

TEST(BackendContract, QueriesCompleteLaterThroughPost)
{
    PostQueue q;
    FakeBackend fp(BackendKind::Flatpak, q);
    fp.catalogSourcesAnswer = std::vector<SourceMetadata>{
            {fixtures::flathubSource(), {"/var/lib/flatpak/appstream/flathub/x86_64/active/appstream.xml.gz", {}}}};
    fp.updatesAnswer = std::vector<AvailableUpdate>{{.offer = offer(kUserApp),
                                                     .app = fixtures::appId("org.example.App"),
                                                     .kind = PlanItemKind::Application,
                                                     .fromVersion = unwrap(Version::upstream("1.0")),
                                                     .toVersion = unwrap(Version::upstream("1.1")),
                                                     .downloadSize = 1000}};
    std::size_t sources = 0, updates = 0;
    fp.catalogSources([&](Result<std::vector<SourceMetadata>> r) { sources = r->size(); });
    fp.queryUpdates([&](Result<std::vector<AvailableUpdate>> r) { updates = r->size(); });
    EXPECT_EQ(sources + updates, 0u) << "never completed inside the call";
    q.runAll();
    EXPECT_EQ(sources, 1u);
    EXPECT_EQ(updates, 1u);

    fp.availabilityAnswer = Error::backend(BackendFailure::Unavailable, "no system installation");
    std::optional<Error> error;
    fp.availability([&](Result<void> r) {
        if (!r)
            error = r.error();
    });
    q.runAll();
    ASSERT_TRUE(error);
    EXPECT_EQ(error->backendFailure(), BackendFailure::Unavailable);
}

TEST(BackendContract, ChangesMadeOutsideTheStoreAreNotified)
{
    PostQueue q;
    FakeBackend pk(BackendKind::PackageKit, q);
    std::vector<BackendChanges> seen;
    pk.setChangeListener([&](const BackendChanges& c) { seen.push_back(c); });
    pk.emitChanges({.installed = true});
    EXPECT_TRUE(seen.empty());
    q.runAll();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_TRUE(seen[0].installed);
    EXPECT_FALSE(seen[0].updates);
    pk.setChangeListener({});
    pk.emitChanges({.sources = true});
    q.runAll();
    EXPECT_EQ(seen.size(), 1u);
}

// --- Transactions through a backend -----------------------------------------------------

TEST(BackendTransactions, InstallWithoutConfirmation)
{
    PostQueue q;
    FakeBackend fp(BackendKind::Flatpak, q);
    fp.planner = [](const PlanRequest&) {
        return Result<Plan>(flatpakPlan(TransactionKind::Install, Scope::User,
                                        {flatpakApp(PlanOperation::Install), flatpakRuntime()}));
    };
    fp.progressSteps = {step(OperationPhase::Downloading, 0.3, true), step(OperationPhase::Installing, 0.9, true)};
    Driver d(fp, q, request(TransactionKind::Install, kUserApp));
    d.start();
    EXPECT_EQ(d.t.state(), S::Planning);
    d.run();
    EXPECT_EQ(d.t.state(), S::Succeeded);
    EXPECT_EQ(d.events, (std::vector<std::string>{"started", "progress", "progress"}));
    ASSERT_EQ(d.progress.size(), 2u);
    EXPECT_EQ(d.progress[0].phase, OperationPhase::Downloading);
    EXPECT_DOUBLE_EQ(d.progress[1].fraction, 0.9);
    EXPECT_EQ(d.completions, 1);
    EXPECT_FALSE(d.eventAfterCompletion);
    EXPECT_EQ(fp.executed.size(), 1u);
    EXPECT_FALSE(fp.busy());
}

TEST(BackendTransactions, NothingRunsBeforeAConfirmation)
{
    PostQueue q;
    FakeBackend fp(BackendKind::Flatpak, q);
    fp.planner = [](const PlanRequest&) {
        return Result<Plan>(flatpakPlan(TransactionKind::Install, Scope::System,
                                        {flatpakApp(PlanOperation::Install), flatpakRuntime()}));
    };
    Driver d(fp, q, request(TransactionKind::Install, kSystemApp));
    d.start();
    d.run();
    EXPECT_EQ(d.t.state(), S::AwaitingConfirmation);
    EXPECT_EQ(d.t.reasons().list(), std::vector<R>{R::SystemScope});
    EXPECT_TRUE(fp.executed.empty());
    EXPECT_TRUE(d.events.empty());

    d.confirm();
    d.run();
    EXPECT_EQ(d.t.state(), S::Succeeded);
    EXPECT_EQ(fp.executed.size(), 1u);
}

TEST(BackendTransactions, D014RuntimeAppearingAtExecutionRunsNothing)
{
    PostQueue q;
    FakeBackend fp(BackendKind::Flatpak, q);
    int planCalls = 0;
    fp.planner = [](const PlanRequest&) { return Result<Plan>(systemUpdate(false)); };
    // Between the passes, the update starts needing a new system runtime.
    fp.executionPlanner = [&](const PlanRequest&) {
        ++planCalls;
        return Result<Plan>(systemUpdate(true));
    };
    Driver d(fp, q, request(TransactionKind::Update, kSystemApp));
    d.start();
    d.run();
    EXPECT_EQ(planCalls, 1);
    EXPECT_EQ(d.t.state(), S::AwaitingConfirmation);
    EXPECT_TRUE(fp.executed.empty()) << "nothing executed";
    EXPECT_TRUE(d.events.empty()) << "`started` never called";
    EXPECT_EQ(d.t.reasons().list(), (std::vector<R>{R::SystemScope, R::PlanChanged}));
    EXPECT_EQ(d.t.plan()->items()[d.t.concerns()[0].item].kind(), PlanItemKind::Runtime);

    d.confirm();
    d.run();
    EXPECT_EQ(d.t.state(), S::Succeeded);
    ASSERT_EQ(fp.executed.size(), 1u);
    EXPECT_EQ(fp.executed[0].items().size(), 2u);
}

TEST(BackendTransactions, ASatisfiedRuntimeDoesNotStopTheExecution)
{
    PostQueue q;
    FakeBackend fp(BackendKind::Flatpak, q);
    fp.planner = [](const PlanRequest&) { return Result<Plan>(systemUpdate(true)); };
    fp.executionPlanner = [](const PlanRequest&) { return Result<Plan>(systemUpdate(false)); };
    Driver d(fp, q, request(TransactionKind::Update, kSystemApp));
    d.start();
    d.run();
    ASSERT_EQ(d.t.state(), S::AwaitingConfirmation); // system runtime install
    d.confirm();
    d.run();
    EXPECT_EQ(d.t.state(), S::Succeeded);
    ASSERT_EQ(fp.executed.size(), 1u);
    EXPECT_EQ(fp.executed[0].items().size(), 1u);
}

TEST(BackendTransactions, AnInvalidPlanFailsTheTransaction)
{
    PostQueue q;
    FakeBackend fp(BackendKind::Flatpak, q);
    // The backend plans another application than the one asked for.
    fp.planner = [](const PlanRequest&) {
        return Result<Plan>(flatpakPlan(TransactionKind::Install, Scope::User,
                                        {flatpakApp(PlanOperation::Install, "app/org.other.App/x86_64/stable")}));
    };
    Driver d(fp, q, request(TransactionKind::Install, kUserApp));
    d.start();
    d.run();
    EXPECT_EQ(d.t.state(), S::Failed);
    EXPECT_EQ(d.t.error()->code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(fp.executed.empty());
}

TEST(BackendTransactions, PlanningAndExecutionErrors)
{
    PostQueue q;
    FakeBackend pk(BackendKind::PackageKit, q);
    pk.planner = [](const PlanRequest&) {
        return Result<Plan>(Error::backend(BackendFailure::Dependency, "unresolvable"));
    };
    Driver planning(pk, q, request(TransactionKind::Install, "packagekit:gimp"));
    planning.start();
    planning.run();
    EXPECT_EQ(planning.t.state(), S::Failed);
    EXPECT_EQ(planning.t.error()->backendFailure(), BackendFailure::Dependency);

    pk.planner = [](const PlanRequest&) {
        return Result<Plan>(debPlan(TransactionKind::Install, {debPackage(PlanOperation::Install, "gimp", true)}));
    };
    pk.progressSteps = {step(OperationPhase::Downloading, 0.5, true)};
    pk.executionError = Error::backend(BackendFailure::NoSpace, "no space left on device");
    Driver running(pk, q, request(TransactionKind::Install, "packagekit:gimp"), "tx-2");
    running.start();
    running.run();
    EXPECT_EQ(running.t.state(), S::Failed);
    EXPECT_EQ(running.t.error()->backendFailure(), BackendFailure::NoSpace);
    EXPECT_EQ(running.events, (std::vector<std::string>{"started", "progress"}));
    EXPECT_FALSE(running.eventAfterCompletion);
    EXPECT_FALSE(pk.busy());
}

TEST(BackendTransactions, ALostTransactionIsInterrupted)
{
    PostQueue q;
    FakeBackend pk(BackendKind::PackageKit, q);
    pk.planner = [](const PlanRequest&) {
        return Result<Plan>(debPlan(TransactionKind::Install, {debPackage(PlanOperation::Install, "gimp", true)}));
    };
    pk.executionError = Error::backend(BackendFailure::Interrupted, "packagekitd restarted");
    Driver d(pk, q, request(TransactionKind::Install, "packagekit:gimp"));
    d.start();
    d.run();
    EXPECT_EQ(d.t.state(), S::Interrupted);
}

// --- Cancellation ------------------------------------------------------------------------

TEST(BackendCancellation, WhilePlanning)
{
    PostQueue q;
    FakeBackend fp(BackendKind::Flatpak, q);
    fp.planner = [](const PlanRequest&) {
        return Result<Plan>(flatpakPlan(TransactionKind::Install, Scope::User, {flatpakApp(PlanOperation::Install)}));
    };
    Driver d(fp, q, request(TransactionKind::Install, kUserApp));
    d.start();
    ASSERT_TRUE(d.cancel());
    d.run();
    EXPECT_EQ(d.t.state(), S::Cancelled);
    EXPECT_TRUE(fp.executed.empty());
}

TEST(BackendCancellation, FlatpakCancelsAtAnyStep)
{
    PostQueue q;
    FakeBackend fp(BackendKind::Flatpak, q);
    fp.planner = [](const PlanRequest&) {
        return Result<Plan>(flatpakPlan(TransactionKind::Install, Scope::User, {flatpakApp(PlanOperation::Install)}));
    };
    fp.progressSteps = {step(OperationPhase::Downloading, 0.1, false), step(OperationPhase::Downloading, 0.5, false),
                        step(OperationPhase::Installing, 0.9, false)};
    Driver d(fp, q, request(TransactionKind::Install, kUserApp));
    d.start();
    // Run until the execution has started and reported its first progress.
    while (d.progress.empty())
        q.runOne();
    EXPECT_EQ(d.t.state(), S::Running);
    ASSERT_TRUE(d.cancel()); // cancelAnytime, even if the step says otherwise
    d.run();
    EXPECT_EQ(d.t.state(), S::Cancelled);
    EXPECT_EQ(d.progress.size(), 1u);
}

TEST(BackendCancellation, PackageKitOnlyWhenAllowed)
{
    PostQueue q;
    FakeBackend pk(BackendKind::PackageKit, q);
    pk.planner = [](const PlanRequest&) {
        return Result<Plan>(debPlan(TransactionKind::Install, {debPackage(PlanOperation::Install, "gimp", true)}));
    };
    pk.progressSteps = {step(OperationPhase::Installing, 0.5, false), step(OperationPhase::Downloading, 0.6, true),
                        step(OperationPhase::Finishing, 0.9, false)};
    Driver d(pk, q, request(TransactionKind::Install, "packagekit:gimp"));
    d.start();
    while (d.progress.empty())
        q.runOne();
    auto refused = d.cancel();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::NotSupported);
    while (d.progress.size() < 2)
        q.runOne();
    ASSERT_TRUE(d.cancel()); // AllowCancel now true
    d.run();
    EXPECT_EQ(d.t.state(), S::Cancelled);

    auto unknown = pk.cancel("nothing");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code(), ErrorCode::NotFound);
    auto late = d.cancel(); // finished: nothing to cancel any more
    EXPECT_FALSE(late);
}

// --- Reattach after a restart ------------------------------------------------------------

TEST(BackendReattach, KnownAndUnknownHandles)
{
    PostQueue q;
    FakeBackend pk(BackendKind::PackageKit, q);
    pk.knownHandles = {"/1234_abc"};
    pk.progressSteps = {step(OperationPhase::Installing, 0.8, false)};
    std::optional<Result<ExecutionResult>> known, lost;
    int progress = 0;
    OperationEvents ev;
    ev.progress = [&](const Progress&) { ++progress; };
    pk.reattach("tx-1", "/1234_abc", ev, [&](Result<ExecutionResult> r) { known = std::move(r); });
    q.runAll();
    ASSERT_TRUE(known && *known);
    EXPECT_EQ((*known)->outcome, ExecutionResult::Outcome::Completed);
    EXPECT_EQ(progress, 1);

    pk.reattach("tx-2", "/9999_gone", {}, [&](Result<ExecutionResult> r) { lost = std::move(r); });
    q.runAll();
    ASSERT_TRUE(lost && !*lost);
    EXPECT_EQ(lost->error().code(), ErrorCode::NotFound); // the host interrupts it
}

TEST(BackendReattach, TheHandleIsReportedForTheJournal)
{
    PostQueue q;
    FakeBackend pk(BackendKind::PackageKit, q);
    pk.planner = [](const PlanRequest&) {
        return Result<Plan>(debPlan(TransactionKind::Install, {debPackage(PlanOperation::Install, "gimp", true)}));
    };
    pk.handleToReport = "/1234_abc";
    Driver d(pk, q, request(TransactionKind::Install, "packagekit:gimp"));
    d.start();
    d.run();
    EXPECT_EQ(d.events, (std::vector<std::string>{"started", "handle:/1234_abc"}));
}

// --- Concurrency -------------------------------------------------------------------------

TEST(BackendConcurrency, OneModifyingOperationPerBackend)
{
    PostQueue q;
    FakeBackend pk(BackendKind::PackageKit, q);
    FakeBackend fp(BackendKind::Flatpak, q);
    pk.planner = [](const PlanRequest&) {
        return Result<Plan>(debPlan(TransactionKind::Install, {debPackage(PlanOperation::Install, "gimp", true)}));
    };
    fp.planner = [](const PlanRequest&) {
        return Result<Plan>(flatpakPlan(TransactionKind::Install, Scope::User, {flatpakApp(PlanOperation::Install)}));
    };
    pk.progressSteps = {step(OperationPhase::Downloading, 0.5, true), step(OperationPhase::Installing, 0.9, false)};
    fp.progressSteps = pk.progressSteps;
    const Plan accepted = unwrap(pk.planner(request(TransactionKind::Install, "packagekit:gimp")));

    std::vector<std::string> results;
    pk.execute("a", request(TransactionKind::Install, "packagekit:gimp"), accepted, {},
               [&](Result<ExecutionResult> r) { results.push_back(r ? "a:done" : "a:" + std::string(toString(r.error().code()))); });
    // A second modifying operation on the same backend: refused at once.
    pk.execute("b", request(TransactionKind::Install, "packagekit:gimp"), accepted, {},
               [&](Result<ExecutionResult> r) { results.push_back(r ? "b:done" : "b:" + std::string(toString(r.error().code()))); });
    pk.refresh("c", {}, [&](Result<void> r) { results.push_back(r ? "c:done" : "c:" + std::string(toString(r.error().code()))); });
    // A plan and a query may still be asked; another backend runs in parallel.
    pk.plan("d", request(TransactionKind::Install, "packagekit:gimp"),
            [&](Result<Plan> r) { results.push_back(r ? "d:done" : "d:error"); });
    pk.queryInstalled([&](Result<std::vector<InstalledItem>> r) { results.push_back(r ? "e:done" : "e:error"); });
    fp.execute("f", request(TransactionKind::Install, kUserApp),
               unwrap(fp.planner(request(TransactionKind::Install, kUserApp))), {},
               [&](Result<ExecutionResult> r) { results.push_back(r ? "f:done" : "f:error"); });
    q.runAll();
    const std::vector<std::string> expected{"b:conflict", "c:conflict", "d:done", "e:done", "a:done", "f:done"};
    std::vector<std::string> sorted = results;
    EXPECT_EQ(sorted.size(), expected.size());
    for (const auto& e : expected)
        EXPECT_NE(std::find(results.begin(), results.end(), e), results.end()) << e;
    // Free again once done.
    std::optional<bool> refreshed;
    pk.refresh("g", {}, [&](Result<void> r) { refreshed = bool(r); });
    q.runAll();
    EXPECT_EQ(refreshed, std::optional<bool>(true));
}

TEST(BackendConcurrency, BackendAndTransactionAlwaysAgree)
{
    // mayExecute() is the rule a backend applies; verifyExecutionPlan() the
    // one the transaction applies. Same verdict for every pair.
    const std::vector<Plan> plans{systemUpdate(false), systemUpdate(true),
                                  flatpakPlan(TransactionKind::Update, Scope::System,
                                              {flatpakApp(PlanOperation::Update), flatpakRuntime(PlanOperation::Update)})};
    for (const Plan& accepted : plans)
        for (const Plan& fresh : plans) {
            Transaction t(TransactionKind::Update);
            ASSERT_TRUE(t.startPlanning());
            ASSERT_TRUE(t.planReady(accepted));
            if (t.state() == S::AwaitingConfirmation) {
                ASSERT_TRUE(t.confirm());
            }
            const auto verdict = unwrap(t.verifyExecutionPlan(fresh));
            EXPECT_EQ(mayExecute(accepted, fresh), verdict != PlanVerdict::NeedsConfirmation);
        }
}

TEST(BackendContract, PhaseNames)
{
    EXPECT_EQ(toString(OperationPhase::WaitingForAuth), "waiting-for-auth");
    EXPECT_EQ(toString(OperationPhase::Finishing), "finishing");
}
