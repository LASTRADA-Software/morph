// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "session.hpp"

/// @file session/session_auth.hpp
/// @brief Opt-in authenticated sessions: signed bearer tokens and a verifying
///        `IAuthorizer` so a `RemoteServer` can trust the caller's identity.
///
/// Include this only if you want authentication. The token is a signed,
/// stateless bearer credential — `base64url(claimsJson) "." base64url(mac)` —
/// minted app-side by a login action via `TokenIssuer` and verified per request
/// by `SigningAuthorizer`, which also feeds the verified principal back to
/// `RemoteServer` so `session::current()->principal` is authoritative inside a
/// model. The MAC primitive is pluggable (`MacFunction`); a self-contained,
/// test-vector-verified HMAC-SHA256 (`hmacSha256`) is the default. See
/// `docs/spec/security.md` for the full trust model and its limits.

namespace morph::session {

// ── Cryptographic primitives (reference implementations) ─────────────────────
//
// These are self-contained so morph has no crypto dependency. They are correct
// (verified against the standard test vectors in test_session_auth.cpp) but are
// *reference* code: security-sensitive deployments should inject a vetted
// library's HMAC via `MacFunction` rather than rely on these.
namespace detail {

// NOLINTBEGIN(readability-identifier-length,readability-function-cognitive-complexity,cppcoreguidelines-pro-bounds-constant-array-index,bugprone-easily-swappable-parameters)

/// @brief SHA-256 (FIPS 180-4). Returns the raw 32-byte digest.
/// @param data Message bytes to hash.
/// @return 32-byte digest as a byte string.
inline std::string sha256(std::string_view data) {
    static constexpr std::array<uint32_t, 64> kRound = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    std::array<uint32_t, 8> hash = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    std::vector<uint8_t> msg(data.begin(), data.end());
    const uint64_t bitLen = msg.size() * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) {
        msg.push_back(0x00);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        msg.push_back(static_cast<uint8_t>((bitLen >> shift) & 0xff));
    }

    auto rotr = [](uint32_t val, uint32_t bits) { return (val >> bits) | (val << (32 - bits)); };

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::array<uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[chunk + (i * 4)]) << 24) |
                   (static_cast<uint32_t>(msg[chunk + (i * 4) + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + (i * 4) + 2]) << 8) |
                   static_cast<uint32_t>(msg[chunk + (i * 4) + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = hash[0];
        uint32_t b = hash[1];
        uint32_t c = hash[2];
        uint32_t d = hash[3];
        uint32_t e = hash[4];
        uint32_t f = hash[5];
        uint32_t g = hash[6];
        uint32_t h = hash[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const uint32_t bigS1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t choose = (e & f) ^ (~e & g);
            const uint32_t t1 = h + bigS1 + choose + kRound[i] + w[i];
            const uint32_t bigS0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = bigS0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    std::string out(32, '\0');
    for (std::size_t i = 0; i < 8; ++i) {
        out[(i * 4)] = static_cast<char>((hash[i] >> 24) & 0xff);
        out[(i * 4) + 1] = static_cast<char>((hash[i] >> 16) & 0xff);
        out[(i * 4) + 2] = static_cast<char>((hash[i] >> 8) & 0xff);
        out[(i * 4) + 3] = static_cast<char>(hash[i] & 0xff);
    }
    return out;
}

/// @brief HMAC-SHA256 (RFC 2104). Returns the raw 32-byte MAC.
/// @param key Secret key bytes.
/// @param msg Message bytes to authenticate.
/// @return 32-byte MAC as a byte string.
inline std::string hmacSha256Raw(std::string_view key, std::string_view msg) {
    constexpr std::size_t blockSize = 64;
    std::string block{key};
    if (block.size() > blockSize) {
        block = sha256(block);
    }
    block.resize(blockSize, '\0');

    std::string inner(blockSize, '\0');
    std::string outer(blockSize, '\0');
    for (std::size_t i = 0; i < blockSize; ++i) {
        inner[i] = static_cast<char>(static_cast<uint8_t>(block[i]) ^ 0x36);
        outer[i] = static_cast<char>(static_cast<uint8_t>(block[i]) ^ 0x5c);
    }
    const std::string innerHash = sha256(inner + std::string{msg});
    return sha256(outer + innerHash);
}

/// @brief Constant-time byte-string equality (guards against MAC timing leaks).
/// @param lhs First string.
/// @param rhs Second string.
/// @return `true` iff the strings are byte-for-byte equal.
inline bool constantTimeEquals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;  // length is not secret
    }
    uint8_t diff = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        diff = uint8_t(diff | (uint8_t(lhs[i]) ^ uint8_t(rhs[i])));
    }
    return diff == 0;
}

/// @brief The url-safe base64 alphabet (RFC 4648 §5): `-` and `_` replace `+`/`/`.
inline constexpr std::string_view kB64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/// @brief base64url-encodes @p in without padding (RFC 4648 §5).
/// @param in Bytes to encode.
/// @return The base64url text.
inline std::string base64UrlEncode(std::string_view in) {
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const uint32_t triple = (static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << 16) |
                                (static_cast<uint32_t>(static_cast<uint8_t>(in[i + 1])) << 8) |
                                static_cast<uint32_t>(static_cast<uint8_t>(in[i + 2]));
        out.push_back(kB64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kB64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back(kB64Alphabet[(triple >> 6) & 0x3f]);
        out.push_back(kB64Alphabet[triple & 0x3f]);
    }
    const std::size_t rem = in.size() - i;
    if (rem == 1) {
        const uint32_t triple = static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << 16;
        out.push_back(kB64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kB64Alphabet[(triple >> 12) & 0x3f]);
    } else if (rem == 2) {
        const uint32_t triple = (static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << 16) |
                                (static_cast<uint32_t>(static_cast<uint8_t>(in[i + 1])) << 8);
        out.push_back(kB64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kB64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back(kB64Alphabet[(triple >> 6) & 0x3f]);
    }
    return out;
}

/// @brief Decodes canonical base64url text (no padding). Returns nullopt on invalid input.
///
/// Canonical decoding is enforced to remove token-string malleability: a token
/// is a signed byte string, but base64url is a *bit*-oriented encoding, so a
/// naive decoder that silently discards the leftover bits of the final symbol
/// would map several distinct encodings onto the same bytes (e.g. the trailing
/// character could be perturbed within the discarded low bits without changing
/// the decoded MAC). This function rejects any such non-canonical input:
///
/// - A length `% 4 == 1` is impossible for real base64url output and is rejected.
/// - The leftover bits that do not form a whole output byte (2 bits for a
///   1-remainder group, 4 bits for a 2-remainder group) **must be zero**; a
///   nonzero remainder means the encoding is not the canonical one this decoder
///   would itself produce, so it is rejected rather than truncated.
///
/// The result is thus a bijection over valid tokens: exactly one string decodes
/// to any given byte sequence.
/// @param in base64url text.
/// @return Decoded bytes, or nullopt if @p in has an invalid character, an
///         impossible length, or nonzero leftover (non-canonical) bits.
inline std::optional<std::string> base64UrlDecode(std::string_view in) {
    auto valueOf = [](char chr) -> int {
        const auto pos = kB64Alphabet.find(chr);
        return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
    };
    // A base64url group of 4 symbols encodes 3 bytes; the only valid tail
    // remainders are 0 (whole groups), 2 (→1 byte), or 3 (→2 bytes). A
    // remainder of 1 symbol cannot be produced by the encoder and is rejected.
    if (in.size() % 4 == 1) {
        return std::nullopt;
    }
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    for (const char chr : in) {
        const int val = valueOf(chr);
        if (val < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    // Reject non-canonical encodings: any bit left in `buffer` below the byte
    // boundary must be zero, otherwise this input is a distinct encoding of the
    // same bytes (malleability) rather than the canonical one.
    if (bits > 0 && (buffer & ((1U << bits) - 1)) != 0) {
        return std::nullopt;
    }
    return out;
}

// NOLINTEND(readability-identifier-length,readability-function-cognitive-complexity,cppcoreguidelines-pro-bounds-constant-array-index,bugprone-easily-swappable-parameters)

}  // namespace detail

/// @brief Pluggable message-authentication primitive: `mac(secret, message)`.
///
/// Returns the raw MAC bytes. Defaults to `hmacSha256`; inject a vetted
/// library's HMAC for production.
using MacFunction = std::function<std::string(std::string_view key, std::string_view message)>;

/// @brief Reference HMAC-SHA256 `MacFunction` (the default). Returns raw MAC bytes.
/// @param key     Secret key bytes.
/// @param message Message bytes to authenticate.
/// @return 32-byte HMAC as a byte string.
inline std::string hmacSha256(std::string_view key, std::string_view message) {
    return detail::hmacSha256Raw(key, message);
}

/// @brief Wall-clock source (milliseconds since the Unix epoch), injectable for tests.
using Clock = std::function<int64_t()>;

/// @brief Default clock: `std::chrono::system_clock` in milliseconds since epoch.
/// @return Current time in ms since the Unix epoch.
inline int64_t systemClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// @brief Verified claims carried by a session token.
///
/// Serialised to JSON (Glaze) as the signed payload. Extend with app claims as
/// needed — unknown fields are ignored on read, so adding claims is compatible.
struct SessionToken {
    /// @brief Authenticated user/principal id.
    std::string principal;
    /// @brief Issue time, ms since epoch. If positive, `verify` rejects a token
    ///        whose `issuedAtMs` is more than `kClockSkewMs` in the future
    ///        (not-yet-valid). `0` (unset) disables the not-before check.
    int64_t issuedAtMs = 0;
    /// @brief Expiry, ms since epoch. **Must be strictly positive** — a token
    ///        must carry a real expiry. `verify` treats `expiresAtMs <= 0`
    ///        (including the default `0`) as invalid/expired, so a zero token is
    ///        never an eternal credential.
    int64_t expiresAtMs = 0;
    /// @brief Coarse-grained roles the app can key authorization policy on.
    std::vector<std::string> roles;
};

/// @brief Allowed clock skew (ms) between issuer and verifier for the
///        not-before (`issuedAtMs`) check. Keeps a token minted a moment ahead
///        of the verifier's clock from being spuriously rejected.
inline constexpr int64_t kClockSkewMs = 60'000;  // 60s

/// @brief Why token verification failed.
enum class AuthError : std::uint8_t {
    Malformed,     ///< Not `payload.sig`, bad base64url, or unparseable claims.
    BadSignature,  ///< MAC did not match — forged or tampered.
    Expired,       ///< Missing/non-positive `expiresAtMs`, or it is in the past.
    NotYetValid,   ///< `issuedAtMs` is set and more than `kClockSkewMs` in the future.
};

/// @brief Thrown by `TokenIssuer::issue` if serialising the claims fails.
///
/// Not realistically reachable for `SessionToken` (a flat aggregate of
/// strings/integers, same as `journal::LogEntry`/`FileQueueRecord`), but
/// routing through a throwing helper rather than discarding the error keeps
/// this writer consistent with its two sibling writers
/// (`journal::toJson`/`offline::detail::toJson`), instead of silently
/// serialising a claims blob that later fails to decode.
struct TokenIssuanceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {

/// @brief Converts a Glaze error into a `TokenIssuanceError`, or does nothing
///        if @p errCode reports success. See `journal::detail::throwOnGlazeError`
///        for the identical pattern in the sibling writer.
/// @param errCode Result of a `glz::write` call.
/// @param context Buffer passed to `glz::format_error` for the message.
inline void throwOnGlazeError(const glz::error_ctx& errCode, std::string_view context) {
    if (errCode) {
        throw TokenIssuanceError{glz::format_error(errCode, context)};
    }
}

/// @brief Write options that escape ASCII control bytes as `\\uXXXX` sequences.
///
/// glaze 7.4 leaves control bytes (0x00-0x1F) unescaped by default, which
/// breaks a `SessionToken` carrying one in `principal`/`roles` two ways: RFC
/// 8259 requires those bytes escaped, so the raw byte alone yields JSON this
/// morph's own `TokenVerifier` cannot decode — a signed token that fails to
/// verify, a silent issue-succeeds/verify-fails asymmetry; worse, once the
/// same string also contains an escaped `\` or `"`, glaze's chunked writer
/// path silently rewrites the control byte as two 0x00 bytes, corrupting the
/// claims before they are ever signed. Mirrors
/// `morph::wire::detail::EscapingWriteOpts` (`core/wire.hpp`) exactly;
/// duplicated here (rather than shared) so this header stays free of a
/// `core/` dependency. Escaping is lossless, so any such byte still
/// round-trips through `TokenVerifier::verify` unchanged.
struct EscapingWriteOpts : glz::opts {
    // NOLINTNEXTLINE(readability-identifier-naming) — glaze's option name, matched by name.
    bool escape_control_characters = true;
};

}  // namespace detail

/// @brief Mints signed bearer tokens from claims using a shared secret.
///
/// Wire format: `base64url(claimsJson) "." base64url(mac(secret, payload))`.
class TokenIssuer {
public:
#ifdef MORPH_REQUIRE_VETTED_HMAC
    /// @brief Constructs an issuer over @p secret using @p mac.
    ///
    /// Built with `MORPH_REQUIRE_VETTED_HMAC` defined: @p mac has **no
    /// default** here, so a call site that relied on the reference
    /// `hmacSha256` default fails to compile, forcing an explicit vetted
    /// `MacFunction` injection. See `docs/spec/security.md` ("MAC-primitive
    /// recommended wiring").
    /// @param secret Shared secret (same value the verifier uses).
    /// @param mac    MAC primitive (required — no default under this build option).
    explicit TokenIssuer(std::string secret, MacFunction mac) : _secret{std::move(secret)}, _mac{std::move(mac)} {}
#else
    /// @brief Constructs an issuer over @p secret using @p mac.
    /// @param secret Shared secret (same value the verifier uses).
    /// @param mac    MAC primitive; defaults to `hmacSha256`.
    explicit TokenIssuer(std::string secret, MacFunction mac = hmacSha256)
        : _secret{std::move(secret)}, _mac{std::move(mac)} {}
#endif

    /// @brief Serialises @p claims and returns a signed token string.
    ///
    /// Writes with `detail::EscapingWriteOpts` so a raw ASCII control byte in
    /// `principal`/`roles` round-trips through `TokenVerifier::verify` instead
    /// of producing invalid JSON (or, alongside an escaped `\`/`"`, silently
    /// corrupted JSON) — see that struct's doc comment.
    /// @param claims Claims to embed and sign.
    /// @return The signed `payload.sig` token.
    /// @throws TokenIssuanceError on encode failure (see `detail::throwOnGlazeError`
    ///         for why this is not realistically reachable for `SessionToken`).
    [[nodiscard]] std::string issue(const SessionToken& claims) const {
        std::string json;
        detail::throwOnGlazeError(glz::write<detail::EscapingWriteOpts{}>(claims, json), json);
        const std::string payload = detail::base64UrlEncode(json);
        const std::string sig = detail::base64UrlEncode(_mac(_secret, payload));
        return payload + "." + sig;
    }

private:
    std::string _secret;
    MacFunction _mac;
};

/// @brief Verifies signed bearer tokens against a shared secret.
class TokenVerifier {
public:
#ifdef MORPH_REQUIRE_VETTED_HMAC
    /// @brief Constructs a verifier over @p secret using @p mac.
    ///
    /// Built with `MORPH_REQUIRE_VETTED_HMAC` defined: @p mac has **no
    /// default** here, so a call site that relied on the reference
    /// `hmacSha256` default fails to compile, forcing an explicit vetted
    /// `MacFunction` injection. See `docs/spec/security.md` ("MAC-primitive
    /// recommended wiring").
    /// @param secret Shared secret (same value the issuer used).
    /// @param mac    MAC primitive (required — no default under this build option).
    explicit TokenVerifier(std::string secret, MacFunction mac) : _secret{std::move(secret)}, _mac{std::move(mac)} {}
#else
    /// @brief Constructs a verifier over @p secret using @p mac.
    /// @param secret Shared secret (same value the issuer used).
    /// @param mac    MAC primitive; defaults to `hmacSha256`.
    explicit TokenVerifier(std::string secret, MacFunction mac = hmacSha256)
        : _secret{std::move(secret)}, _mac{std::move(mac)} {}
#endif

    /// @brief Verifies @p token's signature and expiry and returns its claims.
    ///
    /// The MAC is checked *before* the payload is parsed, so untrusted JSON is
    /// never handed to the parser until authenticity is established.
    /// @param token Token string (`payload.sig`).
    /// @param nowMs Current time (ms since epoch) to check expiry against.
    /// @return The verified claims, or an `AuthError`.
    [[nodiscard]] std::expected<SessionToken, AuthError> verify(std::string_view token, int64_t nowMs) const {
        const auto dot = token.find('.');
        if (dot == std::string_view::npos || token.find('.', dot + 1) != std::string_view::npos) {
            return std::unexpected(AuthError::Malformed);
        }
        const std::string_view payload = token.substr(0, dot);
        const auto sig = detail::base64UrlDecode(token.substr(dot + 1));
        if (!sig) {
            return std::unexpected(AuthError::Malformed);
        }
        const std::string expectedMac = _mac(_secret, std::string{payload});
        if (!detail::constantTimeEquals(*sig, expectedMac)) {
            return std::unexpected(AuthError::BadSignature);
        }
        const auto json = detail::base64UrlDecode(payload);
        if (!json) {
            return std::unexpected(AuthError::Malformed);
        }
        SessionToken claims;
        // Ignore unknown claims so a token minted by a newer issuer (with extra
        // application claims) still verifies against an older verifier — the
        // forward-compatibility the SessionToken doc promises. See security.md.
        static constexpr glz::opts kLenient{.error_on_unknown_keys = false};
        if (glz::read<kLenient>(claims, *json)) {
            return std::unexpected(AuthError::Malformed);
        }
        // A token MUST carry a strictly-positive expiry. A missing/zero (or
        // negative) `expiresAtMs` is treated as already-expired rather than
        // "eternal", so a default-constructed or zeroed token is never an
        // unbounded bearer credential. See docs/spec/security.md.
        if (claims.expiresAtMs <= 0 || nowMs > claims.expiresAtMs) {
            return std::unexpected(AuthError::Expired);
        }
        // Not-before / issued-at check. When `issuedAtMs` is set (positive), a
        // token whose issue time is more than `kClockSkewMs` in the future is
        // not yet valid — it was minted against a clock ahead of ours beyond the
        // tolerated skew. An unset (`0`) or non-positive `issuedAtMs` skips this
        // check (issue time is optional/informational).
        if (claims.issuedAtMs > 0 && claims.issuedAtMs - nowMs > kClockSkewMs) {
            return std::unexpected(AuthError::NotYetValid);
        }
        return claims;
    }

private:
    std::string _secret;
    MacFunction _mac;
};

/// @brief `IAuthorizer` that authenticates via a signed `Context::token`.
///
/// `authorize` returns `true` only for a token with a valid signature and
/// unexpired claims (and, if a policy is supplied, one the policy admits).
/// `authenticate` returns the verified principal so `RemoteServer` makes it
/// authoritative. Install it on the server:
/// @code
/// auto authz = std::make_shared<morph::session::SigningAuthorizer>(sharedSecret);
/// auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz);
/// @endcode
class SigningAuthorizer : public IAuthorizer {
public:
    /// @brief Optional per-request policy over verified claims.
    ///
    /// Receives the verified token plus the target ids; return `false` to deny.
    /// The default (empty) admits any validly-signed, unexpired token.
    using Policy =
        std::function<bool(const SessionToken& claims, std::string_view modelType, std::string_view actionType)>;

#ifdef MORPH_REQUIRE_VETTED_HMAC
    /// @brief Constructs the authorizer.
    ///
    /// Built with `MORPH_REQUIRE_VETTED_HMAC` defined: @p mac has **no
    /// default**, so a call site that relied on the reference `hmacSha256`
    /// default fails to compile. See `docs/spec/security.md` ("MAC-primitive
    /// recommended wiring").
    /// @param secret Shared secret used to verify tokens.
    /// @param mac    MAC primitive (required — no default under this build option).
    /// @param clock  Time source (ms since epoch) for expiry; defaults to system time.
    /// @param policy Optional per-request policy over verified claims.
    explicit SigningAuthorizer(std::string secret, MacFunction mac, Clock clock = systemClockMs, Policy policy = {})
        : _verifier{std::move(secret), std::move(mac)}, _clock{std::move(clock)}, _policy{std::move(policy)} {}
#else
    /// @brief Constructs the authorizer.
    /// @param secret Shared secret used to verify tokens.
    /// @param mac    MAC primitive; defaults to `hmacSha256`.
    /// @param clock  Time source (ms since epoch) for expiry; defaults to system time.
    /// @param policy Optional per-request policy over verified claims.
    explicit SigningAuthorizer(std::string secret, MacFunction mac = hmacSha256, Clock clock = systemClockMs,
                               Policy policy = {})
        : _verifier{std::move(secret), std::move(mac)}, _clock{std::move(clock)}, _policy{std::move(policy)} {}
#endif

    /// @brief Allows the call iff @p ctx carries a valid token the policy admits.
    /// @param ctx        Per-call session (its `token` is verified).
    /// @param modelType  Target model type id (passed to the policy).
    /// @param actionType Target action type id (passed to the policy).
    /// @return `true` to allow dispatch, `false` to reject.
    [[nodiscard]] bool authorize(const Context& ctx,
                                 // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                 std::string_view modelType, std::string_view actionType) const override {
        const auto claims = _verifier.verify(ctx.token, _clock());
        if (!claims) {
            return false;
        }
        return !_policy || _policy(*claims, modelType, actionType);
    }

    /// @brief Returns the verified principal so the server can make it authoritative.
    /// @param ctx Per-call session (its `token` is verified).
    /// @return The token's principal if valid, else `nullopt`.
    [[nodiscard]] std::optional<std::string> authenticate(const Context& ctx) const override {
        const auto claims = _verifier.verify(ctx.token, _clock());
        if (!claims) {
            return std::nullopt;
        }
        return claims->principal;
    }

private:
    TokenVerifier _verifier;
    Clock _clock;
    Policy _policy;
};

}  // namespace morph::session
