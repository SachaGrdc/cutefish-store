#pragma once

#include <cutefish/store/application.h>
#include <cutefish/store/identifiers.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cutefish::store {

/// Limits of a search request (ARCHITECTURE §5.2, §9.4).
inline constexpr std::size_t kMaxSearchTextBytes = 256;
inline constexpr std::uint32_t kDefaultSearchLimit = 60;
inline constexpr std::uint32_t kMaxSearchLimit = 200;

/// A small set of backends, stored as bits.
class BackendSet
{
public:
    constexpr BackendSet() noexcept = default;

    constexpr void insert(BackendKind kind) noexcept { m_bits |= bit(kind); }
    constexpr bool contains(BackendKind kind) const noexcept { return (m_bits & bit(kind)) != 0; }
    constexpr bool empty() const noexcept { return m_bits == 0; }
    constexpr bool operator==(const BackendSet&) const noexcept = default;

private:
    static constexpr std::uint8_t bit(BackendKind kind) noexcept
    {
        return static_cast<std::uint8_t>(1u << static_cast<unsigned>(kind));
    }
    std::uint8_t m_bits = 0;
};

struct SearchOptions
{
    std::uint32_t limit = kDefaultSearchLimit; ///< 1 to kMaxSearchLimit.
    std::uint32_t offset = 0;
    std::string category = {};                 ///< Empty: all categories.
    std::optional<BackendKind> backend = {};   ///< Only applications with an offer of this backend.
    bool installedOnly = false;
};

/// A validated search request. The text is what the user typed: it is never
/// passed to SQLite as query syntax (ARCHITECTURE §6.3).
class SearchQuery
{
public:
    /// Rules: text valid UTF-8, at most 256 bytes, without control
    /// characters (empty text is accepted and finds nothing); limit 1 to
    /// 200; category, when given, a valid category name.
    static Result<SearchQuery> create(std::string text, SearchOptions options = {});

    const std::string& text() const noexcept { return m_text; }
    const SearchOptions& options() const noexcept { return m_options; }

private:
    SearchQuery(std::string text, SearchOptions options) : m_text(std::move(text)), m_options(std::move(options)) {}
    std::string m_text;
    SearchOptions m_options;
};

/// What a list row needs, without a second round trip (ARCHITECTURE §5.4).
struct AppSummary
{
    AppId id;
    std::string name = {};
    std::string summary = {};
    IconRef icon = {};
    BackendSet backends = {};
    bool installed = false;
    bool hasUpdate = false;
    bool verified = false;
    std::string developer = {};
};

/// One page of results, tied to the catalogue generation that produced it.
struct SearchPage
{
    std::uint64_t generation = 0;
    std::uint32_t total = 0; ///< Matches in the whole catalogue, not only this page.
    std::vector<AppSummary> items = {};
};

} // namespace cutefish::store
