#pragma once

#include <cutefish/store/plan.h>
#include <cutefish/store/search.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store {

/// Why a plan needs the user's explicit confirmation (P-008, ARCHITECTURE
/// §5.4). This is the store's own confirmation: it says what must be shown
/// and accepted, and never authenticates anyone. Authorisation stays with
/// the package managers and polkit (ARCHITECTURE §9).
enum class ConfirmationReason : std::uint8_t {
    RemovesPackages, ///< Removes something the user did not ask to remove.
    NewPermissions,  ///< Grants permissions the item did not have.
    NewSource,       ///< Adds a source (remote) to the system.
    Rebase,          ///< Replaces an item by another (end-of-life rebase).
    Untrusted,       ///< Contains an item that is not authenticated.
    SystemScope,     ///< Installs or removes in a shared installation (D-011, D-014).
    PlanChanged,     ///< Differs from the plan the user accepted (P-008).
};

/// "removes-packages", "new-permissions", "new-source", "rebase",
/// "untrusted", "system-scope", "plan-changed".
std::string_view toString(ConfirmationReason reason) noexcept;

/// A set of reasons, one bit each.
class ConfirmationReasons
{
public:
    constexpr ConfirmationReasons() noexcept = default;

    constexpr void insert(ConfirmationReason reason) noexcept { m_bits |= bit(reason); }
    constexpr bool contains(ConfirmationReason reason) const noexcept { return (m_bits & bit(reason)) != 0; }
    constexpr bool empty() const noexcept { return m_bits == 0; }
    constexpr bool operator==(const ConfirmationReasons&) const noexcept = default;
    /// The reasons in the order of the enumeration.
    std::vector<ConfirmationReason> list() const;

private:
    static constexpr std::uint8_t bit(ConfirmationReason reason) noexcept
    {
        return static_cast<std::uint8_t>(1u << static_cast<unsigned>(reason));
    }
    std::uint8_t m_bits = 0;
};

/// What the confirmation rules depend on besides the plan.
struct ConfirmationPolicy
{
    /// Backends whose system-wide installation is shared by every user of
    /// the machine: installing or removing there needs SystemScope (D-011),
    /// even inside an update (D-014). Distribution packages are not in it:
    /// they are system-wide by nature, and P-008 does not ask a confirmation
    /// for them.
    BackendSet sharedSystemScope = defaultSharedSystemScope();

    static constexpr BackendSet defaultSharedSystemScope() noexcept
    {
        BackendSet set;
        set.insert(BackendKind::Flatpak);
        return set;
    }
};

/// One reason, attached to the plan item that causes it.
struct ConfirmationConcern
{
    ConfirmationReason reason;
    std::size_t item; ///< Index in Plan::items().

    bool operator==(const ConfirmationConcern&) const = default;
};

/// Result of the confirmation rules on one plan.
struct ConfirmationAssessment
{
    ConfirmationReasons reasons;
    /// Sorted by item, then by reason. Lets the interface point at the item
    /// concerned, for example the system runtime that an update would
    /// install (D-014).
    std::vector<ConfirmationConcern> concerns;

    bool required() const noexcept { return !reasons.empty(); }
};

/// Applies the confirmation rules of P-008 to a plan, item by item:
/// - RemovesPackages: a removal that was not requested;
/// - NewPermissions: an item with permissions it did not have before (all
///   of an installed item's permissions are new);
/// - NewSource, Rebase, Untrusted: the item's own flags;
/// - SystemScope: an install or a removal, requested or not, in the system
///   installation of a backend of policy.sharedSystemScope. The kind of the
///   transaction does not matter: a runtime installed by an update counts
///   (D-014).
/// PlanChanged is never produced here: it comes from comparePlans().
ConfirmationAssessment assessPlan(const Plan& plan, const ConfirmationPolicy& policy = {});

/// Outcome of comparing a fresh plan with the one the user accepted.
enum class PlanVerdict : std::uint8_t {
    Identical,         ///< No difference at all.
    Compatible,        ///< Only differences that change nothing the user accepted.
    NeedsConfirmation, ///< At least one difference requires a new confirmation.
};

/// One kind of difference between two plans.
enum class PlanChange : std::uint8_t {
    ContextChanged,     ///< Kind, backend or scope differ: nothing else is compared.
    ItemAdded,          ///< An item appeared, a dependency included.
    ItemSatisfied,      ///< An added install or update disappeared: already satisfied. Compatible.
    ItemDropped,        ///< Any other item disappeared.
    OperationChanged,
    ItemKindChanged,
    RequestChanged,     ///< The item went from requested to added, or back.
    OriginChanged,
    RevisionChanged,
    VersionChanged,     ///< Previous or target version.
    PermissionsAdded,   ///< A permission the accepted plan did not grant.
    PermissionsReduced, ///< Only permissions removed. Compatible.
    SourceAdded,        ///< The item now adds a source.
    RebaseChanged,
    TrustLost,
    RiskReduced,        ///< No longer adds a source, or became trusted. Compatible.
    SizeChanged,        ///< Download or disk size only. Compatible.
};

/// "context-changed", "item-added", … (the enumerator in kebab case).
std::string_view toString(PlanChange change) noexcept;

struct PlanDifference
{
    PlanChange change;
    std::string package; ///< Empty for ContextChanged and a plan-wide size change.
    bool needsConfirmation;

    bool operator==(const PlanDifference&) const = default;
};

struct PlanComparison
{
    PlanVerdict verdict;
    /// Sorted by package, then by change: the result does not depend on the
    /// order in which the backend listed the items.
    std::vector<PlanDifference> differences;
};

/// Semantic comparison of P-008 (ARCHITECTURE §7.3, SERVICE.md §7.2).
/// Everything that is not explicitly known to be harmless requires a new
/// confirmation: a disappeared item is harmless only if it was added by the
/// package manager and installed or updated something; a requested item
/// that disappears, or a removal that disappears, is ItemDropped.
PlanComparison comparePlans(const Plan& accepted, const Plan& fresh);

/// The check a backend makes before changing anything (D-017): the plan it
/// is about to execute (`fresh`) may run only if it needs no new
/// confirmation compared with the accepted plan. Same rule as
/// Transaction::verifyExecutionPlan(), so the backend's decision and the
/// transaction's state always agree. Callable from any thread (pure).
bool mayExecute(const Plan& accepted, const Plan& fresh);

} // namespace cutefish::store
