// SPDX-License-Identifier: Apache-2.0
//
// In-memory implementation of AuthModel for the WASM build. Mirrors
// src/models/auth_model.cpp but persists to bank::wasm::Db.

#include <optional>
#include <string>
#include <string_view>

#include "bank/core/demo_hash.hpp"
#include "bank/core/errors.hpp"
#include "bank/core/principal.hpp"
#include "bank/models/auth_model.hpp"
#include "bank/wasm/store.hpp"
#include "bank/wasm/store_ops.hpp"

namespace bank {

namespace {

std::string hashPassword(std::string_view username, std::string_view password) {
    return demoHash(std::string{username} + ":" + std::string{password} + ":morph-bank");
}

std::optional<wasm::UserRow> findUser(wasm::Db& db, const std::string& username) {
    auto rows = db.users.where([&](const wasm::UserRow& u) { return u.username == username; });
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows.front();
}

}  // namespace

dto::AuthResult AuthModel::execute(const dto::RegisterUser& action) {
    if (!action.validate()) {
        throw ValidationError{"username required and password must be at least 4 characters"};
    }
    auto& db = wasm::sharedDb();
    if (findUser(db, action.username).has_value()) {
        return dto::AuthResult{.ok = false, .message = "username already taken"};
    }
    wasm::UserRow rec;
    rec.username = action.username;
    rec.passwordHash = hashPassword(action.username, action.password);
    rec.displayName = action.displayName.empty() ? action.username : action.displayName;
    rec.status = 0;
    db.users.insert(rec);
    return dto::AuthResult{
        .ok = true, .principal = action.username, .displayName = rec.displayName, .message = "registered"};
}

dto::AuthResult AuthModel::execute(const dto::LoginRequest& action) {
    auto& db = wasm::sharedDb();
    auto user = findUser(db, action.username);
    if (!user.has_value()) {
        return dto::AuthResult{.ok = false, .message = "no such user"};
    }
    if (user->status != 0) {
        return dto::AuthResult{.ok = false, .message = "account disabled"};
    }
    if (user->passwordHash != hashPassword(action.username, action.password)) {
        return dto::AuthResult{.ok = false, .message = "invalid credentials"};
    }
    return dto::AuthResult{
        .ok = true, .principal = action.username, .displayName = user->displayName, .message = "welcome"};
}

dto::CommandResult AuthModel::execute(const dto::ChangePassword& action) {
    auto& db = wasm::sharedDb();
    auto user = findUser(db, action.username);
    if (!user.has_value()) {
        throw NotFound{"no such user"};
    }
    if (user->passwordHash != hashPassword(action.username, action.oldPassword)) {
        throw Unauthorized{"current password does not match"};
    }
    if (action.newPassword.size() < 4) {
        throw ValidationError{"new password must be at least 4 characters"};
    }
    user->passwordHash = hashPassword(action.username, action.newPassword);
    db.users.update(*user);
    return dto::CommandResult{.ok = true, .message = "password changed"};
}

dto::SessionInfo AuthModel::execute(const dto::WhoAmI& /*action*/) {
    const std::string principal = sessionPrincipal();
    return dto::SessionInfo{.authenticated = !principal.empty(), .principal = principal};
}

}  // namespace bank
