#pragma once

#include <cutefish/store/identifiers.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>
#include <cutefish/store/version.h>

#include <optional>
#include <string>

namespace cutefish::store {

/// Input of InstalledItem::create.
struct InstalledItemSpec
{
    OfferId offer;
    /// The catalogue application this offer belongs to; empty when the
    /// installed package or ref has no known application (a runtime, a
    /// package installed by hand).
    std::optional<AppId> app = {};
    BackendKind backend;
    Scope scope;
    /// Installed version, when the backend reports one: Debian scheme for
    /// PackageKit, upstream scheme for Flatpak.
    std::optional<Version> version = {};
    /// Where it was installed from, as the backend reports it (for example
    /// `debian-stable-main`, `local`, `flathub`); empty when unknown.
    std::string origin = {};
};

/// Something present on the system, as reported by a backend. Mirrors a row
/// of the `installed` table of state.sqlite (ARCHITECTURE §6.3).
class InstalledItem
{
public:
    /// Rules: PackageKit items are system-wide with a Debian version;
    /// Flatpak items have an upstream version; origin, when present, is 1 to
    /// 255 printable ASCII bytes.
    static Result<InstalledItem> create(InstalledItemSpec spec);

    const OfferId& offer() const noexcept { return m_spec.offer; }
    const std::optional<AppId>& app() const noexcept { return m_spec.app; }
    BackendKind backend() const noexcept { return m_spec.backend; }
    Scope scope() const noexcept { return m_spec.scope; }
    const std::optional<Version>& version() const noexcept { return m_spec.version; }
    const std::string& origin() const noexcept { return m_spec.origin; }

private:
    explicit InstalledItem(InstalledItemSpec spec) : m_spec(std::move(spec)) {}
    InstalledItemSpec m_spec;
};

} // namespace cutefish::store
