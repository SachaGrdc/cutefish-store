#include <cutefish/store/transaction.h>

#include <algorithm>
#include <string>
#include <utility>

namespace cutefish::store {

std::string_view toString(TransactionState state) noexcept
{
    switch (state) {
    case TransactionState::Queued: return "queued";
    case TransactionState::Planning: return "planning";
    case TransactionState::AwaitingConfirmation: return "awaiting-confirmation";
    case TransactionState::Running: return "running";
    case TransactionState::Succeeded: return "succeeded";
    case TransactionState::Failed: return "failed";
    case TransactionState::Cancelled: return "cancelled";
    case TransactionState::Interrupted: return "interrupted";
    }
    return "failed";
}

bool isTerminal(TransactionState state) noexcept
{
    switch (state) {
    case TransactionState::Succeeded:
    case TransactionState::Failed:
    case TransactionState::Cancelled:
    case TransactionState::Interrupted:
        return true;
    case TransactionState::Queued:
    case TransactionState::Planning:
    case TransactionState::AwaitingConfirmation:
    case TransactionState::Running:
        return false;
    }
    return true;
}

Transaction::Transaction(TransactionKind kind, ConfirmationPolicy policy) : m_policy(policy), m_kind(kind) {}

Result<void> Transaction::expect(std::initializer_list<TransactionState> allowed, std::string_view action) const
{
    if (std::find(allowed.begin(), allowed.end(), m_state) != allowed.end())
        return {};
    return Error::conflict(std::string(action) + " is not allowed in state " + std::string(toString(m_state)));
}

void Transaction::awaitConfirmation(Plan plan, bool changed)
{
    ConfirmationAssessment assessment = assessPlan(plan, m_policy);
    m_reasons = assessment.reasons;
    if (changed)
        m_reasons.insert(ConfirmationReason::PlanChanged);
    m_concerns = std::move(assessment.concerns);
    m_plan = std::move(plan);
    m_state = TransactionState::AwaitingConfirmation;
}

bool Transaction::consider(Plan& plan)
{
    if (m_accepted) {
        PlanComparison comparison = comparePlans(*m_accepted, plan);
        const bool changed = comparison.verdict == PlanVerdict::NeedsConfirmation;
        m_lastComparison = std::move(comparison);
        if (changed) {
            awaitConfirmation(std::move(plan), true);
            return false;
        }
    } else if (assessPlan(plan, m_policy).required()) {
        awaitConfirmation(std::move(plan), false);
        return false;
    }
    m_accepted = plan;
    m_plan = std::move(plan);
    m_reasons = {};
    m_concerns.clear();
    return true;
}

Result<void> Transaction::startPlanning()
{
    if (auto ok = expect({TransactionState::Queued}, "planning"); !ok)
        return ok;
    m_state = TransactionState::Planning;
    return {};
}

Result<void> Transaction::planReady(Plan plan)
{
    if (auto ok = expect({TransactionState::Planning}, "a plan"); !ok)
        return ok;
    if (plan.kind() != m_kind)
        return Error::invalidArgument("the plan does not match the transaction kind");
    // Accepted or not, the transaction stays in planning until the
    // execution plan is verified (D-017).
    consider(plan);
    return {};
}

Result<void> Transaction::confirm()
{
    if (auto ok = expect({TransactionState::AwaitingConfirmation}, "confirmation"); !ok)
        return ok;
    m_accepted = m_plan;
    m_confirmedByUser = true;
    m_reasons = {};
    m_concerns.clear();
    m_state = TransactionState::Planning;
    return {};
}

Result<PlanVerdict> Transaction::verifyExecutionPlan(Plan plan)
{
    if (auto ok = expect({TransactionState::Planning}, "an execution plan"); !ok)
        return std::move(ok).error();
    if (!m_accepted)
        return Error::conflict("no plan has been accepted yet");
    if (plan.kind() != m_kind)
        return Error::invalidArgument("the plan does not match the transaction kind");

    if (!consider(plan))
        return PlanVerdict::NeedsConfirmation;
    m_state = TransactionState::Running;
    return m_lastComparison->verdict;
}

Result<void> Transaction::succeed()
{
    if (auto ok = expect({TransactionState::Running}, "success"); !ok)
        return ok;
    m_state = TransactionState::Succeeded;
    return {};
}

Result<void> Transaction::fail(Error error)
{
    if (auto ok = expect({TransactionState::Planning, TransactionState::Running}, "failure"); !ok)
        return ok;
    m_error = std::move(error);
    m_state = TransactionState::Failed;
    return {};
}

Result<void> Transaction::cancel()
{
    if (auto ok = expect({TransactionState::Queued, TransactionState::Planning, TransactionState::AwaitingConfirmation,
                          TransactionState::Running},
                         "cancellation");
        !ok)
        return ok;
    m_reasons = {};
    m_concerns.clear();
    m_state = TransactionState::Cancelled;
    return {};
}

Result<void> Transaction::interrupt()
{
    if (auto ok = expect({TransactionState::Queued, TransactionState::Planning, TransactionState::Running},
                         "interruption");
        !ok)
        return ok;
    m_state = TransactionState::Interrupted;
    return {};
}

} // namespace cutefish::store
