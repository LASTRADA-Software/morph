// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/session/session_auth.hpp>

#include <memory>

/// @file
/// Kanban's `IAuthorizer` -- `SigningAuthorizer`-derived, mirroring
/// `bookmarks::auth::BookmarksAuthorizer`'s shape (design spec §3's
/// corrected identity decision, *not* `polls::auth::PollsAuthorizer`'s
/// `AllowAllAuthorizer`-derived shape): `BoardModel::requireRole()` reads
/// `session::current()->principal` to key its `project_has_roles` lookup,
/// and only a verifying authorizer supplies a trustworthy one --
/// `security.md`'s documented behavior clears an unauthenticated caller's
/// principal to empty before every remote dispatch, which would make every
/// role check either always deny or silently diverge between `Local` and
/// `Socket` test modes.
///
/// `authorizeRegister`/`authorizeInstance` are left at their inherited
/// permissive defaults: `BoardModel` has no per-instance owner concept (its
/// instances are shared/keyed by `projectId`, exactly like `PollModel`), and
/// the actual role gate lives entirely inside `BoardModel::execute()`/
/// `ProjectAdminModel::execute()` via `requireRole()`.

namespace kanban::auth {

/// @brief This rung's `IAuthorizer`: verifies HMAC-signed session tokens
///        (inherited `SigningAuthorizer::authorize`/`authenticate`), stays
///        permissive on register/instance admission.
class KanbanAuthorizer : public ::morph::session::SigningAuthorizer {
  public:
    using SigningAuthorizer::SigningAuthorizer;
};

/// @brief Installs the process-wide `TokenIssuer` `Login` mints tokens from.
/// @param issuer The issuer to install, or `nullptr` to clear it.
void setTokenIssuer(std::shared_ptr<::morph::session::TokenIssuer> issuer);

/// @brief Returns the process-wide `TokenIssuer` installed by
///        `setTokenIssuer`, or `nullptr` if none is installed yet.
[[nodiscard]] std::shared_ptr<::morph::session::TokenIssuer> tokenIssuer();

}  // namespace kanban::auth
