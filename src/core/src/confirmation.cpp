#include <cutefish/store/confirmation.h>

#include <algorithm>
#include <tuple>

namespace cutefish::store {

std::string_view toString(ConfirmationReason reason) noexcept
{
    switch (reason) {
    case ConfirmationReason::RemovesPackages: return "removes-packages";
    case ConfirmationReason::NewPermissions: return "new-permissions";
    case ConfirmationReason::NewSource: return "new-source";
    case ConfirmationReason::Rebase: return "rebase";
    case ConfirmationReason::Untrusted: return "untrusted";
    case ConfirmationReason::SystemScope: return "system-scope";
    case ConfirmationReason::PlanChanged: return "plan-changed";
    }
    return "plan-changed";
}

std::vector<ConfirmationReason> ConfirmationReasons::list() const
{
    std::vector<ConfirmationReason> out;
    for (const ConfirmationReason r :
         {ConfirmationReason::RemovesPackages, ConfirmationReason::NewPermissions, ConfirmationReason::NewSource,
          ConfirmationReason::Rebase, ConfirmationReason::Untrusted, ConfirmationReason::SystemScope,
          ConfirmationReason::PlanChanged})
        if (contains(r))
            out.push_back(r);
    return out;
}

ConfirmationAssessment assessPlan(const Plan& plan, const ConfirmationPolicy& policy)
{
    ConfirmationAssessment result;
    const bool sharedScope = plan.scope() == Scope::System && policy.sharedSystemScope.contains(plan.backend());
    const auto items = plan.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const PlanItem& item = items[i];
        auto add = [&](ConfirmationReason reason) {
            result.reasons.insert(reason);
            result.concerns.push_back({reason, i});
        };
        // Pushed in the order of the enumeration, so concerns come out
        // sorted by item, then by reason.
        if (item.operation() == PlanOperation::Remove && !item.requested())
            add(ConfirmationReason::RemovesPackages);
        if (!item.addedPermissions().empty())
            add(ConfirmationReason::NewPermissions);
        if (item.addsSource())
            add(ConfirmationReason::NewSource);
        if (!item.rebaseFrom().empty())
            add(ConfirmationReason::Rebase);
        if (!item.trusted())
            add(ConfirmationReason::Untrusted);
        if (sharedScope
            && (item.operation() == PlanOperation::Install || item.operation() == PlanOperation::Remove))
            add(ConfirmationReason::SystemScope);
    }
    return result;
}

std::string_view toString(PlanChange change) noexcept
{
    switch (change) {
    case PlanChange::ContextChanged: return "context-changed";
    case PlanChange::ItemAdded: return "item-added";
    case PlanChange::ItemSatisfied: return "item-satisfied";
    case PlanChange::ItemDropped: return "item-dropped";
    case PlanChange::OperationChanged: return "operation-changed";
    case PlanChange::ItemKindChanged: return "item-kind-changed";
    case PlanChange::RequestChanged: return "request-changed";
    case PlanChange::OriginChanged: return "origin-changed";
    case PlanChange::RevisionChanged: return "revision-changed";
    case PlanChange::VersionChanged: return "version-changed";
    case PlanChange::PermissionsAdded: return "permissions-added";
    case PlanChange::PermissionsReduced: return "permissions-reduced";
    case PlanChange::SourceAdded: return "source-added";
    case PlanChange::RebaseChanged: return "rebase-changed";
    case PlanChange::TrustLost: return "trust-lost";
    case PlanChange::RiskReduced: return "risk-reduced";
    case PlanChange::SizeChanged: return "size-changed";
    }
    return "context-changed";
}

namespace {

void compareItems(const PlanItem& a, const PlanItem& b, std::vector<PlanDifference>& out)
{
    auto add = [&](PlanChange change, bool needsConfirmation) {
        out.push_back({change, a.package(), needsConfirmation});
    };
    if (a.operation() != b.operation())
        add(PlanChange::OperationChanged, true);
    if (a.kind() != b.kind())
        add(PlanChange::ItemKindChanged, true);
    if (a.requested() != b.requested())
        add(PlanChange::RequestChanged, true);
    if (a.origin() != b.origin())
        add(PlanChange::OriginChanged, true);
    if (a.revision() != b.revision())
        add(PlanChange::RevisionChanged, true);
    if (a.fromVersion() != b.fromVersion() || a.toVersion() != b.toVersion())
        add(PlanChange::VersionChanged, true);

    // Permissions: what matters is what the item will be allowed to do
    // after the operation, compared with what the user accepted.
    const auto accepted = a.permissions();
    const auto fresh = b.permissions();
    if (!std::includes(accepted.begin(), accepted.end(), fresh.begin(), fresh.end()))
        add(PlanChange::PermissionsAdded, true);
    else if (fresh.size() != accepted.size())
        add(PlanChange::PermissionsReduced, false);

    if (!a.addsSource() && b.addsSource())
        add(PlanChange::SourceAdded, true);
    if (a.rebaseFrom() != b.rebaseFrom())
        add(PlanChange::RebaseChanged, true);
    if (a.trusted() && !b.trusted())
        add(PlanChange::TrustLost, true);
    if ((a.addsSource() && !b.addsSource()) || (!a.trusted() && b.trusted()))
        add(PlanChange::RiskReduced, false);
    if (a.downloadSize() != b.downloadSize())
        add(PlanChange::SizeChanged, false);
}

} // namespace

PlanComparison comparePlans(const Plan& accepted, const Plan& fresh)
{
    PlanComparison result{PlanVerdict::Identical, {}};
    auto& diffs = result.differences;

    if (accepted.kind() != fresh.kind() || accepted.backend() != fresh.backend()
        || accepted.scope() != fresh.scope()) {
        diffs.push_back({PlanChange::ContextChanged, {}, true});
        result.verdict = PlanVerdict::NeedsConfirmation;
        return result;
    }

    for (const PlanItem& a : accepted.items()) {
        if (const PlanItem* b = fresh.find(a.package())) {
            compareItems(a, *b, diffs);
            continue;
        }
        const bool satisfied =
                !a.requested() && (a.operation() == PlanOperation::Install || a.operation() == PlanOperation::Update);
        diffs.push_back({satisfied ? PlanChange::ItemSatisfied : PlanChange::ItemDropped, a.package(), !satisfied});
    }
    for (const PlanItem& b : fresh.items())
        if (!accepted.find(b.package()))
            diffs.push_back({PlanChange::ItemAdded, b.package(), true});
    if (accepted.installedSizeChange() != fresh.installedSizeChange())
        diffs.push_back({PlanChange::SizeChanged, {}, false});

    std::sort(diffs.begin(), diffs.end(), [](const PlanDifference& x, const PlanDifference& y) {
        return std::tie(x.package, x.change) < std::tie(y.package, y.change);
    });
    if (!diffs.empty())
        result.verdict = std::any_of(diffs.begin(), diffs.end(), [](const PlanDifference& d) { return d.needsConfirmation; })
                ? PlanVerdict::NeedsConfirmation
                : PlanVerdict::Compatible;
    return result;
}

bool mayExecute(const Plan& accepted, const Plan& fresh)
{
    return comparePlans(accepted, fresh).verdict != PlanVerdict::NeedsConfirmation;
}

} // namespace cutefish::store
