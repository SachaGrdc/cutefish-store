#include <cutefish/store/identifiers.h>

#include "text.h"

namespace cutefish::store {

namespace {

constexpr std::string_view kDesktopSuffix = ".desktop";

bool isAppIdCharacter(char c) noexcept
{
    return detail::isAsciiAlnum(c) || c == '.' || c == '_' || c == '-' || c == '+';
}

Result<void> checkAppId(std::string_view text)
{
    if (text.empty())
        return Error::invalidArgument("application id is empty");
    if (text.size() > detail::kMaxIdentifierBytes)
        return Error::invalidArgument("application id is longer than 255 bytes");
    for (const char c : text)
        if (!isAppIdCharacter(c))
            return Error::invalidArgument("application id contains an invalid character");
    if (text.front() == '.' || text.back() == '.' || text.find("..") != std::string_view::npos)
        return Error::invalidArgument("application id has an empty segment");
    if (text.ends_with(kDesktopSuffix))
        return Error::invalidArgument("application id is not normalised (.desktop suffix)");
    return {};
}

} // namespace

Result<AppId> AppId::parse(std::string_view text)
{
    if (auto ok = checkAppId(text); !ok)
        return std::move(ok).error();
    return AppId(std::string(text));
}

Result<AppId> AppId::fromComponentId(std::string_view componentId)
{
    if (componentId.ends_with(kDesktopSuffix))
        componentId.remove_suffix(kDesktopSuffix.size());
    return parse(componentId);
}

Result<OfferId> OfferId::parse(std::string_view text)
{
    if (auto ok = detail::checkOpaqueIdentifier(text, "offer id"); !ok)
        return std::move(ok).error();
    return OfferId(std::string(text));
}

Result<SourceId> SourceId::parse(std::string_view text)
{
    if (auto ok = detail::checkOpaqueIdentifier(text, "source id"); !ok)
        return std::move(ok).error();
    return SourceId(std::string(text));
}

} // namespace cutefish::store
