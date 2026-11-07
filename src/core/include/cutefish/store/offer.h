#pragma once

#include <cutefish/store/identifiers.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>
#include <cutefish/store/version.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cutefish::store {

/// Package format of an offer. v1 handles DEB/APT and Flatpak only (D-003);
/// a format is added together with the backend that installs it.
enum class PackageFormat : std::uint8_t {
    Deb,     ///< Debian package, installed through PackageKit (Debian profile).
    Flatpak, ///< Flatpak ref.
};

/// "deb", "flatpak".
std::string_view toString(PackageFormat format) noexcept;
std::optional<PackageFormat> parsePackageFormat(std::string_view text) noexcept;

/// Input of Offer::create.
struct OfferSpec
{
    PackageFormat format;
    /// What the backend installs: the package name for a deb offer
    /// (`firefox-esr`), the full ref for a Flatpak offer
    /// (`app/org.mozilla.firefox/x86_64/stable`).
    std::string package;
    /// Version offered, if known. Debian scheme for a deb offer, upstream
    /// scheme for a Flatpak offer.
    std::optional<Version> version = {};
    /// Architecture, if known. Optional for a deb offer (PackageKit resolves
    /// the native one when planning); for a Flatpak offer it is taken from
    /// the ref, and must match it when given.
    std::string architecture = {};
};

/// One concrete way to install an application: a package of a given format,
/// from a given source. An application usually has several (D-009).
///
/// The offer keeps its provenance (source id, source kind, origin, scope)
/// so that it can be shown and chosen without looking the source up again.
class Offer
{
public:
    /// Validates the spec against the source it comes from:
    /// - a PackageKit source gives deb offers, a Flatpak source Flatpak
    ///   offers;
    /// - a deb package name follows Debian policy (lowercase letters,
    ///   digits, `+`, `-`, `.`, at least two characters, starting with a
    ///   letter or digit);
    /// - a Flatpak ref is `app|runtime/ID/arch/branch`;
    /// - the version scheme matches the format.
    ///
    /// The offer id is built here (ARCHITECTURE §6.4):
    /// `packagekit:<package>` or `flatpak:<scope>:<remote>:<ref>`.
    static Result<Offer> create(OfferSpec spec, const Source& source);

    const OfferId& id() const noexcept { return m_id; }
    BackendKind backend() const noexcept { return m_backend; }
    PackageFormat format() const noexcept { return m_spec.format; }
    Scope scope() const noexcept { return m_scope; }
    const SourceId& source() const noexcept { return m_source; }
    SourceKind sourceKind() const noexcept { return m_sourceKind; }
    /// Repository origin or Flatpak remote name, copied from the source.
    const std::string& origin() const noexcept { return m_origin; }
    const std::string& package() const noexcept { return m_spec.package; }
    const std::optional<Version>& version() const noexcept { return m_spec.version; }
    /// Empty when unknown (deb offer before planning).
    const std::string& architecture() const noexcept { return m_spec.architecture; }

private:
    Offer(OfferId id, OfferSpec spec, const Source& source);

    OfferId m_id;
    OfferSpec m_spec;
    SourceId m_source;
    std::string m_origin;
    BackendKind m_backend;
    Scope m_scope;
    SourceKind m_sourceKind;
};

} // namespace cutefish::store
