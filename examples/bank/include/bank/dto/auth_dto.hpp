// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// Wire DTOs for the Auth model: registration, login, password change, and a
/// session-introspection action.

namespace bank::dto {

/// @brief Create a new user account.
struct RegisterUser {
    std::string username;
    std::string password;
    std::string displayName;

    [[nodiscard]] bool validate() const { return !username.empty() && password.size() >= 4; }
};

/// @brief Authenticate a user.
struct LoginRequest {
    std::string username;
    std::string password;
};

/// @brief Result of register/login: on success carries the principal to install.
struct AuthResult {
    bool ok = false;
    std::string principal;  ///< the username to use as the session principal
    std::string displayName;
    std::string message;
};

/// @brief Change a user's password (requires the current password).
struct ChangePassword {
    std::string username;
    std::string oldPassword;
    std::string newPassword;
};

/// @brief Introspect the current session (no inputs).
struct WhoAmI {};

/// @brief Result of `WhoAmI`.
struct SessionInfo {
    bool authenticated = false;
    std::string principal;
};

}  // namespace bank::dto
