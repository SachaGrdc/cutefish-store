#pragma once

#include <cutefish/store/identifiers.h>
#include <cutefish/store/result.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cutefish::store {

/// The software that installs an offer. v1 has two (D-003).
enum class BackendKind : std::uint8_t {
    PackageKit, ///< Distribution packages, through PackageKit (D-007).
    Flatpak,    ///< Flatpak, through libflatpak.
};

/// Installation an offer or a source belongs to (D-005, D-011).
enum class Scope : std::uint8_t {
    System, ///< System-wide: distribution packages, /var/lib/flatpak.
    User,   ///< The user's own Flatpak installation.
};

/// How the store presents a source (ARCHITECTURE §5.4, §9.3).
enum class SourceKind : std::uint8_t {
    Distribution, ///< Repository of the distribution.
    Flathub,      ///< The Flathub remote, recognised by its URL and key.
    ThirdParty,   ///< Anything else.
};

/// Stable lowercase names, as used by the D-Bus API: "packagekit",
/// "flatpak", "system", "user", "distribution", "flathub", "third-party".
std::string_view toString(BackendKind kind) noexcept;
std::string_view toString(Scope scope) noexcept;
std::string_view toString(SourceKind kind) noexcept;
std::optional<BackendKind> parseBackendKind(std::string_view text) noexcept;
std::optional<Scope> parseScope(std::string_view text) noexcept;
std::optional<SourceKind> parseSourceKind(std::string_view text) noexcept;

/// Input of Source::create. Strings are moved into the Source.
struct SourceSpec
{
    SourceId id;
    BackendKind backend;
    Scope scope;
    SourceKind kind;
    /// Where the offers come from, as the backend names it: the repository
    /// origin for PackageKit (for example `debian-stable-main`), the remote
    /// name for Flatpak (`flathub`). Required.
    std::string origin;
    std::string label = {}; ///< Display name; optional.
    std::string url = {};   ///< Optional.
    bool enabled = true;
    /// Order in which sources complete each other's missing metadata
    /// (ARCHITECTURE §6.4); higher first.
    std::int32_t priority = 0;
};

/// Where offers come from: a repository of the distribution or a Flatpak
/// remote of one installation. Read-only in v1: the core does not manage or
/// modify repositories.
class Source
{
public:
    /// Rules: PackageKit sources are system-wide; only a Flatpak source can
    /// be Flathub; origin is 1 to 255 printable ASCII bytes; label is valid
    /// UTF-8 without control characters (at most 512 bytes); url, when
    /// present, is printable ASCII (at most 2048 bytes).
    static Result<Source> create(SourceSpec spec);

    const SourceId& id() const noexcept { return m_spec.id; }
    BackendKind backend() const noexcept { return m_spec.backend; }
    Scope scope() const noexcept { return m_spec.scope; }
    SourceKind kind() const noexcept { return m_spec.kind; }
    const std::string& origin() const noexcept { return m_spec.origin; }
    const std::string& label() const noexcept { return m_spec.label; }
    const std::string& url() const noexcept { return m_spec.url; }
    bool enabled() const noexcept { return m_spec.enabled; }
    std::int32_t priority() const noexcept { return m_spec.priority; }

private:
    explicit Source(SourceSpec spec) : m_spec(std::move(spec)) {}
    SourceSpec m_spec;
};

} // namespace cutefish::store
