#pragma once

// Builders of plan items and plans, as a backend would report them.

#include "fixtures.h"

#include <cutefish/store/plan.h>

#include <string>
#include <utility>
#include <vector>

namespace planfixtures {

using namespace cutefish::store;
using fixtures::unwrap;

inline const char* kApp = "app/org.example.App/x86_64/stable";
inline const char* kRuntime = "runtime/org.freedesktop.Platform/x86_64/24.08";

/// A Flatpak application item. Previous version for an update or a
/// removal, target version for an install or an update.
inline PlanItemSpec flatpakApp(PlanOperation op, const char* ref = kApp, bool requested = true)
{
    PlanItemSpec item{.operation = op, .kind = PlanItemKind::Application, .requested = requested, .package = ref};
    if (op != PlanOperation::Install)
        item.fromVersion = unwrap(Version::upstream("1.0"));
    if (op != PlanOperation::Remove) {
        item.toVersion = unwrap(Version::upstream("1.1"));
        item.origin = "flathub";
        item.revision = "c0ffee01";
        item.downloadSize = 1000;
    }
    return item;
}

/// A runtime added by the package manager.
inline PlanItemSpec flatpakRuntime(PlanOperation op = PlanOperation::Install, const char* ref = kRuntime)
{
    PlanItemSpec item{.operation = op, .kind = PlanItemKind::Runtime, .requested = false, .package = ref};
    if (op != PlanOperation::Remove) {
        item.origin = "flathub";
        item.revision = "beef0001";
        item.downloadSize = 200000;
    }
    return item;
}

inline PlanItemSpec debPackage(PlanOperation op, const char* name, bool requested,
                               PlanItemKind kind = PlanItemKind::Package)
{
    PlanItemSpec item{.operation = op, .kind = kind, .requested = requested, .package = name};
    if (op != PlanOperation::Install)
        item.fromVersion = unwrap(Version::debian("1.0-1"));
    if (op != PlanOperation::Remove) {
        item.toVersion = unwrap(Version::debian("1.0-2"));
        item.origin = "debian-stable-main";
    }
    return item;
}

inline Plan makePlan(TransactionKind kind, BackendKind backend, Scope scope, std::vector<PlanItemSpec> items)
{
    return unwrap(Plan::create({.kind = kind, .backend = backend, .scope = scope, .items = std::move(items)}));
}

inline Plan flatpakPlan(TransactionKind kind, Scope scope, std::vector<PlanItemSpec> items)
{
    return makePlan(kind, BackendKind::Flatpak, scope, std::move(items));
}

inline Plan debPlan(TransactionKind kind, std::vector<PlanItemSpec> items)
{
    return makePlan(kind, BackendKind::PackageKit, Scope::System, std::move(items));
}

} // namespace planfixtures
