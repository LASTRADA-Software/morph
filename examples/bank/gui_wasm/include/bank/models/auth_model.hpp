// SPDX-License-Identifier: Apache-2.0
#pragma once

// WASM shadow of include/bank/models/auth_model.hpp: the SAME class + action
// registrations the controllers/QML expect, but with no Lightweight/ODBC
// dependency (no db_model.hpp). Persistence is the in-memory store. This header
// is placed first on the WASM include path so it wins over the native one.

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bank/dto/auth_dto.hpp"
#include "bank/dto/common.hpp"

namespace bank {

/// @brief Manages user identities and authentication (in-memory).
class AuthModel {
public:
    dto::AuthResult execute(const dto::RegisterUser& action);
    dto::AuthResult execute(const dto::LoginRequest& action);
    dto::CommandResult execute(const dto::ChangePassword& action);
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
BRIDGE_REGISTER_ACTION(AuthModel, WhoAmI, "WhoAmI")
