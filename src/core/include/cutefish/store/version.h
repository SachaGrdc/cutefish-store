#pragma once

#include <cutefish/store/result.h>

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace cutefish::store {

/// How a version string is to be read. A version never travels without its
/// scheme, so that two versions are compared only when a reliable rule exists.
enum class VersionScheme : std::uint8_t {
    /// Debian package version, `[epoch:]upstream[-revision]`, ordered with the
    /// rules of dpkg. Used for packages handled by PackageKit (profile Debian).
    Debian,
    /// Version declared by the upstream project (AppStream `<release>`, the
    /// version shown for a Flatpak offer). Displayed only: there is no rule
    /// that orders these strings reliably, and Flatpak updates are decided
    /// by commits, not by versions.
    Upstream,
};

std::string_view toString(VersionScheme scheme) noexcept;

/// A version string with its scheme. Immutable once created.
class Version
{
public:
    /// Strict dpkg syntax. What dpkg only warns about (an upstream part that
    /// does not start with a digit, an invalid character) is rejected here,
    /// so that a version is either ordered correctly or not accepted.
    static Result<Version> debian(std::string_view text);

    /// Any non-empty text of at most 255 bytes, valid UTF-8, without control
    /// characters.
    static Result<Version> upstream(std::string_view text);

    VersionScheme scheme() const noexcept { return m_scheme; }
    const std::string& str() const noexcept { return m_text; }

    /// Debian only: the epoch (0 when absent), the upstream part and the
    /// revision (empty when absent). For an upstream version, epoch() is 0,
    /// upstreamPart() is the whole text and revision() is empty.
    std::uint32_t epoch() const noexcept { return m_epoch; }
    std::string_view upstreamPart() const noexcept;
    std::string_view revision() const noexcept;

    /// Identity, not ordering: same scheme and same text. `1.0` and `0:1.0`
    /// are different Version values that compare() orders as equivalent.
    bool operator==(const Version& other) const noexcept
    {
        return m_scheme == other.m_scheme && m_text == other.m_text;
    }

private:
    Version(VersionScheme scheme, std::string text) : m_scheme(scheme), m_text(std::move(text)) {}

    VersionScheme m_scheme;
    std::string m_text;
    std::uint32_t m_epoch = 0;
    std::uint16_t m_upstreamBegin = 0; // offsets in m_text (text is at most 255 bytes)
    std::uint16_t m_upstreamEnd = 0;
    std::uint16_t m_revisionBegin = 0;
};

/// Orders two versions when, and only when, a reliable rule exists:
/// - two Debian versions: the dpkg order (epoch, then upstream part, then
///   revision, `~` sorting before everything, even the end of the string);
/// - anything else, including two upstream versions or versions of
///   different schemes: ErrorCode::NotSupported. The caller must then show
///   both versions without claiming one is newer (D-009).
Result<std::weak_ordering> compare(const Version& a, const Version& b);

} // namespace cutefish::store
