// SPDX-License-Identifier: Apache-2.0
#include "ledger/models/auth_model.hpp"

#include <cstdint>
#include <morph/session/session_auth.hpp>

#include "ledger/auth/ledger_authorizer.hpp"

namespace ledger {

namespace {

/// @brief Expiry stamped into every minted token: 2100-01-01T00:00:00Z.
///
/// `SessionToken::expiresAtMs` must be strictly positive -- `TokenVerifier`
/// treats `<= 0` as already-expired precisely so a zeroed token is never an
/// eternal credential -- so "no expiry" is not expressible and a value has
/// to be chosen. Mirrors `bookmarks::AuthModel`'s identical constant and
/// rationale: this rung ships no session-renewal path either, so a shorter
/// lifetime would only mean testing a re-authentication flow that does not
/// exist.
constexpr std::int64_t kTokenExpiresAtMs = 4102444800000;

}  // namespace

LoginResult AuthModel::execute(const Login& action) {
    if (!action.validate()) {
        throw ValidationError{"Login: username must be a valid principal"};
    }
    if (auth::isReservedPrincipal(action.username)) {
        // See isReservedPrincipal's doc comment: minting one of these on
        // request would hand any caller the report runner's authority.
        throw ValidationError{"Login: the 'system:' principal namespace is reserved"};
    }
    auto issuer = auth::tokenIssuer();
    if (!issuer) {
        // No App has installed one -- e.g. a test that constructs AuthModel
        // directly, or a server bootstrap that forgot. A clear, typed
        // failure, not a null dereference.
        throw ValidationError{"Login: no token issuer installed"};
    }
    auto token = issuer->issue(::morph::session::SessionToken{
        .principal = action.username,
        // 0 disables TokenVerifier's not-before check, which this rung has
        // no use for: the issuer and the verifier are the same process, so
        // there is no scenario where a token is minted against a clock ahead
        // of the verifier's.
        .issuedAtMs = 0,
        .expiresAtMs = kTokenExpiresAtMs,
        .roles = {},
    });
    return LoginResult{.token = AuthToken{std::move(token)}, .principal = action.username};
}

}  // namespace ledger
