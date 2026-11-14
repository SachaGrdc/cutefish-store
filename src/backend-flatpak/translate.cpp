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

std::uint32_t installedFrom(std::span<const InstalledRef> installed, std::string_view name)
{
    return static_cast<std::uint32_t>(
            std::count_if(installed.begin(), installed.end(), [&](const InstalledRef& r) { return r.origin == name; }));
}

Error mapFlatpakError(std::string_view domain, int code, std::string_view message)
{
    if (domain == "flatpak") {
        switch (code) {
        case 4:  return Error::cancelled("flatpak aborted: " + std::string(message)); // FLATPAK_ERROR_ABORTED
        case 13: return Error::backend(BackendFailure::Untrusted, "untrusted: " + std::string(message));
        case 19: return Error::backend(BackendFailure::NoSpace, "out of space: " + std::string(message));
        case 8:  // REMOTE_NOT_FOUND
        case 22: return Error::notFound(std::string(message)); // REF_NOT_FOUND
        case 9:  return Error::backend(BackendFailure::Dependency, "runtime not found: " + std::string(message));
        case 20: // WRONG_USER
        case 23: // PERMISSION_DENIED
        case 24: // AUTHENTICATION_FAILED
        case 25: return Error::backend(BackendFailure::AuthDenied, std::string(message));
        case 0:  return Error::conflict("already installed: " + std::string(message));
        case 1:  return Error::notFound("not installed: " + std::string(message));
        case 3:  // DIFFERENT_REMOTE
        case 10: return Error::backend(BackendFailure::PackageConflict, std::string(message)); // DOWNGRADE
        default: return Error::backend(BackendFailure::Unspecified, "flatpak error " + std::to_string(code)
                                                                   + ": " + std::string(message));
        }
    }
    if (domain == "g-io") {
        switch (code) {
        case 19: return Error::cancelled("cancelled"); // G_IO_ERROR_CANCELLED
        case 24: // G_IO_ERROR_TIMED_OUT
        case 1:  // G_IO_ERROR_NOT_FOUND
        case 2:  // G_IO_ERROR_EXISTS
        case 3:  // G_IO_ERROR_IS_DIRECTORY
        case 4:  // G_IO_ERROR_NOT_DIRECTORY
        case 5:  // G_IO_ERROR_NOT_EMPTY
        case 26: // G_IO_ERROR_HOST_NOT_FOUND
        case 32: // G_IO_ERROR_BROKEN_PIPE
        case 39: // G_IO_ERROR_NOT_CONNECTED
        case 44: return Error::backend(BackendFailure::Network, "network: " + std::string(message));
        case 30: // G_IO_ERROR_NO_SPACE
        case 38: return Error::backend(BackendFailure::NoSpace, "out of space: " + std::string(message));
        case 28: return Error::backend(BackendFailure::AuthDenied, std::string(message)); // G_IO_ERROR_PERMISSION_DENIED
        default: return Error::backend(BackendFailure::Unspecified, "I/O error " + std::to_string(code)
                                                                    + ": " + std::string(message));
        }
    }
    if (domain == "g-dbus") {
        if (code == 0) // G_DBUS_ERROR_ACCESS_DENIED
            return Error::backend(BackendFailure::AuthDenied, std::string(message));
        return Error::backend(BackendFailure::Unspecified, "D-Bus error: " + std::string(message));
    }
    return Error::backend(BackendFailure::Unspecified, std::string(message));
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
