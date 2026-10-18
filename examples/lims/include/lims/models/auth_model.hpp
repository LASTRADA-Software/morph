// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/session/session_auth.hpp>

#include "lims/dto/auth_dto.hpp"

namespace lims {

/// @brief Mints session tokens. Stateless: a sign-in reads nothing but the installed issuer.
class LimsAuthModel {
public:
    /// @brief Mints a signed session token for @p action's username.
    /// @param action The username.
    /// @return The token and the principal.
    /// @throws ValidationError for a username that is not a principal, one in the reserved `system:` namespace, or
    ///         when no token issuer is installed.
    LimsLoginResult execute(const LimsLogin& action);
};

namespace auth {

/// @brief Installs the issuer `LimsLogin` mints with: the server's, or a development one in process. A server's
///        `LimsAuthorizer` verifies with the same secret.
/// @param issuer The issuer; null uninstalls it.
void setTokenIssuer(std::shared_ptr<::morph::session::TokenIssuer> issuer);

/// @brief The installed issuer.
/// @return It, or null.
[[nodiscard]] std::shared_ptr<::morph::session::TokenIssuer> tokenIssuer();

}  // namespace auth

}  // namespace lims

// Prefixed: the generic client links every example application, and each has its own sign-in. The registration
// macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::LimsAuthModel, "LimsAuth")
// Signing in changes no row a screen shows: the screen that runs it signs the session in, which reloads the catalog.
BRIDGE_REGISTER_ACTION(lims::LimsAuthModel, lims::LimsLogin, "LimsLogin", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(lims::LimsLogin, ::morph::effects::none)
// NOLINTEND(misc-const-correctness)
