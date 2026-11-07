#include "sha256.h"

#include <algorithm>
#include <cstring>

namespace cutefish::store::detail {

namespace {

constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

} // namespace

Sha256::Sha256() noexcept
    : m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}
{
}

void Sha256::block(const std::uint8_t* p) noexcept
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = std::uint32_t(p[4 * i]) << 24 | std::uint32_t(p[4 * i + 1]) << 16 | std::uint32_t(p[4 * i + 2]) << 8
                | std::uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    m_bytes += size;
    if (m_used > 0) {
        const std::size_t take = std::min(size, m_buffer.size() - m_used);
        std::memcpy(m_buffer.data() + m_used, p, take);
        m_used += take;
        p += take;
        size -= take;
        if (m_used < m_buffer.size())
            return;
        block(m_buffer.data());
        m_used = 0;
    }
    for (; size >= 64; size -= 64, p += 64)
        block(p);
    if (size > 0) {
        std::memcpy(m_buffer.data(), p, size);
        m_used = size;
    }
}

std::array<std::uint8_t, 32> Sha256::digest() noexcept
{
    const std::uint64_t bits = m_bytes * 8;
    const std::uint8_t pad = 0x80;
    update(&pad, 1);
    const std::uint8_t zero = 0;
    while (m_used != 56)
        update(&zero, 1);
    std::uint8_t length[8];
    for (int i = 0; i < 8; ++i)
        length[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(length, 8);
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            out[4 * i + j] = static_cast<std::uint8_t>(m_state[i] >> (24 - 8 * j));
    return out;
}

std::string Sha256::hexDigest()
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t byte : digest()) {
        out += hex[byte >> 4];
        out += hex[byte & 0xF];
    }
    return out;
}

} // namespace cutefish::store::detail
