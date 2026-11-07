#pragma once

// Writing JSON text, internal to store-core. Reading is left to SQLite's
// JSON functions (json_extract, json_each), so the core needs no JSON
// library.

#include <cstdio>
#include <string>
#include <string_view>

namespace cutefish::store::detail {

/// Appends `text` as a JSON string. The text must be valid UTF-8 (checked
/// by the callers); control characters are escaped.
inline void appendJsonString(std::string& out, std::string_view text)
{
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

} // namespace cutefish::store::detail
