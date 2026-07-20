// SPDX-License-Identifier: Apache-2.0
//
// Compile-check fixture for the MORPH_REQUIRE_VETTED_HMAC guard (see the
// try_compile() block at the end of tests/CMakeLists.txt). Passes an explicit
// MacFunction — must compile whether or not MORPH_REQUIRE_VETTED_HMAC is
// defined (a deployer who already injects a vetted MAC is unaffected by the
// guard).

#include <morph/session/session_auth.hpp>

int main() {
    const morph::session::TokenVerifier verifier{"secret", morph::session::hmacSha256};
    (void)verifier;
    return 0;
}
