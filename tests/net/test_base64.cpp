// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/net/detail/base64.hpp>
#include <span>
#include <string>
#include <vector>

namespace {
std::string encodeAscii(std::string_view text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    return morph::net::detail::base64Encode(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}
}  // namespace

// RFC 4648 §10 test vectors.
TEST_CASE("base64Encode matches RFC 4648 test vectors", "[net][base64]") {
    REQUIRE(encodeAscii("") == "");
    REQUIRE(encodeAscii("f") == "Zg==");
    REQUIRE(encodeAscii("fo") == "Zm8=");
    REQUIRE(encodeAscii("foo") == "Zm9v");
    REQUIRE(encodeAscii("foob") == "Zm9vYg==");
    REQUIRE(encodeAscii("fooba") == "Zm9vYmE=");
    REQUIRE(encodeAscii("foobar") == "Zm9vYmFy");
}
