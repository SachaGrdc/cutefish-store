#pragma once

#include <cutefish/store/confirmation.h>
#include <cutefish/store/error.h>
#include <cutefish/store/plan.h>
#include <cutefish/store/result.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

namespace cutefish::store {

/// States of a transaction (ARCHITECTURE §3.5, SERVICE.md §5.1).
enum class TransactionState : std::uint8_t {
    Queued,
    Planning,
    AwaitingConfirmation,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    Interrupted,
};

/// "queued", "planning", "awaiting-confirmation", "running", "succeeded",
/// "failed", "cancelled", "interrupted".
std::string_view toString(TransactionState state) noexcept;
bool isTerminal(TransactionState state) noexcept;

/// The state machine of one transaction, with its plans (P-008). It only
/// records and decides: it calls no backend, keeps no time and creates no
/// thread. The service drives it from its main thread and turns each change
/// into D-Bus properties.
///
/// Flow (D-017): the transaction stays in planning until the plan the
/// backend will really execute has been checked. `running` therefore always
/// means that the backend may change the system; there is no way back from
/// running to awaiting-confirmation.
/// - startPlanning(): queued → planning;
/// - planReady(plan): a plan computed without executing anything (first
///   Flatpak pass, PackageKit simulation). Without an accepted plan: if the
///   rules ask for a confirmation, awaiting-confirmation; otherwise the plan
///   is accepted as is and the transaction stays in planning. With an
///   accepted plan (after a confirmation): compared with it; still planning
///   if compatible, awaiting-confirmation with PlanChanged otherwise;
/// - confirm(): awaiting-confirmation → planning; the pending plan becomes
///   the accepted plan;
/// - verifyExecutionPlan(plan): the plan the backend is about to execute
///   (second Flatpak pass in `ready`, new PackageKit simulation), compared
///   with the accepted plan. Compatible: planning → running, and only then
///   may the backend execute. Otherwise: planning → awaiting-confirmation
///   with PlanChanged, and the backend must not execute. For PackageKit the
///   simulation is a separate transaction: the check narrows the window
///   between plan and execution, it does not close it (D-017);
/// - succeed(), fail(), cancel(), interrupt(): terminal states.
///
/// Every method that is not allowed in the current state returns a Conflict
/// error and changes nothing.
class Transaction
{
public:
    explicit Transaction(TransactionKind kind, ConfirmationPolicy policy = {});

    TransactionKind kind() const noexcept { return m_kind; }
    TransactionState state() const noexcept { return m_state; }

    /// The plan to show: the one awaiting confirmation, or the accepted one.
    /// Empty before the first plan.
    const std::optional<Plan>& plan() const noexcept { return m_plan; }
    /// The plan accepted by the user, or as is when it needed no
    /// confirmation: the reference of every comparison, and what the
    /// backend's execution plan is checked against. Empty until then.
    const std::optional<Plan>& acceptedPlan() const noexcept { return m_accepted; }
    /// Reasons and concerns of the pending confirmation; empty outside
    /// awaiting-confirmation.
    const ConfirmationReasons& reasons() const noexcept { return m_reasons; }
    const std::vector<ConfirmationConcern>& concerns() const noexcept { return m_concerns; }
    /// True once the user has confirmed at least once.
    bool confirmedByUser() const noexcept { return m_confirmedByUser; }
    /// The last comparison made between an accepted plan and a fresh one.
    const std::optional<PlanComparison>& lastComparison() const noexcept { return m_lastComparison; }
    /// Set in the failed state.
    const std::optional<Error>& error() const noexcept { return m_error; }

    Result<void> startPlanning();
    Result<void> planReady(Plan plan);
    Result<void> confirm();
    /// From planning, once a plan has been accepted. Returns the verdict:
    /// Identical or Compatible, the transaction is running; NeedsConfirmation,
    /// it is awaiting confirmation and the backend must not execute.
    Result<PlanVerdict> verifyExecutionPlan(Plan plan);
    /// From running, which only verifyExecutionPlan() can reach: a
    /// transaction never succeeds without that check.
    Result<void> succeed();
    /// From planning or running.
    Result<void> fail(Error error);
    /// From queued, planning, awaiting-confirmation, or running (when the
    /// backend accepted the cancellation).
    Result<void> cancel();
    /// From queued, planning or running: the service stopped, or the
    /// backend lost the transaction (SERVICE.md §5.6).
    Result<void> interrupt();

private:
    Result<void> expect(std::initializer_list<TransactionState> allowed, std::string_view action) const;
    void awaitConfirmation(Plan plan, bool changed);
    /// Records a fresh plan against the accepted one, if any. Returns false
    /// when it went to awaiting-confirmation.
    bool consider(Plan& plan);

    ConfirmationPolicy m_policy;
    std::optional<Plan> m_plan;
    std::optional<Plan> m_accepted;
    std::optional<PlanComparison> m_lastComparison;
    std::optional<Error> m_error;
    ConfirmationReasons m_reasons;
    std::vector<ConfirmationConcern> m_concerns;
    TransactionKind m_kind;
    TransactionState m_state = TransactionState::Queued;
    bool m_confirmedByUser = false;
};

} // namespace cutefish::store
