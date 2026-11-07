#include <cutefish/store/source.h>

#include "text.h"

namespace cutefish::store {

std::string_view toString(BackendKind kind) noexcept
{
    switch (kind) {
    case BackendKind::PackageKit: return "packagekit";
    case BackendKind::Flatpak: return "flatpak";
    }
    return "packagekit";
}

std::string_view toString(Scope scope) noexcept
{
    switch (scope) {
    case Scope::System: return "system";
    case Scope::User: return "user";
    }
    return "system";
}

std::string_view toString(SourceKind kind) noexcept
{
    switch (kind) {
    case SourceKind::Distribution: return "distribution";
    case SourceKind::Flathub: return "flathub";
    case SourceKind::ThirdParty: return "third-party";
    }
    return "third-party";
}

std::optional<BackendKind> parseBackendKind(std::string_view text) noexcept
{
    for (const BackendKind k : {BackendKind::PackageKit, BackendKind::Flatpak})
        if (toString(k) == text)
            return k;
    return std::nullopt;
}

std::optional<Scope> parseScope(std::string_view text) noexcept
{
    for (const Scope s : {Scope::System, Scope::User})
        if (toString(s) == text)
            return s;
    return std::nullopt;
}

std::optional<SourceKind> parseSourceKind(std::string_view text) noexcept
{
    for (const SourceKind k : {SourceKind::Distribution, SourceKind::Flathub, SourceKind::ThirdParty})
        if (toString(k) == text)
            return k;
    return std::nullopt;
}

Result<Source> Source::create(SourceSpec spec)
{
    if (spec.backend == BackendKind::PackageKit && spec.scope != Scope::System)
        return Error::invalidArgument("a PackageKit source is always system-wide");
    if (spec.kind == SourceKind::Flathub && spec.backend != BackendKind::Flatpak)
        return Error::invalidArgument("only a Flatpak source can be Flathub");
    if (auto ok = detail::checkOpaqueIdentifier(spec.origin, "source origin"); !ok)
        return std::move(ok).error();
    if (auto ok = detail::checkText(spec.label, "source label", 512, false); !ok)
        return std::move(ok).error();
    if (!spec.url.empty())
        if (auto ok = detail::checkAsciiToken(spec.url, "source url", 2048); !ok)
            return std::move(ok).error();
    return Source(std::move(spec));
}

} // namespace cutefish::store
