#pragma once

#include <filesystem>
#include <string>

namespace cutefish::store::detail {

/// `file:` URI opening `path` read-only; with `immutable`, SQLite takes no
/// lock and never looks for a journal: only valid for a file that nobody
/// modifies in place, which is the case of a catalogue snapshot.
inline std::string readOnlyUri(const std::filesystem::path& path, bool immutable)
{
    std::string uri = "file:";
    for (const char c : path.string()) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '%' || c == '?' || c == '#' || u < 0x21 || u >= 0x7F) {
            static constexpr char hex[] = "0123456789abcdef";
            uri += '%';
            uri += hex[u >> 4];
            uri += hex[u & 0xF];
        } else {
            uri += c;
        }
    }
    uri += "?mode=ro";
    if (immutable)
        uri += "&immutable=1";
    return uri;
}

} // namespace cutefish::store::detail
