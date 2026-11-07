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
/// Flow:
/// - startPlanning(): queued → planning;
/// - planReady(plan), first plan: running when the rules ask for no
///   confirmation (the plan is then accepted as is), awaiting-confirmation
///   otherwise;
/// - confirm(): awaiting-confirmation → planning; the pending plan becomes
///   the accepted plan, and the backend computes a second one;
/// - planReady(plan) after a confirmation: compared with the accepted plan;
///   running if compatible, back to awaiting-confirmation with PlanChanged
///   otherwise;
/// - verifyExecutionPlan(plan): last check of the plan the backend is about
///   to execute (second Flatpak pass, new PackageKit simulation); same
///   comparison; on a difference, running → awaiting-confirmation. The
///   backend must not have changed anything yet;
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
    /// Reasons and concerns of the pending confirmation; empty outside
    /// awaiting-confirmation.
    const ConfirmationReasons& reasons() const noexcept { return m_reasons; }
    const std::vector<ConfirmationConcern>& concerns() const noexcept { return m_concerns; }
    /// True once the user has confirmed at least once.
    bool confirmedByUser() const noexcept { return m_confirmedByUser; }
    /// True once verifyExecutionPlan() has accepted the plan: from then on
    /// the backend may change the system, and the plan can no longer be
    /// revised.
    bool executing() const noexcept { return m_executing; }
    /// The last comparison made between an accepted plan and a fresh one.
    const std::optional<PlanComparison>& lastComparison() const noexcept { return m_lastComparison; }
    /// Set in the failed state.
    const std::optional<Error>& error() const noexcept { return m_error; }

    Result<void> startPlanning();
    Result<void> planReady(Plan plan);
    Result<void> confirm();
    /// Returns the verdict; NeedsConfirmation means the transaction went
    /// back to awaiting-confirmation and the backend must not execute.
    Result<PlanVerdict> verifyExecutionPlan(Plan plan);
    /// From running, once verifyExecutionPlan() has accepted the plan: a
    /// transaction never succeeds without that last check.
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
    void accept(Plan plan);

    ConfirmationPolicy m_policy;
    std::optional<Plan> m_plan;
    /// The plan accepted by the user, or automatically when it needed no
    /// confirmation. Reference of every comparison.
    std::optional<Plan> m_accepted;
    std::optional<PlanComparison> m_lastComparison;
    std::optional<Error> m_error;
    ConfirmationReasons m_reasons;
    std::vector<ConfirmationConcern> m_concerns;
    TransactionKind m_kind;
    TransactionState m_state = TransactionState::Queued;
    bool m_confirmedByUser = false;
    bool m_executing = false;
};

} // namespace cutefish::store
