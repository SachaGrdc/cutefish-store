#include "translate.h"

#include "packageid.h"
#include "pkenums.h"

#include <algorithm>
#include <map>
#include <set>

namespace cutefish::store::packagekit {

namespace {

BackendFailure failureOf(std::string_view code)
{
    static const std::map<std::string_view, BackendFailure> kMap{
        {"not-authorized", BackendFailure::AuthDenied},
        {"cannot-get-lock", BackendFailure::Locked},
        {"lock-required", BackendFailure::Locked},
        {"no-network", BackendFailure::Network},
        {"package-download-failed", BackendFailure::Network},
        {"no-more-mirrors-to-try", BackendFailure::Network},
        {"cannot-fetch-sources", BackendFailure::Network},
        {"gpg-failure", BackendFailure::Untrusted},
        {"bad-gpg-signature", BackendFailure::Untrusted},
        {"missing-gpg-signature", BackendFailure::Untrusted},
        {"cannot-install-repo-unsigned", BackendFailure::Untrusted},
        {"cannot-update-repo-unsigned", BackendFailure::Untrusted},
        {"dep-resolution-failed", BackendFailure::Dependency},
        {"file-conflicts", BackendFailure::PackageConflict},
        {"package-conflicts", BackendFailure::PackageConflict},
        {"no-space-on-device", BackendFailure::NoSpace},
    };
    const auto it = kMap.find(code);
    return it == kMap.end() ? BackendFailure::Unspecified : it->second;
}

Result<Version> debianVersion(const PackageId& id)
{
    auto v = Version::debian(id.version);
    if (!v)
        return Error::backend(BackendFailure::Unspecified, "unreadable version in " + id.str());
    return v;
}

bool nativeOrAll(const PackageId& id, const Profile& p)
{
    return id.arch == "all" || id.arch == p.nativeArch;
}

Result<OfferId> offerOf(const PackageId& id)
{
    auto o = OfferId::parse("packagekit:" + id.name);
    if (!o)
        return std::move(o).error();
    return o;
}

} // namespace

Result<void> checkResult(const PkResult& r)
{
    if (!r.dbusError.empty()) {
        if (r.dbusError == "org.freedesktop.DBus.Error.ServiceUnknown"
            || r.dbusError == "org.freedesktop.DBus.Error.NameHasNoOwner"
            || r.dbusError == "org.freedesktop.DBus.Error.NoReply"
            || r.dbusError == "org.freedesktop.DBus.Error.Disconnected")
            return Error::backend(BackendFailure::Unavailable, "PackageKit: " + r.dbusError + ": " + r.dbusErrorMessage);
        return Error::backend(BackendFailure::Unspecified, "PackageKit: " + r.dbusError + ": " + r.dbusErrorMessage);
    }
    const auto exit = static_cast<Exit>(r.exit);
    if (exit == Exit::Success)
        return {};
    const std::string code = r.errorCode ? std::string(errorName(*r.errorCode)) : std::string();
    const std::string text = "PackageKit " + std::string(exitName(r.exit)) + (code.empty() ? "" : ", " + code)
            + (r.errorDetails.empty() ? "" : ": " + r.errorDetails);
    if (exit == Exit::Cancelled || exit == Exit::CancelledPriority || code == "transaction-cancelled"
        || code == "cancelled-priority")
        return Error::cancelled(text);
    if (exit == Exit::Killed)
        return Error::backend(BackendFailure::Interrupted, text);
    if (code == "package-not-found" || code == "package-id-invalid")
        return Error::notFound(text);
    return Error::backend(failureOf(code), text);
}

std::optional<OperationPhase> phaseOf(std::uint32_t status) noexcept
{
    switch (static_cast<Status>(status)) {
    case Status::WaitingForAuth: return OperationPhase::WaitingForAuth;
    case Status::WaitingForLock: return OperationPhase::WaitingForLock;
    case Status::Download:
    case Status::DownloadChangelog:
    case Status::DownloadUpdateinfo:
    case Status::DownloadRepository:
    case Status::DownloadPackagelist:
    case Status::DownloadFilelist:
    case Status::DownloadGroup:
    case Status::RefreshCache:
        return OperationPhase::Downloading;
    case Status::Install:
    case Status::Update:
    case Status::Commit:
        return OperationPhase::Installing;
    case Status::Remove:
    case Status::Obsolete:
        return OperationPhase::Removing;
    case Status::Cleanup:
    case Status::Cancel:
    case Status::Finished:
        return OperationPhase::Finishing;
    default:
        return std::nullopt;
    }
}

Progress progressOf(const PkProgress& p, std::uint64_t totalBytes)
{
    Progress out;
    out.phase = phaseOf(p.status);
    out.fraction = p.percentage <= 100 ? p.percentage / 100.0 : -1.0;
    out.totalBytes = totalBytes;
    out.downloadedBytes = totalBytes > p.downloadSizeRemaining ? totalBytes - p.downloadSizeRemaining : 0;
    out.cancellable = p.allowCancel;
    if (!p.item.empty()) {
        if (auto id = parsePackageId(p.item))
            out.item = id->name;
        else
            out.item = p.item;
    }
    return out;
}

Result<std::vector<InstalledItem>> installedItems(const PkResult& result, const Profile& profile)
{
    if (auto ok = checkResult(result); !ok)
        return std::move(ok).error();
    std::vector<InstalledItem> out;
    std::set<std::string> seen;
    for (const PkPackage& p : result.packages) {
        auto id = parsePackageId(p.id);
        if (!id)
            return std::move(id).error();
        if (!nativeOrAll(*id, profile) || !seen.insert(id->name).second)
            continue;
        auto offer = offerOf(*id);
        auto version = debianVersion(*id);
        if (!offer || !version)
            continue; // not a deb name the core accepts: nothing to link it to
        const std::string origin = id->origin();
        auto item = InstalledItem::create({.offer = std::move(offer).value(),
                                           .app = std::nullopt,
                                           .backend = BackendKind::PackageKit,
                                           .scope = Scope::System,
                                           .version = std::move(version).value(),
                                           .origin = origin});
        if (item)
            out.push_back(std::move(item).value());
    }
    return out;
}

Result<std::vector<AvailableUpdate>> availableUpdates(const PkResult& result, std::span<const InstalledItem> installed,
                                                      const Profile& profile)
{
    if (auto ok = checkResult(result); !ok)
        return std::move(ok).error();
    std::map<OfferId, const InstalledItem*> byOffer;
    for (const InstalledItem& i : installed)
        byOffer.emplace(i.offer(), &i);
    std::vector<AvailableUpdate> out;
    std::set<std::string> seen;
    for (const PkPackage& p : result.packages) {
        auto id = parsePackageId(p.id);
        if (!id)
            return std::move(id).error();
        if (!nativeOrAll(*id, profile) || !seen.insert(id->name).second)
            continue;
        auto offer = offerOf(*id);
        auto to = debianVersion(*id);
        if (!offer || !to)
            continue;
        AvailableUpdate u{.offer = std::move(offer).value(),
                          .app = std::nullopt,
                          .kind = PlanItemKind::Package,
                          .fromVersion = std::nullopt,
                          .toVersion = std::move(to).value(),
                          .downloadSize = 0};
        if (const auto it = byOffer.find(u.offer); it != byOffer.end() && it->second->version()) {
            // Listed by GetUpdates at the installed version: not an update.
            if (it->second->version()->str() == u.toVersion->str())
                continue;
            u.fromVersion = it->second->version();
        }
        out.push_back(std::move(u));
    }
    return out;
}

Result<std::vector<std::string>> idsFor(const PlanRequest& request, const PkResult& result, const Profile& profile)
{
    if (auto ok = checkResult(result); !ok)
        return std::move(ok).error();
    std::map<std::string, std::pair<int, std::string>> best; // name -> (rank, id)
    for (const PkPackage& p : result.packages) {
        auto id = parsePackageId(p.id);
        if (!id)
            return std::move(id).error();
        const int rank = id->arch == profile.nativeArch ? 2 : id->arch == "all" ? 1 : 0;
        if (rank == 0)
            continue;
        auto& slot = best[id->name];
        if (rank >= slot.first)
            slot = {rank, p.id};
    }
    std::vector<std::string> out;
    for (const OfferAddress& t : request.targets()) {
        const auto it = best.find(t.package);
        if (it == best.end())
            return Error::notFound("PackageKit does not know package " + t.package);
        out.push_back(it->second.second);
    }
    return out;
}

Result<SimulatedPlan> planFromSimulation(const PlanRequest& request, const PkResult& result, const Profile& profile)
{
    if (auto ok = checkResult(result); !ok)
        return std::move(ok).error();
    std::set<std::string> requested;
    for (const OfferAddress& t : request.targets())
        requested.insert(t.package);

    PlanSpec spec{.kind = request.kind(), .backend = BackendKind::PackageKit, .scope = Scope::System, .items = {}};
    std::set<std::string> seenIds;
    for (const PkPackage& p : result.packages) {
        if (!seenIds.insert(p.id).second)
            continue; // the same package in two signals
        auto id = parsePackageId(p.id);
        if (!id)
            return std::move(id).error();
        PlanOperation op;
        switch (infoOf(p.info)) {
        case Info::Installing:
        case Info::Install:
            op = PlanOperation::Install;
            break;
        case Info::Updating:
        case Info::Reinstalling:
            op = PlanOperation::Update;
            break;
        case Info::Downgrading:
        case Info::Downgrade:
            op = PlanOperation::Downgrade;
            break;
        case Info::Removing:
        case Info::Remove:
        case Info::Obsoleting:
        case Info::Obsolete:
            op = PlanOperation::Remove;
            break;
        default:
            return Error::backend(BackendFailure::Unspecified,
                                  "unexpected information " + std::to_string(p.info) + " for " + p.id
                                          + " in a simulation");
        }
        const std::string name = id->qualifiedName(profile.nativeArch);
        if (op == PlanOperation::Remove && profile.isProtected && profile.isProtected(name))
            return Error::invalidArgument("the plan would remove " + name + ", which the store protects");
        const bool isRequested = requested.contains(name);
        PlanItemSpec item{.operation = op,
                          .kind = isRequested ? PlanItemKind::Application : PlanItemKind::Package,
                          .requested = isRequested,
                          .package = name};
        auto version = debianVersion(*id);
        if (!version)
            return std::move(version).error();
        if (op == PlanOperation::Remove)
            item.fromVersion = std::move(version).value(); // the version being removed
        else
            item.toVersion = std::move(version).value();
        const std::string origin = id->origin();
        if (op != PlanOperation::Remove)
            item.origin = origin;
        spec.items.push_back(std::move(item));
    }
    if (spec.items.empty())
        return Error::conflict("nothing to do: PackageKit planned no change");
    // The total only: PackageKit does not split it per package.
    spec.downloadSize = result.downloadSizeRemaining;
    auto plan = Plan::create(std::move(spec));
    if (!plan)
        return Error::backend(BackendFailure::Unspecified, "PackageKit plan refused: " + plan.error().message());
    return SimulatedPlan{std::move(plan).value(), result.downloadSizeRemaining};
}

} // namespace cutefish::store::packagekit
