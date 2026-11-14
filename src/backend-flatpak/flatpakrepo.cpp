#include "flatpakrepo.h"

#include <algorithm>
#include <optional>

namespace cutefish::store::flatpak {

namespace {

std::string_view trimmed(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

} // namespace

Result<FlatpakRepoFile> checkFlatpakRepo(std::string_view text)
{
    constexpr std::size_t kMaxSize = 64 * 1024;
    if (text.size() > kMaxSize)
        return Error::invalidArgument("a .flatpakrepo of more than 64 KiB");
    bool inGroup = false;
    bool sawGroup = false;
    std::optional<std::string> url;
    bool key = false;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = trimmed(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[') {
            inGroup = line == "[Flatpak Repo]";
            sawGroup = sawGroup || inGroup;
            continue;
        }
        if (!inGroup)
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view name = trimmed(line.substr(0, eq));
        const std::string_view value = trimmed(line.substr(eq + 1));
        if (name == "Url")
            url = std::string(value);
        else if (name == "GPGKey")
            key = !value.empty();
    }
    if (!sawGroup)
        return Error::invalidArgument("not a .flatpakrepo: no [Flatpak Repo] group");
    if (!url || !url->starts_with("https://") || url->size() <= std::string_view("https://").size())
        return Error::invalidArgument("a .flatpakrepo whose Url is not https");
    if (!key)
        return Error::backend(BackendFailure::Untrusted, "untrusted: the .flatpakrepo has no GPGKey");
    return FlatpakRepoFile{.url = std::move(*url)};
}

Result<void> checkRemoteName(std::string_view name)
{
    auto allowed = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_'
               || c == '-';
    };
    if (name.empty() || name.size() > 64 || name.front() == '.' || name.front() == '-'
        || !std::all_of(name.begin(), name.end(), allowed))
        return Error::invalidArgument("not a remote name the store creates: " + std::string(name));
    return Result<void>::success();
}

} // namespace cutefish::store::flatpak
