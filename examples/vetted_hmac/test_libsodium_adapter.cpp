// SPDX-License-Identifier: Apache-2.0
//
// Known-answer + interop tests for the libsodium HMAC-SHA256 adapter
// (libsodium_adapter.hpp). Compiled only when MORPH_BUILD_HMAC_EXAMPLE_LIBSODIUM
// is ON (see CMakeLists.txt in this directory).

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>

#include "libsodium_adapter.hpp"

using morph::examples::sodiumHmacSha256;
using morph::session::hmacSha256;
using morph::session::SessionToken;
using morph::session::SigningAuthorizer;
using morph::session::TokenIssuer;
using morph::session::TokenVerifier;

namespace {

std::string toHex(std::string_view bytes) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char chr : bytes) {
        const auto val = static_cast<unsigned char>(chr);
        out.push_back(kHex.at(val >> 4));
        out.push_back(kHex.at(val & 0x0f));
    }
    return out;
}

constexpr int64_t kNow = 1'700'000'000'000;

SessionToken sampleClaims(std::string principal) {
    return SessionToken{.principal = std::move(principal), .issuedAtMs = kNow, .expiresAtMs = kNow + 60'000};
}

}  // namespace

TEST_CASE("sodiumHmacSha256 matches RFC 4231 test case 2", "[vetted_hmac][libsodium]") {
    REQUIRE(toHex(sodiumHmacSha256("Jefe", "what do ya want for nothing?")) ==
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("sodiumHmacSha256 hashes an over-long key down to the block size (RFC 4231 case 6)",
          "[vetted_hmac][libsodium]") {
    const std::string longKey(131, '\xaa');
    REQUIRE(toHex(sodiumHmacSha256(longKey, "Test Using Larger Than Block-Size Key - Hash Key First")) ==
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("sodiumHmacSha256 is byte-identical to the reference hmacSha256", "[vetted_hmac][libsodium]") {
    REQUIRE(sodiumHmacSha256("top-secret", "some payload") == hmacSha256("top-secret", "some payload"));
    REQUIRE(sodiumHmacSha256("", "") == hmacSha256("", ""));
}

TEST_CASE("a token issued with the reference impl verifies under the libsodium adapter and vice versa",
          "[vetted_hmac][libsodium]") {
    const std::string secret = "shared-secret";

    const std::string tokenFromReference = TokenIssuer{secret, hmacSha256}.issue(sampleClaims("alice"));
    const auto verifiedBySodium = TokenVerifier{secret, sodiumHmacSha256}.verify(tokenFromReference, kNow);
    REQUIRE(verifiedBySodium.has_value());
    REQUIRE(verifiedBySodium->principal == "alice");

    const std::string tokenFromSodium = TokenIssuer{secret, sodiumHmacSha256}.issue(sampleClaims("bob"));
    const auto verifiedByReference = TokenVerifier{secret, hmacSha256}.verify(tokenFromSodium, kNow);
    REQUIRE(verifiedByReference.has_value());
    REQUIRE(verifiedByReference->principal == "bob");
}

TEST_CASE("SigningAuthorizer with the libsodium adapter authorizes a valid token and rejects a tampered one",
          "[vetted_hmac][libsodium]") {
    const std::string secret = "hmac-key";
    const auto fixedClock = [] { return kNow; };
    const SigningAuthorizer authz{secret, sodiumHmacSha256, fixedClock};

    morph::session::Context ctx;
    ctx.token = TokenIssuer{secret, sodiumHmacSha256}.issue(sampleClaims("carol"));
    REQUIRE(authz.authorize(ctx, "AccountModel", "Deposit"));
    REQUIRE(authz.authenticate(ctx).value() == "carol");

    ctx.token.front() = (ctx.token.front() == 'A') ? 'B' : 'A';  // tamper the payload
    REQUIRE_FALSE(authz.authorize(ctx, "AccountModel", "Deposit"));
}
