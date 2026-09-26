// SPDX-License-Identifier: Apache-2.0
//
// morph::net's WebSocket handshake takes its base64 from core-cpp
// (ws_handshake.hpp: Sec-WebSocket-Accept and Sec-WebSocket-Key), so this pins
// the encoding it relies on: standard RFC 4648 alphabet, `=` padding.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <core/Base64.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace {
std::string encodeAscii(std::string_view text) { return ::core::base64::encode(text); }
}  // namespace

// RFC 4648 §10 test vectors.
TEST_CASE("core::base64::encode matches RFC 4648 test vectors", "[net][base64]") {
    REQUIRE(encodeAscii("") == "");
    REQUIRE(encodeAscii("f") == "Zg==");
    REQUIRE(encodeAscii("fo") == "Zm8=");
    REQUIRE(encodeAscii("foo") == "Zm9v");
    REQUIRE(encodeAscii("foob") == "Zm9vYg==");
    REQUIRE(encodeAscii("fooba") == "Zm9vYmE=");
    REQUIRE(encodeAscii("foobar") == "Zm9vYmFy");
}

// The handshake encodes raw bytes (a SHA-1 digest, a random key), not text:
// bytes above 0x7F and the alphabet's last two symbols, `+` and `/`, which the
// URL-safe alphabet would spell `-` and `_`.
TEST_CASE("core::base64::encode encodes high bytes with the standard alphabet", "[net][base64]") {
    std::array<std::uint8_t, 2> const bytes{0xFB, 0xFF};
    REQUIRE(::core::base64::encode(bytes.begin(), bytes.end()) == "+/8=");
}
