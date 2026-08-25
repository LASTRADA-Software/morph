// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bank/db/db_model.hpp"
#include "bank/dto/auth_dto.hpp"
#include "bank/dto/common.hpp"

/// @file
/// The Auth model: user registration, login, password change, and session
/// introspection. The model validates credentials and returns the principal to
/// install; the application layer (`App::login`) is what actually attaches the
/// principal to the bridge's default session.

namespace bank {

/// @brief Manages user identities and authentication.
class AuthModel : private db::WithMapper {
public:
    /// @brief Registers a new user; returns an AuthResult carrying the principal.
    dto::AuthResult execute(const dto::RegisterUser& action);

    /// @brief Verifies credentials; returns an AuthResult carrying the principal.
    dto::AuthResult execute(const dto::LoginRequest& action);

    /// @brief Changes a user's password after verifying the old one.
    dto::CommandResult execute(const dto::ChangePassword& action);

    /// @brief Reports whether a principal is attached to the current session.
    dto::SessionInfo execute(const dto::WhoAmI& action);
};

}  // namespace bank

using bank::AuthModel;
using bank::dto::ChangePassword;
using bank::dto::LoginRequest;
using bank::dto::RegisterUser;
using bank::dto::WhoAmI;

BRIDGE_REGISTER_MODEL(AuthModel, "AuthModel")
BRIDGE_REGISTER_ACTION(AuthModel, RegisterUser, "RegisterUser")
BRIDGE_REGISTER_ACTION(AuthModel, LoginRequest, "LoginRequest")
BRIDGE_REGISTER_ACTION(AuthModel, ChangePassword, "ChangePassword")
BRIDGE_REGISTER_ACTION(AuthModel, WhoAmI, "WhoAmI", ::morph::model::Loggable::No)
