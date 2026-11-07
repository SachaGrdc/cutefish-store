#pragma once

#include <cutefish/store/identifiers.h>
#include <cutefish/store/offer.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store {

/// A block of an application description. The AppStream markup is turned
/// into these blocks before it reaches the core; no HTML is ever kept
/// (ARCHITECTURE §5.4, §9.5).
enum class DescriptionBlockType : std::uint8_t {
    Paragraph,       ///< `p`
    ListItem,        ///< `li` of an unordered list
    OrderedListItem, ///< `li` of an ordered list
};

struct DescriptionBlock
{
    DescriptionBlockType type;
    std::string text;

    bool operator==(const DescriptionBlock&) const = default;
};

enum class IconKind : std::uint8_t {
    None,
    LocalPath, ///< Absolute path to a cached icon of a catalogue (§5.8).
    RemoteUrl, ///< `https://` URL, fetched by the service's image cache.
    StockName, ///< Icon theme name.
};

struct IconRef
{
    IconKind kind = IconKind::None;
    std::string value;

    bool operator==(const IconRef&) const = default;
};

/// Input of Application::create. Everything is moved into the Application.
struct ApplicationSpec
{
    AppId id;
    std::string name;
    std::string summary = {};
    std::vector<DescriptionBlock> description = {};
    IconRef icon = {};
    /// AppStream categories (`AudioVideo`, `X-GNOME-Utilities`), in order.
    std::vector<std::string> categories = {};
    /// Other ids under which the application is known: legacy `.desktop`
    /// form, `<provides><id>` (ARCHITECTURE §6.4).
    std::vector<AppId> aliases = {};
    /// At least one. Their order is kept (source priority, from the indexer).
    std::vector<Offer> offers = {};
};

/// The product shown by the store. It is not a package: it can be installed
/// through any of its offers, from different sources and formats.
class Application
{
public:
    /// Rules: name required (at most 512 bytes), summary at most 1024 bytes,
    /// both valid UTF-8 without control characters; description blocks
    /// non-empty (newlines allowed); icon consistent with its kind;
    /// categories and aliases without duplicates, aliases different from
    /// the id; at least one offer, offer ids unique.
    static Result<Application> create(ApplicationSpec spec);

    const AppId& id() const noexcept { return m_spec.id; }
    const std::string& name() const noexcept { return m_spec.name; }
    const std::string& summary() const noexcept { return m_spec.summary; }
    std::span<const DescriptionBlock> description() const noexcept { return m_spec.description; }
    const IconRef& icon() const noexcept { return m_spec.icon; }
    std::span<const std::string> categories() const noexcept { return m_spec.categories; }
    std::span<const AppId> aliases() const noexcept { return m_spec.aliases; }
    std::span<const Offer> offers() const noexcept { return m_spec.offers; }

    /// nullptr if the application has no such offer.
    const Offer* findOffer(const OfferId& id) const noexcept;
    /// True if `id` is the application id or one of its aliases.
    bool isKnownAs(const AppId& id) const noexcept;
    bool hasCategory(std::string_view category) const noexcept;
    bool hasBackend(BackendKind backend) const noexcept;

private:
    explicit Application(ApplicationSpec spec) : m_spec(std::move(spec)) {}
    ApplicationSpec m_spec;
};

} // namespace cutefish::store
