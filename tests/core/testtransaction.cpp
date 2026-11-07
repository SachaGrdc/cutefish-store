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

TEST(Transaction, PlanWithoutReasonRunsDirectly)
{
    Transaction t(TransactionKind::Install);
    EXPECT_EQ(t.state(), S::Queued);
    EXPECT_FALSE(t.plan());
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(userInstall()));
    EXPECT_EQ(t.state(), S::Running);
    EXPECT_FALSE(t.executing());
    EXPECT_FALSE(t.confirmedByUser());

    auto verdict = t.verifyExecutionPlan(userInstall());
    ASSERT_TRUE(verdict);
    EXPECT_EQ(*verdict, PlanVerdict::Identical);
    EXPECT_TRUE(t.executing());
    ASSERT_TRUE(t.succeed());
    EXPECT_EQ(t.state(), S::Succeeded);
    EXPECT_TRUE(isTerminal(t.state()));
}

TEST(Transaction, ConfirmationThenSecondPass)
{
    Transaction t(TransactionKind::Install);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(systemInstall()));
    EXPECT_EQ(t.state(), S::AwaitingConfirmation);
    EXPECT_EQ(t.reasons().list(), std::vector<R>{R::SystemScope});
    EXPECT_EQ(t.concerns().size(), 2u);

    ASSERT_TRUE(t.confirm());
    EXPECT_EQ(t.state(), S::Planning);
    EXPECT_TRUE(t.confirmedByUser());
    EXPECT_TRUE(t.reasons().empty());

    ASSERT_TRUE(t.planReady(systemInstall()));
    EXPECT_EQ(t.state(), S::Running);
    ASSERT_TRUE(t.lastComparison());
    EXPECT_EQ(t.lastComparison()->verdict, PlanVerdict::Identical);
    ASSERT_TRUE(t.verifyExecutionPlan(systemInstall()));
    ASSERT_TRUE(t.succeed());
}

TEST(Transaction, D014RuntimeAppearingAtExecutionAsksAgain)
{
    // First plan: a plain system update, no confirmation needed. At the
    // second pass the update now needs a new system runtime: nothing runs,
    // and the user is asked, with the runtime pointed at (D-014).
    Transaction t(TransactionKind::Update);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(systemUpdate(false)));
    ASSERT_EQ(t.state(), S::Running);

    auto verdict = t.verifyExecutionPlan(systemUpdate(true));
    ASSERT_TRUE(verdict);
    EXPECT_EQ(*verdict, PlanVerdict::NeedsConfirmation);
    EXPECT_EQ(t.state(), S::AwaitingConfirmation);
    EXPECT_FALSE(t.executing());
    EXPECT_EQ(t.reasons().list(), (std::vector<R>{R::SystemScope, R::PlanChanged}));
    ASSERT_EQ(t.concerns().size(), 1u);
    EXPECT_EQ(t.plan()->items()[t.concerns()[0].item].package(), kRuntime);
    EXPECT_EQ(t.plan()->items()[t.concerns()[0].item].kind(), PlanItemKind::Runtime);

    ASSERT_TRUE(t.confirm());
    ASSERT_TRUE(t.planReady(systemUpdate(true)));
    ASSERT_EQ(t.state(), S::Running);
    auto again = t.verifyExecutionPlan(systemUpdate(true));
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, PlanVerdict::Identical);
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
    // the second pass, which is harmless.
    ASSERT_TRUE(t.planReady(systemUpdate(false)));
    EXPECT_EQ(t.state(), S::Running);
    EXPECT_EQ(t.lastComparison()->verdict, PlanVerdict::Compatible);
    auto verdict = t.verifyExecutionPlan(systemUpdate(false));
    ASSERT_TRUE(verdict);
    EXPECT_EQ(*verdict, PlanVerdict::Identical);
    ASSERT_TRUE(t.succeed());
}

TEST(Transaction, ChangedSecondPassReturnsToConfirmation)
{
    Transaction t(TransactionKind::Install);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(systemInstall()));
    ASSERT_TRUE(t.confirm());

    auto app = flatpakApp(PlanOperation::Install);
    app.revision = "c0ffee02";
    app.permissions = {"shared=network"};
    ASSERT_TRUE(t.planReady(flatpakPlan(TransactionKind::Install, Scope::System, {app, flatpakRuntime()})));
    EXPECT_EQ(t.state(), S::AwaitingConfirmation);
    EXPECT_EQ(t.reasons().list(), (std::vector<R>{R::NewPermissions, R::SystemScope, R::PlanChanged}));
    EXPECT_EQ(t.lastComparison()->verdict, PlanVerdict::NeedsConfirmation);
    EXPECT_EQ(t.plan()->items()[0].revision(), "c0ffee02");
}

TEST(Transaction, ExecutionNeedsAVerifiedPlan)
{
    Transaction t(TransactionKind::Install);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(userInstall()));
    expectConflict(t.succeed());
    ASSERT_TRUE(t.verifyExecutionPlan(userInstall()));
    auto twice = t.verifyExecutionPlan(userInstall());
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().code(), ErrorCode::Conflict);
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
    auto verify = t.verifyExecutionPlan(systemInstall());
    ASSERT_FALSE(verify);
    EXPECT_EQ(verify.error().code(), ErrorCode::Conflict);
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
