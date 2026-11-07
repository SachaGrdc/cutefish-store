#pragma once

// SHA-256 (FIPS 180-4), internal to store-core: used to fingerprint the
// metadata files of the sources. Not a security boundary (the file and the
// catalogue belong to the same user); it only detects a change of content.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cutefish::store::detail {

class Sha256
{
public:
    Sha256() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    void update(std::string_view text) noexcept { update(text.data(), text.size()); }
    /// Finishes the computation; the object must not be updated afterwards.
    std::array<std::uint8_t, 32> digest() noexcept;
    /// Lowercase hexadecimal digest, 64 characters.
    std::string hexDigest();

private:
    void block(const std::uint8_t* p) noexcept;

    std::array<std::uint32_t, 8> m_state;
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_used = 0;
    std::uint64_t m_bytes = 0;
};

} // namespace cutefish::store::detail
