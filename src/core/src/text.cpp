#include "text.h"

#include <string>

namespace cutefish::store::detail {

bool isValidUtf8(std::string_view text) noexcept
{
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const auto* end = p + text.size();
    while (p < end) {
        const unsigned char c = *p;
        if (c < 0x80) {
            ++p;
            continue;
        }
        std::size_t length;
        char32_t codepoint;
        char32_t minimum;
        if ((c & 0xE0) == 0xC0) {
            length = 2;
            codepoint = c & 0x1F;
            minimum = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
            codepoint = c & 0x0F;
            minimum = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
            codepoint = c & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (static_cast<std::size_t>(end - p) < length)
            return false;
        for (std::size_t i = 1; i < length; ++i) {
            if ((p[i] & 0xC0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (p[i] & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
            return false;
        p += length;
    }
    return true;
}

bool hasControlCharacters(std::string_view text, bool allowNewlines) noexcept
{
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '\n' && allowNewlines)
            continue;
        if (c < 0x20 || c == 0x7F)
            return true;
    }
    return false;
}

Result<void> checkOpaqueIdentifier(std::string_view text, std::string_view what)
{
    if (text.empty())
        return Error::invalidArgument(std::string(what) + " is empty");
    if (text.size() > kMaxIdentifierBytes)
        return Error::invalidArgument(std::string(what) + " is longer than 255 bytes");
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7F)
            return Error::invalidArgument(std::string(what) + " contains a character outside printable ASCII");
    }
    return {};
}

Result<void> checkText(std::string_view text, std::string_view what, std::size_t maxBytes, bool required,
                       bool allowNewlines)
{
    if (text.empty())
        return required ? Result<void>(Error::invalidArgument(std::string(what) + " is empty")) : Result<void>();
    if (text.size() > maxBytes)
        return Error::invalidArgument(std::string(what) + " is too long");
    if (!isValidUtf8(text))
        return Error::invalidArgument(std::string(what) + " is not valid UTF-8");
    if (hasControlCharacters(text, allowNewlines))
        return Error::invalidArgument(std::string(what) + " contains a control character");
    return {};
}

} // namespace cutefish::store::detail
