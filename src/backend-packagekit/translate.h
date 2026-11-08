#pragma once

// PackageKit answers translated into the core's types (ARCHITECTURE §7.2).
// Generic: what is Debian's comes in through Profile (debianprofile.h).

#include "pkdata.h"

#include <cutefish/store/backend.h>
#include <cutefish/store/error.h>
#include <cutefish/store/installed.h>
#include <cutefish/store/plan.h>
#include <cutefish/store/result.h>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store::packagekit {

/// What the generic code needs from the distribution profile.
struct Profile
{
    /// dpkg's architecture of this machine (`amd64`).
    std::string nativeArch;
    /// Packages the store never removes, even as a consequence.
    std::function<bool(std::string_view name)> isProtected;
};

/// Success for exit `success`; otherwise the error, mapped from ErrorCode
/// (ARCHITECTURE §7.2 "Erreurs"): cancellations to ErrorCode::Cancelled,
/// the rest to ErrorCode::Backend with its BackendFailure. The PackageKit
/// code name and its text go into the message, for diagnosis.
Result<void> checkResult(const PkResult& result);

/// The phase of a running transaction (ARCHITECTURE §7.2 "Phases"); none
/// for the preparation statuses (setup, loading-cache, query...).
std::optional<OperationPhase> phaseOf(std::uint32_t status) noexcept;

/// Progress for the core. `totalBytes` is the download size the plan
/// announced (0 when unknown).
Progress progressOf(const PkProgress& p, std::uint64_t totalBytes);

/// GetPackages(installed;arch): one item per package, for the native
/// architecture and `all` (foreign ones are left out: their offer id would
/// not be a deb package name). Duplicate ids are merged.
Result<std::vector<InstalledItem>> installedItems(const PkResult& result, const Profile& profile);

/// GetUpdates: one update per package, without the entries whose version is
/// the installed one (D-010, prototype 4); `fromVersion` from `installed`.
/// Not yet filtered by the catalogue: applicationUpdates() does it.
Result<std::vector<AvailableUpdate>> availableUpdates(const PkResult& result, std::span<const InstalledItem> installed,
                                                      const Profile& profile);

/// The package ids for the request's packages among the result of a
/// Resolve or GetUpdates: native architecture first, then `all`, the newest
/// version listed last wins. NotFound for a requested package not there.
Result<std::vector<std::string>> idsFor(const PlanRequest& request, const PkResult& result, const Profile& profile);

/// The plan of a simulation (InstallPackages, RemovePackages or
/// UpdatePackages with SIMULATE):
/// - one item per package of the Packages signals, named as dpkg names it
///   (`name`, or `name:arch` for a foreign architecture);
/// - requested: the packages of the request; everything else was added by
///   APT (dependencies, cascaded removals);
/// - removing and obsoleting are removals, downgrading a downgrade; any
///   other information is refused (unclassifiable, P-008);
/// - a removal of a protected package, requested or not, is refused;
/// - no item at all (nothing to do) is a Conflict.
/// The plan's download size is not split per package by PackageKit: items
/// carry 0 and the total is returned in `downloadSize`.
struct SimulatedPlan
{
    Plan plan;
    std::uint64_t downloadSize = 0;
};
Result<SimulatedPlan> planFromSimulation(const PlanRequest& request, const PkResult& result, const Profile& profile);

} // namespace cutefish::store::packagekit
