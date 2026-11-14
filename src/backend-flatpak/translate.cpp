#include "translate.h"

#include <algorithm>
#include <cstddef>

namespace cutefish::store::flatpak {

namespace {

/// Splits `s` on `/`. A ref is exactly four non-empty parts: kind, id, arch,
/// branch, and the branch has no further `/`.
bool splitRef(std::string_view ref, std::string_view out[4])
{
    std::size_t begin = 0;
    for (int part = 0; part < 3; ++part) {
        const std::size_t sep = ref.find('/', begin);
        if (sep == std::string_view::npos)
            return false;
        out[part] = ref.substr(begin, sep - begin);
        if (out[part].empty())
            return false;
        begin = sep + 1;
    }
    out[3] = ref.substr(begin);
    return !out[3].empty() && out[3].find('/') == std::string_view::npos;
}

PlanOperation planOperation(OperationType type)
{
    switch (type) {
    case OperationType::Install: return PlanOperation::Install;
    case OperationType::Update: return PlanOperation::Update;
    case OperationType::Uninstall: return PlanOperation::Remove;
    }
    return PlanOperation::Install;
}

PlanItemKind itemKind(RefKind kind)
{
    return kind == RefKind::App ? PlanItemKind::Application : PlanItemKind::Runtime;
}

} // namespace

std::string FlatpakRef::full() const
{
    std::string out;
    out.reserve(name.size() + arch.size() + branch.size() + 8);
    out += (kind == RefKind::App ? "app/" : "runtime/");
    out += name;
    out += '/';
    out += arch;
    out += '/';
    out += branch;
    return out;
}

Result<FlatpakRef> parseRef(std::string_view ref)
{
    std::string_view parts[4];
    if (!splitRef(ref, parts))
        return Error::invalidArgument("invalid Flatpak ref: " + std::string(ref));
    FlatpakRef out;
    if (parts[0] == "app")
        out.kind = RefKind::App;
    else if (parts[0] == "runtime")
        out.kind = RefKind::Runtime;
    else
        return Error::invalidArgument("unknown Flatpak ref kind: " + std::string(parts[0]));
    out.name = std::string(parts[1]);
    out.arch = std::string(parts[2]);
    out.branch = std::string(parts[3]);
    return out;
}

SourceKind remoteKind(std::string_view name, std::string_view url)
{
    (void)name; // classification is by URL host, not by name (P-011)
    // Flathub is recognised by its URL host; the name alone is not enough
    // (a third party could call a remote "flathub"). Signature key checked by
    // flatpak itself, not here.
    const auto scheme = url.find("://");
    const auto host = scheme == std::string_view::npos ? std::string_view(url)
                                                       : url.substr(scheme + 3, url.find('/', scheme + 3) - (scheme + 3));
    if (host == "dl.flathub.org" || host == "flathub.org" || host == "mirror.flathub.org")
        return SourceKind::Flathub;
    return SourceKind::ThirdParty;
}

Result<Source> sourceForRemote(Scope scope, const RemoteInfo& remote)
{
    auto id = SourceId::parse("flatpak:" + std::string(toString(scope)) + ":" + remote.name);
    if (!id)
        return std::move(id).error();
    return Source::create({.id = std::move(id).value(),
                           .backend = BackendKind::Flatpak,
                           .scope = scope,
                           .kind = remoteKind(remote.name, remote.url),
                           .origin = remote.name,
                           .url = remote.url,
                           .enabled = !remote.disabled,
                           .priority = remoteKind(remote.name, remote.url) == SourceKind::Flathub ? 20 : 10});
}

Result<std::pair<Scope, std::string>> remoteOfSource(const SourceId& source)
{
    const std::string_view id = source.str();
    constexpr std::string_view prefix = "flatpak:";
    if (!id.starts_with(prefix))
        return Error::invalidArgument("not a Flatpak source: " + std::string(id));
    const std::string_view rest = id.substr(prefix.size());
    const std::size_t colon = rest.find(':');
    if (colon == std::string_view::npos)
        return Error::invalidArgument("not a Flatpak source: " + std::string(id));
    const auto scope = parseScope(rest.substr(0, colon));
    const std::string_view name = rest.substr(colon + 1);
    if (!scope || name.empty() || name.find(':') != std::string_view::npos)
        return Error::invalidArgument("not a Flatpak source: " + std::string(id));
    return std::pair{*scope, std::string(name)};
}

Result<void> checkSignedRemotes(std::span<const FlatpakOp> ops, std::span<const RemoteInfo> remotes)
{
    for (const FlatpakOp& op : ops) {
        if (op.type == OperationType::Uninstall || op.remote.empty())
            continue;
        const auto it = std::find_if(remotes.begin(), remotes.end(),
                                     [&](const RemoteInfo& r) { return r.name == op.remote; });
        if (it != remotes.end() && !it->gpgVerify)
            return Error::backend(BackendFailure::Untrusted, "untrusted: " + op.ref.full() + " would come from the remote "
                                                                 + op.remote + ", which does not verify signatures");
    }
    return Result<void>::success();
}

std::uint32_t installedFrom(std::span<const InstalledRef> installed, std::string_view name)
{
    return static_cast<std::uint32_t>(
            std::count_if(installed.begin(), installed.end(), [&](const InstalledRef& r) { return r.origin == name; }));
}

Error mapFlatpakError(std::string_view domain, int code, std::string_view message)
{
    const std::string text(message);
    if (domain == "flatpak") {
        namespace f = codes::flatpak;
        switch (code) {
        case f::kAborted: return Error::cancelled("flatpak aborted: " + text);
        case f::kUntrusted: return Error::backend(BackendFailure::Untrusted, "untrusted: " + text);
        case f::kOutOfSpace: return Error::backend(BackendFailure::NoSpace, "out of space: " + text);
        case f::kRemoteNotFound:
        case f::kRefNotFound: return Error::notFound(text);
        case f::kRuntimeNotFound: return Error::backend(BackendFailure::Dependency, "runtime not found: " + text);
        case f::kWrongUser:
        case f::kPermissionDenied:
        case f::kAuthenticationFailed:
        case f::kNotAuthorized: return Error::backend(BackendFailure::AuthDenied, text);
        case f::kAlreadyInstalled: return Error::conflict("already installed: " + text);
        case f::kNotInstalled: return Error::notFound("not installed: " + text);
        case f::kDifferentRemote:
        case f::kDowngrade: return Error::backend(BackendFailure::PackageConflict, text);
        // Flatpak refuses to remove a remote something installed came from.
        case f::kRemoteUsed: return Error::notAllowed("remote in use: " + text);
        case f::kInvalidRef:
        case f::kInvalidName:
        case f::kInvalidData: return Error::invalidArgument(text);
        default: return Error::backend(BackendFailure::Unspecified, "flatpak error " + std::to_string(code) + ": " + text);
        }
    }
    if (domain == "g-io") {
        namespace g = codes::gio;
        switch (code) {
        case g::kCancelled: return Error::cancelled("cancelled");
        case g::kTimedOut:
        case g::kHostNotFound:
        case g::kHostUnreachable:
        case g::kNetworkUnreachable:
        case g::kConnectionRefused:
        case g::kProxyFailed:
        case g::kConnectionClosed:
        case g::kNotConnected: return Error::backend(BackendFailure::Network, "network: " + text);
        case g::kNoSpace: return Error::backend(BackendFailure::NoSpace, "out of space: " + text);
        case g::kPermissionDenied: return Error::backend(BackendFailure::AuthDenied, text);
        default: return Error::backend(BackendFailure::Unspecified, "I/O error " + std::to_string(code) + ": " + text);
        }
    }
    if (domain == "g-dbus") {
        if (code == codes::gdbus::kAccessDenied || code == codes::gdbus::kAuthFailed)
            return Error::backend(BackendFailure::AuthDenied, text);
        return Error::backend(BackendFailure::Unspecified, "D-Bus error: " + text);
    }
    return Error::backend(BackendFailure::Unspecified, text);
}

std::vector<std::string> normalisePermissions(std::vector<std::string> permissions)
{
    std::sort(permissions.begin(), permissions.end());
    permissions.erase(std::unique(permissions.begin(), permissions.end()), permissions.end());
    return permissions;
}

Result<Plan> buildPlan(TransactionKind kind, Scope scope, std::span<const FlatpakOp> ops,
                       std::span<const std::string> requestedRefs)
{
    PlanSpec spec{.kind = kind, .backend = BackendKind::Flatpak, .scope = scope, .items = {}};
    spec.items.reserve(ops.size());
    for (const FlatpakOp& op : ops) {
        PlanItemSpec item{.operation = planOperation(op.type),
                          .kind = itemKind(op.ref.kind),
                          .requested = std::find(requestedRefs.begin(), requestedRefs.end(), op.ref.full())
                                       != requestedRefs.end(),
                          .package = op.ref.full(),
                          .origin = op.remote,
                          .revision = op.commit,
                          .downloadSize = op.downloadSize,
                          .permissions = normalisePermissions(op.permissions),
                          .oldPermissions = normalisePermissions(op.oldPermissions),
                          .addsSource = op.addsSource,
                          .rebaseFrom = op.rebaseFrom,
                          .trusted = true};
        spec.items.push_back(std::move(item));
    }
    return Plan::create(std::move(spec));
}

} // namespace cutefish::store::flatpak
