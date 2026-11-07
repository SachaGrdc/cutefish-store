#include "planfixtures.h"

#include <cutefish/store/transaction.h>

#include <gtest/gtest.h>

using namespace cutefish::store;
using namespace planfixtures;

namespace {

using R = ConfirmationReason;
using S = TransactionState;

Plan userInstall() { return flatpakPlan(TransactionKind::Install, Scope::User, {flatpakApp(PlanOperation::Install)}); }
Plan systemInstall()
{
    return flatpakPlan(TransactionKind::Install, Scope::System, {flatpakApp(PlanOperation::Install), flatpakRuntime()});
}
Plan systemUpdate(bool withNewRuntime)
{
    std::vector<PlanItemSpec> items{flatpakApp(PlanOperation::Update)};
    if (withNewRuntime)
        items.push_back(flatpakRuntime(PlanOperation::Install));
    return flatpakPlan(TransactionKind::Update, Scope::System, std::move(items));
}

void expectConflict(const Result<void>& r)
{
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::Conflict);
}

} // namespace

TEST(Transaction, PlanWithoutReasonStaysInPlanningUntilVerified)
{
    Transaction t(TransactionKind::Install);
    EXPECT_EQ(t.state(), S::Queued);
    EXPECT_FALSE(t.plan());
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(userInstall()));
    // Accepted as is, but nothing may run yet (D-017).
    EXPECT_EQ(t.state(), S::Planning);
    ASSERT_TRUE(t.acceptedPlan());
    EXPECT_TRUE(t.reasons().empty());
    EXPECT_FALSE(t.confirmedByUser());
    expectConflict(t.succeed());

    auto verdict = t.verifyExecutionPlan(userInstall());
    ASSERT_TRUE(verdict);
    EXPECT_EQ(*verdict, PlanVerdict::Identical);
    EXPECT_EQ(t.state(), S::Running);
    ASSERT_TRUE(t.succeed());
    EXPECT_EQ(t.state(), S::Succeeded);
    EXPECT_TRUE(isTerminal(t.state()));
}

TEST(Transaction, ConfirmationThenVerifiedExecution)
{
    Transaction t(TransactionKind::Install);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(systemInstall()));
    EXPECT_EQ(t.state(), S::AwaitingConfirmation);
    EXPECT_FALSE(t.acceptedPlan());
    EXPECT_EQ(t.reasons().list(), std::vector<R>{R::SystemScope});
    EXPECT_EQ(t.concerns().size(), 2u);

    ASSERT_TRUE(t.confirm());
    EXPECT_EQ(t.state(), S::Planning);
    EXPECT_TRUE(t.confirmedByUser());
    EXPECT_TRUE(t.reasons().empty());
    ASSERT_TRUE(t.acceptedPlan());

    // Flatpak: the second pass is the executed transaction; its `ready`
    // plan goes straight to verifyExecutionPlan().
    auto verdict = t.verifyExecutionPlan(systemInstall());
    ASSERT_TRUE(verdict);
    EXPECT_EQ(*verdict, PlanVerdict::Identical);
    EXPECT_EQ(t.state(), S::Running);
    ASSERT_TRUE(t.succeed());
}

TEST(Transaction, ReplanningAfterConfirmationStaysInPlanning)
{
    // PackageKit: after a confirmation the simulation may be redone (the
    // catalogue or the state changed) before the execution check.
    Transaction t(TransactionKind::Install);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(systemInstall()));
    ASSERT_TRUE(t.confirm());
    ASSERT_TRUE(t.planReady(systemInstall()));
    EXPECT_EQ(t.state(), S::Planning);
    ASSERT_TRUE(t.lastComparison());
    EXPECT_EQ(t.lastComparison()->verdict, PlanVerdict::Identical);
    ASSERT_TRUE(t.verifyExecutionPlan(systemInstall()));
    EXPECT_EQ(t.state(), S::Running);
}

TEST(Transaction, D014RuntimeAppearingAtExecutionAsksAgain)
{
    // First plan: a plain system update, no confirmation needed, accepted
    // as is. The execution plan now needs a new system runtime: the
    // transaction never reaches running, and the user is asked, with the
    // runtime pointed at (D-014, D-017).
    Transaction t(TransactionKind::Update);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(systemUpdate(false)));
    ASSERT_EQ(t.state(), S::Planning);

    auto verdict = t.verifyExecutionPlan(systemUpdate(true));
    ASSERT_TRUE(verdict);
    EXPECT_EQ(*verdict, PlanVerdict::NeedsConfirmation);
    EXPECT_EQ(t.state(), S::AwaitingConfirmation);
    EXPECT_EQ(t.reasons().list(), (std::vector<R>{R::SystemScope, R::PlanChanged}));
    ASSERT_EQ(t.concerns().size(), 1u);
    EXPECT_EQ(t.plan()->items()[t.concerns()[0].item].package(), kRuntime);
    EXPECT_EQ(t.plan()->items()[t.concerns()[0].item].kind(), PlanItemKind::Runtime);
    expectConflict(t.succeed());

    ASSERT_TRUE(t.confirm());
    EXPECT_EQ(t.state(), S::Planning);
    auto again = t.verifyExecutionPlan(systemUpdate(true));
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, PlanVerdict::Identical);
    EXPECT_EQ(t.state(), S::Running);
    ASSERT_TRUE(t.succeed());
}

TEST(Transaction, D014RuntimeKnownFromTheStartIsConfirmedOnce)
{
    Transaction t(TransactionKind::Update);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(systemUpdate(true)));
    ASSERT_EQ(t.state(), S::AwaitingConfirmation);
    EXPECT_EQ(t.reasons().list(), std::vector<R>{R::SystemScope});

    ASSERT_TRUE(t.confirm());
    // Meanwhile another process installed the runtime: it disappears from
    // the execution plan, which is harmless.
    auto verdict = t.verifyExecutionPlan(systemUpdate(false));
    ASSERT_TRUE(verdict);
    EXPECT_EQ(*verdict, PlanVerdict::Compatible);
    EXPECT_EQ(t.state(), S::Running);
    EXPECT_EQ(t.acceptedPlan()->items().size(), 1u);
    ASSERT_TRUE(t.succeed());
}

TEST(Transaction, ChangedPlanAfterConfirmationReturnsToConfirmation)
{
    Transaction t(TransactionKind::Install);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(systemInstall()));
    ASSERT_TRUE(t.confirm());

    auto app = flatpakApp(PlanOperation::Install);
    app.revision = "c0ffee02";
    app.permissions = {"shared=network"};
    auto changed = flatpakPlan(TransactionKind::Install, Scope::System, {app, flatpakRuntime()});
    auto verdict = t.verifyExecutionPlan(changed);
    ASSERT_TRUE(verdict);
    EXPECT_EQ(*verdict, PlanVerdict::NeedsConfirmation);
    EXPECT_EQ(t.state(), S::AwaitingConfirmation);
    EXPECT_EQ(t.reasons().list(), (std::vector<R>{R::NewPermissions, R::SystemScope, R::PlanChanged}));
    EXPECT_EQ(t.plan()->items()[0].revision(), "c0ffee02");
    // The accepted plan is still the one the user confirmed.
    EXPECT_EQ(t.acceptedPlan()->items()[0].revision(), "c0ffee01");

    // A changed re-simulation is caught by planReady() as well.
    ASSERT_TRUE(t.confirm());
    auto app3 = app;
    app3.revision = "c0ffee03";
    ASSERT_TRUE(t.planReady(flatpakPlan(TransactionKind::Install, Scope::System, {app3, flatpakRuntime()})));
    EXPECT_EQ(t.state(), S::AwaitingConfirmation);
    EXPECT_TRUE(t.reasons().contains(R::PlanChanged));
}

TEST(Transaction, ExecutionNeedsAnAcceptedThenVerifiedPlan)
{
    Transaction t(TransactionKind::Install);
    ASSERT_TRUE(t.startPlanning());
    // No accepted plan yet: nothing to verify against.
    auto early = t.verifyExecutionPlan(userInstall());
    ASSERT_FALSE(early);
    EXPECT_EQ(early.error().code(), ErrorCode::Conflict);
    EXPECT_EQ(t.state(), S::Planning);

    ASSERT_TRUE(t.planReady(userInstall()));
    expectConflict(t.succeed());
    ASSERT_TRUE(t.verifyExecutionPlan(userInstall()));
    // Running: the plan can no longer be revised, by either path.
    auto twice = t.verifyExecutionPlan(userInstall());
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().code(), ErrorCode::Conflict);
    expectConflict(t.planReady(userInstall()));
    EXPECT_EQ(t.state(), S::Running);
}

TEST(Transaction, RunningNeverReturnsToConfirmation)
{
    Transaction t(TransactionKind::Update);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(systemUpdate(false)));
    ASSERT_TRUE(t.verifyExecutionPlan(systemUpdate(false)));
    ASSERT_EQ(t.state(), S::Running);
    // A different plan reported while running is refused, not turned into
    // a new confirmation (D-017).
    auto late = t.verifyExecutionPlan(systemUpdate(true));
    ASSERT_FALSE(late);
    EXPECT_EQ(late.error().code(), ErrorCode::Conflict);
    expectConflict(t.confirm());
    EXPECT_EQ(t.state(), S::Running);
    EXPECT_TRUE(t.reasons().empty());
}

TEST(Transaction, RejectsAPlanOfAnotherKind)
{
    Transaction t(TransactionKind::Remove);
    ASSERT_TRUE(t.startPlanning());
    auto r = t.planReady(userInstall());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(t.state(), S::Planning);
    EXPECT_FALSE(t.plan());

    Transaction install(TransactionKind::Install);
    ASSERT_TRUE(install.startPlanning());
    ASSERT_TRUE(install.planReady(userInstall()));
    auto wrong = install.verifyExecutionPlan(
            flatpakPlan(TransactionKind::Remove, Scope::User, {flatpakApp(PlanOperation::Remove)}));
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(install.state(), S::Planning);
}

TEST(Transaction, TransitionsOutsideTheStateMachineAreRefused)
{
    Transaction t(TransactionKind::Install);
    expectConflict(t.planReady(userInstall()));
    expectConflict(t.confirm());
    expectConflict(t.succeed());
    expectConflict(t.fail(Error::internal("x")));
    EXPECT_EQ(t.state(), S::Queued);

    ASSERT_TRUE(t.startPlanning());
    expectConflict(t.startPlanning());
    ASSERT_TRUE(t.planReady(systemInstall()));
    ASSERT_EQ(t.state(), S::AwaitingConfirmation);
    // A pending confirmation is cancelled, never interrupted (SERVICE.md §5.6).
    expectConflict(t.interrupt());
    expectConflict(t.fail(Error::internal("x")));
    expectConflict(t.planReady(systemInstall()));
    auto verify = t.verifyExecutionPlan(systemInstall());
    ASSERT_FALSE(verify);
    EXPECT_EQ(verify.error().code(), ErrorCode::Conflict);
}

TEST(Transaction, CancelAndInterruptWhilePlanning)
{
    // Validated by D-017: planning → cancelled, queued/planning → interrupted.
    Transaction cancelled(TransactionKind::Install);
    ASSERT_TRUE(cancelled.startPlanning());
    ASSERT_TRUE(cancelled.planReady(userInstall()));
    ASSERT_TRUE(cancelled.cancel());
    EXPECT_EQ(cancelled.state(), S::Cancelled);

    Transaction queued(TransactionKind::Install);
    ASSERT_TRUE(queued.interrupt());
    EXPECT_EQ(queued.state(), S::Interrupted);

    Transaction planning(TransactionKind::Install);
    ASSERT_TRUE(planning.startPlanning());
    ASSERT_TRUE(planning.interrupt());
    EXPECT_EQ(planning.state(), S::Interrupted);
}

TEST(Transaction, TerminalStates)
{
    Transaction cancelled(TransactionKind::Install);
    ASSERT_TRUE(cancelled.startPlanning());
    ASSERT_TRUE(cancelled.planReady(systemInstall()));
    ASSERT_TRUE(cancelled.cancel());
    EXPECT_EQ(cancelled.state(), S::Cancelled);
    EXPECT_TRUE(cancelled.reasons().empty());
    expectConflict(cancelled.confirm());
    expectConflict(cancelled.cancel());

    Transaction failed(TransactionKind::Install);
    ASSERT_TRUE(failed.startPlanning());
    ASSERT_TRUE(failed.fail(Error::backend(BackendFailure::Dependency, "unresolvable")));
    EXPECT_EQ(failed.state(), S::Failed);
    ASSERT_TRUE(failed.error());
    EXPECT_EQ(failed.error()->backendFailure(), BackendFailure::Dependency);
    expectConflict(failed.startPlanning());

    Transaction interrupted(TransactionKind::Install);
    ASSERT_TRUE(interrupted.startPlanning());
    ASSERT_TRUE(interrupted.planReady(userInstall()));
    ASSERT_TRUE(interrupted.verifyExecutionPlan(userInstall()));
    ASSERT_EQ(interrupted.state(), S::Running);
    ASSERT_TRUE(interrupted.interrupt());
    EXPECT_EQ(interrupted.state(), S::Interrupted);
    expectConflict(interrupted.succeed());

    Transaction queued(TransactionKind::Update);
    ASSERT_TRUE(queued.cancel());
    EXPECT_EQ(queued.state(), S::Cancelled);
}

TEST(Transaction, StableNames)
{
    EXPECT_EQ(toString(S::AwaitingConfirmation), "awaiting-confirmation");
    EXPECT_EQ(toString(S::Interrupted), "interrupted");
    for (S s : {S::Queued, S::Planning, S::AwaitingConfirmation, S::Running})
        EXPECT_FALSE(isTerminal(s)) << toString(s);
    for (S s : {S::Succeeded, S::Failed, S::Cancelled, S::Interrupted})
        EXPECT_TRUE(isTerminal(s)) << toString(s);
}

// --- Snapshot and restore (state.sqlite) ------------------------------------

TEST(TransactionSnapshot, RoundTripsEveryState)
{
    std::vector<Transaction> samples;
    samples.emplace_back(TransactionKind::Install); // queued

    Transaction planning(TransactionKind::Install);
    ASSERT_TRUE(planning.startPlanning());
    ASSERT_TRUE(planning.planReady(userInstall()));
    samples.push_back(planning);

    Transaction awaiting(TransactionKind::Update);
    ASSERT_TRUE(awaiting.startPlanning());
    ASSERT_TRUE(awaiting.planReady(systemUpdate(false)));
    ASSERT_TRUE(awaiting.verifyExecutionPlan(systemUpdate(true))); // plan-changed
    samples.push_back(awaiting);

    Transaction running = planning;
    ASSERT_TRUE(running.verifyExecutionPlan(userInstall()));
    samples.push_back(running);

    Transaction failed(TransactionKind::Install);
    ASSERT_TRUE(failed.startPlanning());
    ASSERT_TRUE(failed.fail(Error::backend(BackendFailure::Network, "timeout")));
    samples.push_back(failed);

    for (const Transaction& t : samples) {
        auto restored = Transaction::restore(t.snapshot());
        ASSERT_TRUE(restored) << toString(t.state()) << ": " << restored.error().message();
        EXPECT_EQ(restored->state(), t.state());
        EXPECT_EQ(restored->kind(), t.kind());
        EXPECT_EQ(restored->reasons(), t.reasons());
        EXPECT_EQ(restored->concerns(), t.concerns());
        EXPECT_EQ(restored->confirmedByUser(), t.confirmedByUser());
        EXPECT_EQ(restored->plan().has_value(), t.plan().has_value());
        EXPECT_EQ(restored->acceptedPlan().has_value(), t.acceptedPlan().has_value());
        EXPECT_EQ(restored->error(), t.error());
    }

    // A restored transaction keeps working.
    auto again = Transaction::restore(awaiting.snapshot());
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->reasons().contains(R::PlanChanged));
    ASSERT_TRUE(again->confirm());
    ASSERT_TRUE(again->verifyExecutionPlan(systemUpdate(true)));
    EXPECT_EQ(again->state(), S::Running);
}

TEST(TransactionSnapshot, RefusesInconsistentSnapshots)
{
    auto refuse = [](TransactionSnapshot s) {
        auto r = Transaction::restore(std::move(s));
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    };
    refuse({.kind = TransactionKind::Install, .state = S::Queued, .plan = userInstall()});
    refuse({.kind = TransactionKind::Install, .state = S::AwaitingConfirmation});
    refuse({.kind = TransactionKind::Install, .state = S::Running, .plan = userInstall()});
    refuse({.kind = TransactionKind::Remove, .state = S::Planning, .plan = userInstall()});
    refuse({.kind = TransactionKind::Install, .state = S::Planning, .planChanged = true});
    refuse({.kind = TransactionKind::Install, .state = S::Failed});
    refuse({.kind = TransactionKind::Install, .state = S::Succeeded, .error = Error::internal("x")});
}
