#pragma once

// Internal text checks shared by the models. Not part of the public API.

#include <cutefish/store/application.h>
#include <cutefish/store/result.h>

#include <cstddef>
#include <string_view>

namespace cutefish::store::detail {

/// Longest identifier accepted anywhere (ARCHITECTURE §9.4).
inline constexpr std::size_t kMaxIdentifierBytes = 255;

/// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
bool isValidUtf8(std::string_view text) noexcept;

/// True if the text contains an ASCII control character (U+0000-U+001F, U+007F).
/// With allowNewlines, '\n' is accepted.
bool hasControlCharacters(std::string_view text, bool allowNewlines = false) noexcept;

/// Opaque identifier: 1 to 255 bytes of printable ASCII without spaces.
Result<void> checkOpaqueIdentifier(std::string_view text, std::string_view what);

/// Printable ASCII without spaces, 1 to maxBytes bytes (URLs, paths).
Result<void> checkAsciiToken(std::string_view text, std::string_view what, std::size_t maxBytes);

/// Display text: valid UTF-8, no control character, at most maxBytes.
/// Empty text is accepted unless required.
Result<void> checkText(std::string_view text, std::string_view what, std::size_t maxBytes, bool required,
                       bool allowNewlines = false);

/// Icon reference consistent with its kind (see Application::create).
Result<void> checkIcon(const IconRef& icon);

inline bool isAsciiDigit(char c) noexcept { return c >= '0' && c <= '9'; }
inline bool isAsciiAlpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isAsciiAlnum(char c) noexcept { return isAsciiDigit(c) || isAsciiAlpha(c); }

} // namespace cutefish::store::detail
