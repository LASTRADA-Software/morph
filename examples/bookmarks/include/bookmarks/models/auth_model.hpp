// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/dto/auth_dto.hpp"

namespace bookmarks {

/// @brief Mints a signed token for whichever `username` the caller claims —
///        see `bookmarks/dto/auth_dto.hpp`'s own `@file` comment for exactly
///        what "dev-mode login" does and does not mean here.
///
/// Stateless: no database, so no `db::WithMapper` base and nothing to
/// persist. The secret it signs with comes from the process-global
/// `auth::tokenIssuer()` slot, which `app::App` installs at startup with the
/// *same* secret it hands its `auth::BookmarksAuthorizer` — this model is
/// registered via the plain `BRIDGE_REGISTER_MODEL` default-construction
/// path rather than `ModelRegistryFactory`'s per-instance construction-hook
/// seam (`include/morph/core/registry.hpp`), so a process-global slot passes
/// the secret through instead, exactly as `morph::journal::setActionLog`
/// already works around for action logs.
class AuthModel {
  public:
    /// @brief Verifies @p action's username and mints a token for it.
    /// @param action The login request.
    /// @return The minted token plus the principal it was minted for.
    /// @throws ValidationError if the username is not a valid principal, or
    ///         if no `TokenIssuer` has been installed (no `App` is alive).
    LoginResult execute(const Login& action);
};

}  // namespace bookmarks

BRIDGE_REGISTER_MODEL(bookmarks::AuthModel, "AuthModel")
// Loggable::No: the action's JSON body is the caller's claimed identity and
// its result carries a live bearer token — neither belongs in a durable,
// replayable action log.
BRIDGE_REGISTER_ACTION(bookmarks::AuthModel, bookmarks::Login, "Login", ::morph::model::Loggable::No)
