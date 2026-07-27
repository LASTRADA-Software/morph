// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/net/detail/sha1.hpp>
#include <sstream>
#include <string>
#include <string_view>

namespace {
std::string toHex(const std::array<std::uint8_t, 20>& digest) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (std::uint8_t byte : digest) {
        out.push_back(kHexDigits[(byte >> 4) & 0x0Fu]);
        out.push_back(kHexDigits[byte & 0x0Fu]);
    }
    return out;
}
}  // namespace

// FIPS 180-4 / well-known SHA-1 test vectors.
TEST_CASE("sha1Digest matches known test vectors", "[net][sha1]") {
    REQUIRE(toHex(morph::net::detail::sha1Digest("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    REQUIRE(toHex(morph::net::detail::sha1Digest("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    REQUIRE(toHex(morph::net::detail::sha1Digest(
                std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"})) ==
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST_CASE("sha1Digest handles a message spanning multiple 64-byte blocks", "[net][sha1]") {
    // 56 'a' characters plus padding crosses the FIPS 180-4 single/double-block
    // boundary (the 55/56-byte edge where the length suffix no longer fits in
    // the first block) — a common off-by-one in hand-rolled implementations.
    std::string longMessage(56, 'a');
    REQUIRE(toHex(morph::net::detail::sha1Digest(longMessage)) == "c2db330f6083854c99d4b5bfb6e8f29f201be699");
}
