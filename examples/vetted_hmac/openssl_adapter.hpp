// SPDX-License-Identifier: Apache-2.0
//
// Example adapter: wires OpenSSL's HMAC(EVP_sha256(), ...) into morph's
// MacFunction seam (see docs/spec/security.md, "MAC-primitive recommended
// wiring"). Copy this file (and link OpenSSL::Crypto) into a production
// deployment that wants a vetted HMAC-SHA256 instead of the self-contained
// reference implementation `morph::session::hmacSha256`.
//
// Requires OpenSSL (https://openssl.org) — only compiled when
// MORPH_BUILD_HMAC_EXAMPLE_OPENSSL is ON (see CMakeLists.txt in this
// directory).
#pragma once

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <morph/session/session_auth.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace morph::examples {

/// @brief OpenSSL-backed HMAC-SHA256 `MacFunction` adapter.
///
/// Returns the raw 32-byte MAC, matching the `MacFunction` contract
/// (`morph::session::MacFunction`) exactly — a drop-in replacement for
/// `morph::session::hmacSha256` with a vetted, widely-audited core.
/// @param key Secret key bytes.
/// @param msg Message bytes to authenticate.
/// @return 32-byte HMAC-SHA256 MAC as a byte string.
/// @throws std::runtime_error if OpenSSL's `HMAC()` call fails.
inline std::string opensslHmacSha256(std::string_view key, std::string_view msg) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outLen = 0;
    const unsigned char* result = HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                                       reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), out, &outLen);
    if (result == nullptr) {
        throw std::runtime_error("OpenSSL: HMAC computation failed");
    }
    return std::string(reinterpret_cast<char*>(out), outLen);
}

}  // namespace morph::examples
