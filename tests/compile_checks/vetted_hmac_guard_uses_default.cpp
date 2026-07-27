// SPDX-License-Identifier: Apache-2.0
//
// Compile-check fixture for the MORPH_REQUIRE_VETTED_HMAC guard (see the
// try_compile() block at the end of tests/CMakeLists.txt). Relies on
// TokenVerifier's default `mac = hmacSha256` argument — must compile when
// MORPH_REQUIRE_VETTED_HMAC is NOT defined, and must FAIL to compile when it
// is (see docs/spec/security.md, "MAC-primitive recommended wiring").

#include <morph/session/session_auth.hpp>

int main() {
    const morph::session::TokenVerifier verifier{"secret"};  // relies on the default mac
    (void)verifier;
    return 0;
}
