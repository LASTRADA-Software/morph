// SPDX-License-Identifier: Apache-2.0

#include "bank/models/auth_model.hpp"

#include <Lightweight/Lightweight.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/db/user_entity.hpp"

namespace bank {

namespace {

/// Demo-grade password hashing. NOT secure — a real app would use a slow,
/// salted KDF (Argon2/bcrypt). Salting with the username keeps identical
/// passwords from colliding across users.
std::string hashPassword(std::string_view username, std::string_view password) {
    const std::string material = std::string{username} + ":" + std::string{password} + ":morph-bank";
    const std::size_t digest = std::hash<std::string>{}(material);
    return std::format("{:016x}", digest);
}

/// Finds a user by username, or std::nullopt.
std::optional<db::UserRecord> findUser(Lightweight::DataMapper& mapper, const std::string& username) {
    auto rows = mapper.Query<db::UserRecord>()
                    .Where(Lightweight::FieldNameOf<&db::UserRecord::username>, "=", username)
                    .All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return std::move(rows.front());
}

}  // namespace

dto::AuthResult AuthModel::execute(const dto::RegisterUser& action) {
    if (!action.validate()) {
        throw ValidationError{"username required and password must be at least 4 characters"};
    }
    if (findUser(mapper(), action.username).has_value()) {
        return dto::AuthResult{.ok = false, .message = "username already taken"};
    }

    db::UserRecord rec;
    rec.username = Light::SqlAnsiString<64>{action.username};
    rec.passwordHash = Light::SqlAnsiString<32>{hashPassword(action.username, action.password)};
    rec.displayName =
        Light::SqlAnsiString<128>{action.displayName.empty() ? action.username : action.displayName};
    rec.status = 0;
    mapper().Create(rec);

    return dto::AuthResult{.ok = true,
                           .principal = action.username,
                           .displayName = std::string{rec.displayName.Value().str()},
                           .message = "registered"};
}

dto::AuthResult AuthModel::execute(const dto::LoginRequest& action) {
    auto user = findUser(mapper(), action.username);
    if (!user.has_value()) {
        return dto::AuthResult{.ok = false, .message = "no such user"};
    }
    if (user->status.Value() != 0) {
        return dto::AuthResult{.ok = false, .message = "account disabled"};
    }
    const std::string expected = hashPassword(action.username, action.password);
    if (std::string{user->passwordHash.Value().str()} != expected) {
        return dto::AuthResult{.ok = false, .message = "invalid credentials"};
    }
    return dto::AuthResult{.ok = true,
                           .principal = action.username,
                           .displayName = std::string{user->displayName.Value().str()},
                           .message = "welcome"};
}

dto::CommandResult AuthModel::execute(const dto::ChangePassword& action) {
    auto user = findUser(mapper(), action.username);
    if (!user.has_value()) {
        throw NotFound{"no such user"};
    }
    if (std::string{user->passwordHash.Value().str()} != hashPassword(action.username, action.oldPassword)) {
        throw Unauthorized{"current password does not match"};
    }
    if (action.newPassword.size() < 4) {
        throw ValidationError{"new password must be at least 4 characters"};
    }
    user->passwordHash = Light::SqlAnsiString<32>{hashPassword(action.username, action.newPassword)};
    mapper().Update(*user);
    return dto::CommandResult{.ok = true, .message = "password changed"};
}

dto::SessionInfo AuthModel::execute(const dto::WhoAmI& /*action*/) {
    const std::string principal = sessionPrincipal();
    return dto::SessionInfo{.authenticated = !principal.empty(), .principal = principal};
}

}  // namespace bank
