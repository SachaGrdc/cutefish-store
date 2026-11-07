#include <cutefish/store/plan.h>

#include "text.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace cutefish::store {

namespace {

constexpr std::size_t kMaxPermissionBytes = 1024;
constexpr std::size_t kMaxPermissions = 256;

PlanOperation requestedOperation(TransactionKind kind) noexcept
{
    switch (kind) {
    case TransactionKind::Install: return PlanOperation::Install;
    case TransactionKind::Remove: return PlanOperation::Remove;
    case TransactionKind::Update: return PlanOperation::Update;
    }
    return PlanOperation::Install;
}

Result<void> normalisePermissions(std::vector<std::string>& list, std::string_view what)
{
    if (list.size() > kMaxPermissions)
        return Error::invalidArgument(std::string(what) + ": too many entries");
    for (const std::string& p : list)
        if (auto ok = detail::checkText(p, what, kMaxPermissionBytes, true); !ok)
            return ok;
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    return {};
}

Result<void> checkItem(PlanItemSpec& item, const PlanSpec& plan)
{
    if (auto ok = detail::checkOpaqueIdentifier(item.package, "plan package"); !ok)
        return ok;
    const std::string context = " (" + item.package + ")";
    if (!item.origin.empty())
        if (auto ok = detail::checkOpaqueIdentifier(item.origin, "plan origin" + context); !ok)
            return ok;
    if (!item.revision.empty())
        if (auto ok = detail::checkOpaqueIdentifier(item.revision, "plan revision" + context); !ok)
            return ok;
    if (!item.rebaseFrom.empty())
        if (auto ok = detail::checkOpaqueIdentifier(item.rebaseFrom, "plan rebase" + context); !ok)
            return ok;

    if (item.operation == PlanOperation::Remove && item.toVersion)
        return Error::invalidArgument("a removal has no target version" + context);
    if (item.operation == PlanOperation::Install && item.fromVersion)
        return Error::invalidArgument("an install has no previous version" + context);

    const VersionScheme scheme =
            plan.backend == BackendKind::PackageKit ? VersionScheme::Debian : VersionScheme::Upstream;
    for (const auto* v : {&item.fromVersion, &item.toVersion})
        if (*v && (*v)->scheme() != scheme)
            return Error::invalidArgument(std::string("a ") + std::string(toString(plan.backend))
                                          + " plan needs " + (scheme == VersionScheme::Debian ? "Debian" : "upstream")
                                          + " versions" + context);

    if (item.requested && item.operation != requestedOperation(plan.kind))
        return Error::invalidArgument("a requested item must " + std::string(toString(plan.kind)) + context);

    if (auto ok = normalisePermissions(item.permissions, "plan permission" + context); !ok)
        return ok;
    return normalisePermissions(item.oldPermissions, "plan old permission" + context);
}

} // namespace

std::string_view toString(TransactionKind kind) noexcept
{
    switch (kind) {
    case TransactionKind::Install: return "install";
    case TransactionKind::Remove: return "remove";
    case TransactionKind::Update: return "update";
    }
    return "install";
}

std::string_view toString(PlanOperation operation) noexcept
{
    switch (operation) {
    case PlanOperation::Install: return "install";
    case PlanOperation::Update: return "update";
    case PlanOperation::Downgrade: return "downgrade";
    case PlanOperation::Remove: return "remove";
    }
    return "install";
}

std::string_view toString(PlanItemKind kind) noexcept
{
    switch (kind) {
    case PlanItemKind::Application: return "application";
    case PlanItemKind::Runtime: return "runtime";
    case PlanItemKind::Package: return "package";
    }
    return "package";
}

std::optional<TransactionKind> parseTransactionKind(std::string_view text) noexcept
{
    for (const TransactionKind k : {TransactionKind::Install, TransactionKind::Remove, TransactionKind::Update})
        if (toString(k) == text)
            return k;
    return std::nullopt;
}

std::optional<PlanOperation> parsePlanOperation(std::string_view text) noexcept
{
    for (const PlanOperation o :
         {PlanOperation::Install, PlanOperation::Update, PlanOperation::Downgrade, PlanOperation::Remove})
        if (toString(o) == text)
            return o;
    return std::nullopt;
}

std::optional<PlanItemKind> parsePlanItemKind(std::string_view text) noexcept
{
    for (const PlanItemKind k : {PlanItemKind::Application, PlanItemKind::Runtime, PlanItemKind::Package})
        if (toString(k) == text)
            return k;
    return std::nullopt;
}

std::vector<std::string> PlanItem::addedPermissions() const
{
    std::vector<std::string> added;
    std::set_difference(m_spec.permissions.begin(), m_spec.permissions.end(), m_spec.oldPermissions.begin(),
                        m_spec.oldPermissions.end(), std::back_inserter(added));
    return added;
}

Result<Plan> Plan::create(PlanSpec spec)
{
    if (spec.items.empty())
        return Error::invalidArgument("a plan needs at least one item");
    if (spec.backend == BackendKind::PackageKit && spec.scope != Scope::System)
        return Error::invalidArgument("a PackageKit plan is always system-wide");

    bool anyRequested = false;
    for (PlanItemSpec& item : spec.items) {
        if (auto ok = checkItem(item, spec); !ok)
            return std::move(ok).error();
        anyRequested = anyRequested || item.requested;
    }
    if (!anyRequested)
        return Error::invalidArgument("a plan needs at least one requested item");

    // Plans are short (a handful of items, a few hundred for a large
    // update): a sorted copy of the names finds duplicates without a set.
    std::vector<std::string_view> names;
    names.reserve(spec.items.size());
    for (const PlanItemSpec& item : spec.items)
        names.push_back(item.package);
    std::sort(names.begin(), names.end());
    if (auto dup = std::adjacent_find(names.begin(), names.end()); dup != names.end())
        return Error::invalidArgument("duplicate plan item: " + std::string(*dup));

    std::vector<PlanItem> items;
    items.reserve(spec.items.size());
    for (PlanItemSpec& item : spec.items)
        items.push_back(PlanItem(std::move(item)));
    return Plan(spec.kind, spec.backend, spec.scope, std::move(items), spec.installedSizeChange);
}

const PlanItem* Plan::find(std::string_view package) const noexcept
{
    for (const PlanItem& item : m_items)
        if (item.package() == package)
            return &item;
    return nullptr;
}

std::uint64_t Plan::downloadSize() const noexcept
{
    std::uint64_t total = 0;
    for (const PlanItem& item : m_items)
        total += item.downloadSize();
    return total;
}

} // namespace cutefish::store
