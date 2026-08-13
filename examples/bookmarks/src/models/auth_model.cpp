// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/auth_model.hpp"

#include "bookmarks/auth/bookmarks_authorizer.hpp"

#include <morph/session/session_auth.hpp>

#include <cstdint>

namespace bookmarks {

namespace {

/// @brief Expiry stamped into every minted token: 2100-01-01T00:00:00Z.
///
/// `SessionToken::expiresAtMs` must be strictly positive — `TokenVerifier`
/// treats `<= 0` as already-expired precisely so a zeroed token is never an
/// eternal credential — so "no expiry" is not expressible and a value has to
/// be chosen. This rung chooses one far enough out to be irrelevant, because
/// it ships no session-renewal path: a shorter lifetime would mean a client
/// silently losing its session mid-run with nothing to recover it but
/// logging in again, which would be testing a re-authentication flow this
/// rung does not have rather than the authorization pipeline it does. A
/// deployment that replaces this model's body with a real credential check
/// (see `auth_dto.hpp`'s `@file` comment) sets a real lifetime here at the
/// same time.
constexpr std::int64_t kTokenExpiresAtMs = 4102444800000;

}  // namespace

LoginResult AuthModel::execute(const Login& action) {
    if (!action.validate()) {
        throw ValidationError{"Login: username must be a valid principal"};
    }
    if (auth::isReservedPrincipal(action.username)) {
        // See isReservedPrincipal's doc comment: minting one of these on
        // request would hand any caller the internal worker's authority.
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
        // no use for: there is no scenario here where a token is minted
        // against a clock ahead of the verifier's, since the issuer and the
        // verifier are the same process.
        .issuedAtMs = 0,
        .expiresAtMs = kTokenExpiresAtMs,
        .roles = {},
    });
    return LoginResult{.token = AuthToken{std::move(token)}, .principal = action.username};
}

}  // namespace bookmarks
