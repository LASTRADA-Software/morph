// SPDX-License-Identifier: Apache-2.0
//
// Example adapter: wires libsodium's crypto_auth_hmacsha256_* into morph's
// MacFunction seam (see docs/spec/security.md, "MAC-primitive recommended
// wiring"). Copy this file (and link libsodium) into a production deployment
// that wants a vetted, side-channel-hardened HMAC-SHA256 instead of the
// self-contained reference implementation `morph::session::hmacSha256`.
//
// Requires libsodium (https://libsodium.org) — only compiled when
// MORPH_BUILD_HMAC_EXAMPLE_LIBSODIUM is ON (see CMakeLists.txt in this
// directory).
#pragma once

#include <sodium.h>

#include <morph/session/session_auth.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace morph::examples {

/// @brief libsodium-backed HMAC-SHA256 `MacFunction` adapter.
///
/// Returns the raw 32-byte MAC, matching the `MacFunction` contract
/// (`morph::session::MacFunction`) exactly — a drop-in replacement for
/// `morph::session::hmacSha256` with a vetted, side-channel-hardened core.
/// Calls `sodium_init()` on every invocation; libsodium documents this as
/// idempotent and safe to call repeatedly (it returns early once already
/// initialized), and skipping it is undefined behavior on some platforms.
/// @param key Secret key bytes.
/// @param msg Message bytes to authenticate.
/// @return 32-byte HMAC-SHA256 MAC as a byte string.
/// @throws std::runtime_error if `sodium_init()` fails.
inline std::string sodiumHmacSha256(std::string_view key, std::string_view msg) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium: sodium_init failed");
    }
    unsigned char out[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, reinterpret_cast<const unsigned char*>(key.data()), key.size());
    crypto_auth_hmacsha256_update(&st, reinterpret_cast<const unsigned char*>(msg.data()), msg.size());
    crypto_auth_hmacsha256_final(&st, out);
    return std::string(reinterpret_cast<char*>(out), sizeof out);
}

}  // namespace morph::examples
