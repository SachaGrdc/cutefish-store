#pragma once

#include <cutefish/store/result.h>

#include <compare>
#include <string>
#include <string_view>

namespace cutefish::store {

/// Identifier of an application: the AppStream component id, normalised
/// (ARCHITECTURE §6.4). Case-sensitive.
///
/// Accepted form, checked against the Debian and Flathub catalogues: 1 to 255
/// bytes of ASCII letters, digits and `.`, `_`, `-`, `+`, without an empty
/// dot-separated segment, and never ending in `.desktop` (that legacy suffix
/// is removed by fromComponentId and kept by the caller as an alias).
class AppId
{
public:
    /// Strict parsing of an already normalised id (for example from D-Bus).
    static Result<AppId> parse(std::string_view text);

    /// Normalises an id read from a catalogue: removes a trailing `.desktop`,
    /// then validates. `org.gnome.Gedit.desktop` gives `org.gnome.Gedit`.
    static Result<AppId> fromComponentId(std::string_view componentId);

    const std::string& str() const noexcept { return m_value; }

    bool operator==(const AppId&) const = default;
    auto operator<=>(const AppId&) const = default;

private:
    explicit AppId(std::string value) : m_value(std::move(value)) {}
    std::string m_value;
};

/// Opaque identifier of an offer, built by the core from the offer itself
/// (see Offer). Clients never build or interpret it (ARCHITECTURE §5.1);
/// parse() only checks that a received value is well formed.
class OfferId
{
public:
    /// 1 to 255 bytes of printable ASCII without spaces.
    static Result<OfferId> parse(std::string_view text);

    const std::string& str() const noexcept { return m_value; }

    bool operator==(const OfferId&) const = default;
    auto operator<=>(const OfferId&) const = default;

private:
    friend class Offer;
    explicit OfferId(std::string value) : m_value(std::move(value)) {}
    std::string m_value;
};

/// Opaque identifier of a source (a repository or a Flatpak remote of one
/// installation). Same syntax as OfferId.
class SourceId
{
public:
    static Result<SourceId> parse(std::string_view text);

    const std::string& str() const noexcept { return m_value; }

    bool operator==(const SourceId&) const = default;
    auto operator<=>(const SourceId&) const = default;

private:
    explicit SourceId(std::string value) : m_value(std::move(value)) {}
    std::string m_value;
};

} // namespace cutefish::store
