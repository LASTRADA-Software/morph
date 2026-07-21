// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace morph::net::detail {

namespace sha1_impl {
inline std::uint32_t rotl(std::uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }
}  // namespace sha1_impl

/// @brief Computes the raw 20-byte SHA-1 digest of @p message.
///
/// Used only to compute the WebSocket handshake's `Sec-WebSocket-Accept`
/// value (RFC 6455 §1.3) — not a general-purpose or security-audited hash
/// implementation.
/// @param message Input bytes, treated as an opaque byte sequence (not text).
/// @return The 20-byte digest, big-endian per FIPS 180-4.
inline std::array<std::uint8_t, 20> sha1Digest(std::string_view message) {
    std::uint32_t h0 = 0x67452301;
    std::uint32_t h1 = 0xEFCDAB89;
    std::uint32_t h2 = 0x98BADCFE;
    std::uint32_t h3 = 0x10325476;
    std::uint32_t h4 = 0xC3D2E1F0;

    std::uint64_t const bitLen = static_cast<std::uint64_t>(message.size()) * 8u;
    std::vector<std::uint8_t> data(message.begin(), message.end());
    data.push_back(std::uint8_t{0x80});
    while (data.size() % 64 != 56) {
        data.push_back(std::uint8_t{0x00});
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<std::uint8_t>((bitLen >> shift) & 0xFFu));
    }

    for (std::size_t chunkStart = 0; chunkStart < data.size(); chunkStart += 64) {
        std::array<std::uint32_t, 80> w{};
        for (int i = 0; i < 16; ++i) {
            std::size_t const base = chunkStart + static_cast<std::size_t>(i) * 4;
            w[static_cast<std::size_t>(i)] =
                (static_cast<std::uint32_t>(data[base]) << 24) | (static_cast<std::uint32_t>(data[base + 1]) << 16) |
                (static_cast<std::uint32_t>(data[base + 2]) << 8) | static_cast<std::uint32_t>(data[base + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[static_cast<std::size_t>(i)] =
                sha1_impl::rotl(w[static_cast<std::size_t>(i - 3)] ^ w[static_cast<std::size_t>(i - 8)] ^
                                    w[static_cast<std::size_t>(i - 14)] ^ w[static_cast<std::size_t>(i - 16)],
                                1);
        }

        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            std::uint32_t const temp = sha1_impl::rotl(a, 5) + f + e + k + w[static_cast<std::size_t>(i)];
            e = d;
            d = c;
            c = sha1_impl::rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::uint8_t, 20> digest{};
    std::array<std::uint32_t, 5> const hs{h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < 5; ++i) {
        digest[(i * 4) + 0] = static_cast<std::uint8_t>((hs[i] >> 24) & 0xFFu);
        digest[(i * 4) + 1] = static_cast<std::uint8_t>((hs[i] >> 16) & 0xFFu);
        digest[(i * 4) + 2] = static_cast<std::uint8_t>((hs[i] >> 8) & 0xFFu);
        digest[(i * 4) + 3] = static_cast<std::uint8_t>(hs[i] & 0xFFu);
    }
    return digest;
}

}  // namespace morph::net::detail
