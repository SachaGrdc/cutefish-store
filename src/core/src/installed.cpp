#include <cutefish/store/installed.h>

#include "text.h"

namespace cutefish::store {

Result<InstalledItem> InstalledItem::create(InstalledItemSpec spec)
{
    switch (spec.backend) {
    case BackendKind::PackageKit:
        if (spec.scope != Scope::System)
            return Error::invalidArgument("a PackageKit package is always installed system-wide");
        if (spec.version && spec.version->scheme() != VersionScheme::Debian)
            return Error::invalidArgument("a PackageKit package needs a Debian version");
        break;
    case BackendKind::Flatpak:
        if (spec.version && spec.version->scheme() != VersionScheme::Upstream)
            return Error::invalidArgument("a Flatpak ref needs an upstream version");
        break;
    }
    if (!spec.origin.empty())
        if (auto ok = detail::checkOpaqueIdentifier(spec.origin, "installed origin"); !ok)
            return std::move(ok).error();
    return InstalledItem(std::move(spec));
}

} // namespace cutefish::store
