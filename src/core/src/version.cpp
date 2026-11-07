#include <cutefish/store/version.h>

#include "text.h"

#include <limits>

namespace cutefish::store {

namespace {

using detail::isAsciiAlnum;
using detail::isAsciiAlpha;
using detail::isAsciiDigit;

// dpkg's verrevcmp() (lib/dpkg/version.c). Characters past the end read as
// '\0'. Letters sort before non-letters, '~' before everything, including
// the end of the string; runs of digits compare numerically.
int orderOf(char c) noexcept
{
    if (isAsciiDigit(c))
        return 0;
    if (isAsciiAlpha(c))
        return static_cast<unsigned char>(c);
    if (c == '~')
        return -1;
    if (c)
        return static_cast<unsigned char>(c) + 256;
    return 0;
}

int compareFragments(std::string_view a, std::string_view b) noexcept
{
    std::size_t i = 0;
    std::size_t j = 0;
    auto at = [](std::string_view s, std::size_t k) { return k < s.size() ? s[k] : '\0'; };
    while (i < a.size() || j < b.size()) {
        int firstDiff = 0;
        while ((i < a.size() && !isAsciiDigit(a[i])) || (j < b.size() && !isAsciiDigit(b[j]))) {
            const int ac = orderOf(at(a, i));
            const int bc = orderOf(at(b, j));
            if (ac != bc)
                return ac - bc;
            ++i;
            ++j;
        }
        while (at(a, i) == '0')
            ++i;
        while (at(b, j) == '0')
            ++j;
        while (isAsciiDigit(at(a, i)) && isAsciiDigit(at(b, j))) {
            if (!firstDiff)
                firstDiff = at(a, i) - at(b, j);
            ++i;
            ++j;
        }
        if (isAsciiDigit(at(a, i)))
            return 1;
        if (isAsciiDigit(at(b, j)))
            return -1;
        if (firstDiff)
            return firstDiff;
    }
    return 0;
}

std::weak_ordering toOrdering(int c) noexcept
{
    return c < 0 ? std::weak_ordering::less : c > 0 ? std::weak_ordering::greater : std::weak_ordering::equivalent;
}

bool isUpstreamCharacter(char c) noexcept
{
    return isAsciiAlnum(c) || c == '.' || c == '+' || c == '~' || c == '-' || c == ':';
}

bool isRevisionCharacter(char c) noexcept
{
    return isAsciiAlnum(c) || c == '.' || c == '+' || c == '~';
}

} // namespace

std::string_view toString(VersionScheme scheme) noexcept
{
    switch (scheme) {
    case VersionScheme::Debian: return "debian";
    case VersionScheme::Upstream: return "upstream";
    }
    return "upstream";
}

std::string_view Version::upstreamPart() const noexcept
{
    return std::string_view(m_text).substr(m_upstreamBegin, m_upstreamEnd - m_upstreamBegin);
}

std::string_view Version::revision() const noexcept
{
    if (m_revisionBegin == 0)
        return {};
    return std::string_view(m_text).substr(m_revisionBegin);
}

Result<Version> Version::debian(std::string_view text)
{
    if (text.empty())
        return Error::invalidArgument("Debian version is empty");
    if (text.size() > detail::kMaxIdentifierBytes)
        return Error::invalidArgument("Debian version is longer than 255 bytes");

    Version v(VersionScheme::Debian, std::string(text));
    std::size_t begin = 0;
    if (const std::size_t colon = text.find(':'); colon != std::string_view::npos) {
        if (colon == 0)
            return Error::invalidArgument("Debian version has an empty epoch");
        std::uint64_t epoch = 0;
        for (std::size_t k = 0; k < colon; ++k) {
            if (!isAsciiDigit(text[k]))
                return Error::invalidArgument("Debian version epoch is not a number");
            epoch = epoch * 10 + static_cast<std::uint64_t>(text[k] - '0');
            if (epoch > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
                return Error::invalidArgument("Debian version epoch is too big");
        }
        v.m_epoch = static_cast<std::uint32_t>(epoch);
        begin = colon + 1;
    }
    std::size_t upstreamEnd = text.size();
    if (const std::size_t hyphen = text.rfind('-'); hyphen != std::string_view::npos && hyphen >= begin) {
        if (hyphen + 1 == text.size())
            return Error::invalidArgument("Debian version has an empty revision");
        for (std::size_t k = hyphen + 1; k < text.size(); ++k)
            if (!isRevisionCharacter(text[k]))
                return Error::invalidArgument("Debian revision contains an invalid character");
        upstreamEnd = hyphen;
        v.m_revisionBegin = static_cast<std::uint16_t>(hyphen + 1);
    }
    if (begin == upstreamEnd)
        return Error::invalidArgument("Debian version has an empty upstream part");
    if (!isAsciiDigit(text[begin]))
        return Error::invalidArgument("Debian upstream version does not start with a digit");
    for (std::size_t k = begin; k < upstreamEnd; ++k)
        if (!isUpstreamCharacter(text[k]))
            return Error::invalidArgument("Debian upstream version contains an invalid character");
    v.m_upstreamBegin = static_cast<std::uint16_t>(begin);
    v.m_upstreamEnd = static_cast<std::uint16_t>(upstreamEnd);
    return v;
}

Result<Version> Version::upstream(std::string_view text)
{
    if (auto ok = detail::checkText(text, "upstream version", detail::kMaxIdentifierBytes, true); !ok)
        return std::move(ok).error();
    Version v(VersionScheme::Upstream, std::string(text));
    v.m_upstreamEnd = static_cast<std::uint16_t>(text.size());
    return v;
}

Result<std::weak_ordering> compare(const Version& a, const Version& b)
{
    if (a.scheme() != b.scheme())
        return Error::notSupported("versions of different schemes cannot be ordered");
    if (a.scheme() != VersionScheme::Debian)
        return Error::notSupported("upstream versions have no reliable ordering");
    if (a.epoch() != b.epoch())
        return a.epoch() < b.epoch() ? std::weak_ordering::less : std::weak_ordering::greater;
    if (const int c = compareFragments(a.upstreamPart(), b.upstreamPart()))
        return toOrdering(c);
    return toOrdering(compareFragments(a.revision(), b.revision()));
}

} // namespace cutefish::store
