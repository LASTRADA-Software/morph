// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace morph::net::detail {

/// @brief Encodes @p bytes as standard (RFC 4648) base64 with `=` padding.
/// @param bytes Input byte span to encode.
/// @return The base64-encoded string.
inline std::string base64Encode(std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
        std::uint32_t const chunk = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                    (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                    static_cast<std::uint32_t>(bytes[i + 2]);
        out.push_back(kAlphabet[(chunk >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(chunk >> 6) & 0x3Fu]);
        out.push_back(kAlphabet[chunk & 0x3Fu]);
        i += 3;
    }
    std::size_t const remaining = bytes.size() - i;
    if (remaining == 1) {
        std::uint32_t const chunk = static_cast<std::uint32_t>(bytes[i]) << 16;
        out.push_back(kAlphabet[(chunk >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3Fu]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        std::uint32_t const chunk =
            (static_cast<std::uint32_t>(bytes[i]) << 16) | (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out.push_back(kAlphabet[(chunk >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3Fu]);
        out.push_back(kAlphabet[(chunk >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

}  // namespace morph::net::detail
