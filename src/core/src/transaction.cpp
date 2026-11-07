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

void Transaction::accept(Plan plan)
{
    m_accepted = plan;
    m_plan = std::move(plan);
    m_reasons = {};
    m_concerns.clear();
    m_state = TransactionState::Running;
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

    if (m_accepted) {
        // Second plan, after a confirmation (P-008).
        PlanComparison comparison = comparePlans(*m_accepted, plan);
        const bool changed = comparison.verdict == PlanVerdict::NeedsConfirmation;
        m_lastComparison = std::move(comparison);
        if (changed)
            awaitConfirmation(std::move(plan), true);
        else
            accept(std::move(plan));
        return {};
    }
    if (assessPlan(plan, m_policy).required())
        awaitConfirmation(std::move(plan), false);
    else
        accept(std::move(plan));
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
    if (auto ok = expect({TransactionState::Running}, "an execution plan"); !ok)
        return std::move(ok).error();
    if (m_executing)
        return Error::conflict("the transaction is already executing");
    if (plan.kind() != m_kind)
        return Error::invalidArgument("the plan does not match the transaction kind");

    PlanComparison comparison = comparePlans(*m_accepted, plan);
    const PlanVerdict verdict = comparison.verdict;
    m_lastComparison = std::move(comparison);
    if (verdict == PlanVerdict::NeedsConfirmation) {
        awaitConfirmation(std::move(plan), true);
        return verdict;
    }
    accept(std::move(plan));
    m_executing = true;
    return verdict;
}

Result<void> Transaction::succeed()
{
    if (auto ok = expect({TransactionState::Running}, "success"); !ok)
        return ok;
    if (!m_executing)
        return Error::conflict("success before the execution plan was verified");
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
