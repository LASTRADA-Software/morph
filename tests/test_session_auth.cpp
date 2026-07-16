// SPDX-License-Identifier: Apache-2.0

#include <morph/session_auth.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using morph::session::AuthError;
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

// A fixed clock so token-expiry tests are deterministic.
constexpr int64_t kNow = 1'700'000'000'000;  // ms since epoch
constexpr auto fixedClock = [] { return kNow; };

SessionToken sampleClaims(std::string principal, std::vector<std::string> roles = {}) {
    return SessionToken{.principal = std::move(principal),
                        .issuedAtMs = kNow,
                        .expiresAtMs = kNow + 60'000,
                        .roles = std::move(roles)};
}

}  // namespace

TEST_CASE("sha256 matches FIPS 180-4 known-answer vectors", "[session_auth][crypto]") {
    REQUIRE(toHex(morph::session::detail::sha256("abc")) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(toHex(morph::session::detail::sha256("")) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // A multi-block message (> 64 bytes) exercises the chunk loop.
    REQUIRE(toHex(morph::session::detail::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("hmacSha256 matches RFC 4231 test case 2", "[session_auth][crypto]") {
    REQUIRE(toHex(hmacSha256("Jefe", "what do ya want for nothing?")) ==
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("base64url uses the url-safe alphabet without padding", "[session_auth][crypto]") {
    const auto encoded = morph::session::detail::base64UrlEncode(std::string_view{"\x00\x01\x02\xff", 4});
    REQUIRE_FALSE(encoded.contains('+'));
    REQUIRE_FALSE(encoded.contains('/'));
    REQUIRE_FALSE(encoded.contains('='));
}

TEST_CASE("base64url round-trips every remainder length", "[session_auth][crypto]") {
    for (const std::string_view sample : {"", "f", "fo", "foo", "foob", "fooba", "foobar"}) {
        const auto decoded = morph::session::detail::base64UrlDecode(morph::session::detail::base64UrlEncode(sample));
        REQUIRE(decoded.value() == std::string{sample});
    }
}

TEST_CASE("base64url rejects invalid characters", "[session_auth][crypto]") {
    REQUIRE_FALSE(morph::session::detail::base64UrlDecode("has space").has_value());
}

TEST_CASE("an issued token verifies and returns its claims", "[session_auth]") {
    const TokenIssuer issuer{"top-secret"};
    const std::string token = issuer.issue(sampleClaims("alice", {"admin"}));
    REQUIRE_FALSE(token.empty());

    const auto verified = TokenVerifier{"top-secret"}.verify(token, kNow);
    REQUIRE(verified.has_value());
    REQUIRE(verified.value().principal == "alice");
    REQUIRE(verified.value().roles == std::vector<std::string>{"admin"});
}

TEST_CASE("verification rejects the wrong secret", "[session_auth]") {
    const std::string token = TokenIssuer{"top-secret"}.issue(sampleClaims("bob"));
    const auto res = TokenVerifier{"other-secret"}.verify(token, kNow);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error() == AuthError::BadSignature);
}

TEST_CASE("verification rejects a tampered payload", "[session_auth]") {
    std::string token = TokenIssuer{"top-secret"}.issue(sampleClaims("bob"));
    token.front() = (token.front() == 'A') ? 'B' : 'A';  // flip a payload char
    const auto res = TokenVerifier{"top-secret"}.verify(token, kNow);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error() == AuthError::BadSignature);
}

TEST_CASE("verification rejects an expired token", "[session_auth]") {
    const std::string token = TokenIssuer{"top-secret"}.issue(sampleClaims("bob"));
    const auto res = TokenVerifier{"top-secret"}.verify(token, kNow + 120'000);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error() == AuthError::Expired);
}

TEST_CASE("verification rejects a malformed token", "[session_auth]") {
    REQUIRE(TokenVerifier{"top-secret"}.verify("notatoken", kNow).error() == AuthError::Malformed);
}

TEST_CASE("verification rejects a token with more than one '.'", "[session_auth]") {
    REQUIRE(TokenVerifier{"top-secret"}.verify("a.b.c", kNow).error() == AuthError::Malformed);
}

TEST_CASE("verification rejects a signature segment with invalid base64url characters", "[session_auth]") {
    const std::string token = TokenIssuer{"top-secret"}.issue(sampleClaims("bob"));
    const auto dot = token.find('.');
    const std::string corrupted = token.substr(0, dot + 1) + "has space";
    REQUIRE(TokenVerifier{"top-secret"}.verify(corrupted, kNow).error() == AuthError::Malformed);
}

TEST_CASE("verification rejects a payload segment with invalid base64url characters", "[session_auth]") {
    // The MAC is computed over the raw (still-encoded) payload substring, so a
    // hand-crafted payload needs its MAC recomputed to reach past BadSignature
    // and into the payload-decode Malformed branch.
    const std::string secret = "top-secret";
    const std::string payload = "has space";
    const std::string sig = morph::session::detail::base64UrlEncode(hmacSha256(secret, payload));
    const std::string token = payload + "." + sig;
    REQUIRE(TokenVerifier{secret}.verify(token, kNow).error() == AuthError::Malformed);
}

TEST_CASE("SigningAuthorizer authorizes a valid token and exposes its principal", "[session_auth]") {
    const std::string secret = "hmac-key";
    const SigningAuthorizer authz{secret, hmacSha256, fixedClock};
    morph::session::Context ctx;
    ctx.token = TokenIssuer{secret}.issue(sampleClaims("carol"));

    REQUIRE(authz.authorize(ctx, "AccountModel", "Deposit"));
    REQUIRE(authz.authenticate(ctx).value() == "carol");
}

TEST_CASE("SigningAuthorizer denies a request with no token", "[session_auth]") {
    const SigningAuthorizer authz{"hmac-key", hmacSha256, fixedClock};
    const morph::session::Context anon;
    REQUIRE_FALSE(authz.authorize(anon, "AccountModel", "Deposit"));
    REQUIRE_FALSE(authz.authenticate(anon).has_value());
}

TEST_CASE("SigningAuthorizer applies a role policy over a valid token", "[session_auth]") {
    const std::string secret = "hmac-key";
    const auto requiresAdmin = [](const SessionToken& claims, std::string_view, std::string_view) {
        return std::ranges::find(claims.roles, "admin") != claims.roles.end();
    };
    const SigningAuthorizer authz{secret, hmacSha256, fixedClock, requiresAdmin};

    morph::session::Context noRole;
    noRole.token = TokenIssuer{secret}.issue(sampleClaims("carol"));
    REQUIRE_FALSE(authz.authorize(noRole, "AccountModel", "Deposit"));

    morph::session::Context admin;
    admin.token = TokenIssuer{secret}.issue(sampleClaims("dave", {"admin"}));
    REQUIRE(authz.authorize(admin, "AccountModel", "Deposit"));
}

TEST_CASE("SigningAuthorizer with a policy still denies an invalid token before the policy runs", "[session_auth]") {
    const std::string secret = "hmac-key";
    bool policyCalled = false;
    const auto alwaysAllow = [&](const SessionToken&, std::string_view, std::string_view) {
        policyCalled = true;
        return true;
    };
    const SigningAuthorizer authz{secret, hmacSha256, fixedClock, alwaysAllow};

    morph::session::Context noToken;
    REQUIRE_FALSE(authz.authorize(noToken, "AccountModel", "Deposit"));
    REQUIRE_FALSE(policyCalled);

    morph::session::Context wrongSecret;
    wrongSecret.token = TokenIssuer{"other-secret"}.issue(sampleClaims("eve"));
    REQUIRE_FALSE(authz.authorize(wrongSecret, "AccountModel", "Deposit"));
    REQUIRE_FALSE(policyCalled);
}
