#include <cutefish/store/offer.h>

#include "text.h"

namespace cutefish::store {

namespace {

using detail::isAsciiAlnum;
using detail::isAsciiDigit;

bool isLowerAlnum(char c) noexcept
{
    return isAsciiDigit(c) || (c >= 'a' && c <= 'z');
}

Result<void> checkDebPackageName(std::string_view name)
{
    if (name.size() < 2 || name.size() > detail::kMaxIdentifierBytes)
        return Error::invalidArgument("deb package name must have 2 to 255 characters");
    if (!isLowerAlnum(name.front()))
        return Error::invalidArgument("deb package name must start with a lowercase letter or a digit");
    for (const char c : name)
        if (!isLowerAlnum(c) && c != '+' && c != '-' && c != '.')
            return Error::invalidArgument("deb package name contains an invalid character");
    return {};
}

Result<void> checkDebArchitecture(std::string_view arch)
{
    if (arch.empty())
        return {};
    if (arch.size() > 64)
        return Error::invalidArgument("deb architecture is too long");
    for (const char c : arch)
        if (!isLowerAlnum(c) && c != '-')
            return Error::invalidArgument("deb architecture contains an invalid character");
    return {};
}

bool isFlatpakNameCharacter(char c) noexcept
{
    return isAsciiAlnum(c) || c == '.' || c == '_' || c == '-';
}

// app|runtime/ID/arch/branch; returns the arch part.
Result<std::string_view> checkFlatpakRef(std::string_view ref)
{
    std::string_view parts[4];
    std::string_view rest = ref;
    for (int i = 0; i < 4; ++i) {
        const std::size_t slash = rest.find('/');
        if (i < 3 && slash == std::string_view::npos)
            return Error::invalidArgument("Flatpak ref must be kind/ID/arch/branch");
        parts[i] = i < 3 ? rest.substr(0, slash) : rest;
        rest = i < 3 ? rest.substr(slash + 1) : std::string_view();
    }
    if (parts[3].find('/') != std::string_view::npos)
        return Error::invalidArgument("Flatpak ref must be kind/ID/arch/branch");
    if (parts[0] != "app" && parts[0] != "runtime")
        return Error::invalidArgument("Flatpak ref kind must be app or runtime");
    // A Flatpak ID may end in ".desktop" (app/chat.delta.desktop/...), so
    // the AppId rules do not apply: only the character set and segments.
    const std::string_view name = parts[1];
    if (name.empty() || name.size() > detail::kMaxIdentifierBytes || name.front() == '.' || name.back() == '.' ||
        name.find("..") != std::string_view::npos)
        return Error::invalidArgument("Flatpak ref has an invalid ID");
    for (const char c : name)
        if (!isFlatpakNameCharacter(c))
            return Error::invalidArgument("Flatpak ref has an invalid ID");
    if (parts[2].empty())
        return Error::invalidArgument("Flatpak ref has an empty architecture");
    for (const char c : parts[2])
        if (!isLowerAlnum(c) && c != '_')
            return Error::invalidArgument("Flatpak ref architecture contains an invalid character");
    if (parts[3].empty())
        return Error::invalidArgument("Flatpak ref has an empty branch");
    for (const char c : parts[3])
        if (!isFlatpakNameCharacter(c))
            return Error::invalidArgument("Flatpak ref branch contains an invalid character");
    return parts[2];
}

} // namespace

std::string_view toString(PackageFormat format) noexcept
{
    switch (format) {
    case PackageFormat::Deb: return "deb";
    case PackageFormat::Flatpak: return "flatpak";
    }
    return "deb";
}

std::optional<PackageFormat> parsePackageFormat(std::string_view text) noexcept
{
    for (const PackageFormat f : {PackageFormat::Deb, PackageFormat::Flatpak})
        if (toString(f) == text)
            return f;
    return std::nullopt;
}

Offer::Offer(OfferId id, OfferSpec spec, const Source& source)
    : m_id(std::move(id))
    , m_spec(std::move(spec))
    , m_source(source.id())
    , m_origin(source.origin())
    , m_backend(source.backend())
    , m_scope(source.scope())
    , m_sourceKind(source.kind())
{
}

Result<Offer> Offer::create(OfferSpec spec, const Source& source)
{
    std::string id;
    switch (spec.format) {
    case PackageFormat::Deb: {
        if (source.backend() != BackendKind::PackageKit)
            return Error::invalidArgument("a deb offer must come from a PackageKit source");
        if (auto ok = checkDebPackageName(spec.package); !ok)
            return std::move(ok).error();
        if (auto ok = checkDebArchitecture(spec.architecture); !ok)
            return std::move(ok).error();
        if (spec.version && spec.version->scheme() != VersionScheme::Debian)
            return Error::invalidArgument("a deb offer needs a Debian version");
        id.reserve(11 + spec.package.size());
        id.append("packagekit:").append(spec.package);
        break;
    }
    case PackageFormat::Flatpak: {
        if (source.backend() != BackendKind::Flatpak)
            return Error::invalidArgument("a Flatpak offer must come from a Flatpak source");
        auto arch = checkFlatpakRef(spec.package);
        if (!arch)
            return std::move(arch).error();
        if (spec.architecture.empty())
            spec.architecture = std::string(*arch);
        else if (spec.architecture != *arch)
            return Error::invalidArgument("Flatpak offer architecture does not match its ref");
        if (spec.version && spec.version->scheme() != VersionScheme::Upstream)
            return Error::invalidArgument("a Flatpak offer needs an upstream version");
        const std::string_view scope = toString(source.scope());
        id.reserve(10 + scope.size() + source.origin().size() + spec.package.size());
        id.append("flatpak:").append(scope).append(":").append(source.origin()).append(":").append(spec.package);
        break;
    }
    }
    if (id.size() > detail::kMaxIdentifierBytes)
        return Error::invalidArgument("offer id would be longer than 255 bytes");
    return Offer(OfferId(std::move(id)), std::move(spec), source);
}

} // namespace cutefish::store
