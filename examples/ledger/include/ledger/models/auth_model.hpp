// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "ledger/core/errors.hpp"
#include "ledger/dto/auth_dto.hpp"

namespace ledger {

/// @brief Mints a signed token for whichever `username` the caller claims --
///        see `ledger/dto/auth_dto.hpp`'s own `@file` comment for exactly
///        what "dev-mode login" does and does not mean here. Mirrors
///        `bookmarks::AuthModel` exactly.
///
/// Stateless: no database, so nothing to persist. The secret it signs with
/// comes from the process-global `auth::tokenIssuer()` slot, which
/// `app::App` installs at startup with the *same* secret it hands its
/// `auth::LedgerAuthorizer` -- this model is registered via the plain
/// `BRIDGE_REGISTER_MODEL` default-construction path rather than
/// `ModelRegistryFactory`'s per-instance construction-hook seam, so a
/// process-global slot passes the secret through instead.
class AuthModel {
public:
    /// @brief Verifies @p action's username and mints a token for it.
    /// @param action The login request.
    /// @return The minted token plus the principal it was minted for.
    /// @throws ValidationError if the username is not a valid principal, if
    ///         it names the reserved `system:` namespace, or if no
    ///         `TokenIssuer` has been installed (no `App` is alive).
    LoginResult execute(const Login& action);
};

}  // namespace ledger

BRIDGE_REGISTER_MODEL(ledger::AuthModel, "AuthModel")
// Loggable::No: the action's JSON body is the caller's claimed identity and
// its result carries a live bearer token -- neither belongs in a durable,
// replayable action log. Mirrors bookmarks::AuthModel's identical
// registration.
BRIDGE_REGISTER_ACTION(ledger::AuthModel, ledger::Login, "Login", ::morph::model::Loggable::No)
