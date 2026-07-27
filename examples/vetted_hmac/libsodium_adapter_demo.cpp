// SPDX-License-Identifier: Apache-2.0
//
// Runnable demo: mints a token and verifies it through a SigningAuthorizer
// wired to the libsodium adapter — the "recommended wiring" from
// docs/spec/security.md, made concrete. Not a test; see
// test_libsodium_adapter.cpp for the known-answer/interop assertions.

#include <cstdlib>
#include <iostream>
#include <morph/session/session_auth.hpp>
#include <string>

#include "libsodium_adapter.hpp"

int main() {
    using morph::examples::sodiumHmacSha256;
    using morph::session::SessionToken;
    using morph::session::SigningAuthorizer;
    using morph::session::TokenIssuer;

    const std::string secret = "replace-with-a-real-secret-from-your-kms";

    // Login-flow side: mint a token (see docs/spec/security.md, "Issuing
    // tokens — the login flow"), using the vetted adapter as the MacFunction.
    const TokenIssuer issuer{secret, sodiumHmacSha256};
    const std::string token = issuer.issue(SessionToken{
        .principal = "demo-user",
        .issuedAtMs = 0,
        .expiresAtMs = 9'999'999'999'999,  // demo only: a real deployment sets a short expiry
        .roles = {"admin"},
    });

    // Server side: install a SigningAuthorizer wired to the same vetted MacFunction.
    const SigningAuthorizer authz{secret, sodiumHmacSha256};
    morph::session::Context ctx;
    ctx.token = token;

    if (!authz.authorize(ctx, "DemoModel", "DemoAction")) {
        std::cerr << "unexpected: token failed to authorize\n";
        return EXIT_FAILURE;
    }
    std::cout << "authorized principal: " << authz.authenticate(ctx).value() << '\n';
    return EXIT_SUCCESS;
}
