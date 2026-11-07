#pragma once

#include <cutefish/store/identifiers.h>
#include <cutefish/store/installed.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>

#include <optional>
#include <vector>

namespace cutefish::store {

/// What is installed on the system, as last reported by the backends.
///
/// The production implementation will read state.sqlite (ARCHITECTURE §6.3),
/// which the service fills from the backends in the background: answering
/// never waits for PackageKit or libflatpak (§7.2, P-009). Same threading
/// rules as Catalog.
class InstalledState
{
public:
    virtual ~InstalledState() = default;

    /// Everything installed, or only what one backend installed.
    virtual Result<std::vector<InstalledItem>> items(std::optional<BackendKind> backend) const = 0;

    /// The installed item for this offer, or std::nullopt if the offer is
    /// not installed. An error means the state could not be read.
    virtual Result<std::optional<InstalledItem>> find(const OfferId& offer) const = 0;

    /// Installed items that belong to this application, whatever their
    /// offer (several are possible: deb and Flatpak, user and system).
    virtual Result<std::vector<InstalledItem>> forApplication(const AppId& app) const = 0;

protected:
    InstalledState() = default;
    InstalledState(const InstalledState&) = default;
    InstalledState& operator=(const InstalledState&) = default;
};

} // namespace cutefish::store
