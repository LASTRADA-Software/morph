// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "bank/core/errors.hpp"
#include "bank/db/user_entity.hpp"

/// @file
/// Helpers that map the session principal (a username string) to the `users`
/// row it identifies. Every owned record references its owner through a
/// `BelongsTo<&UserRecord::id>` foreign key, so the models resolve the
/// principal to a `user_id` here rather than storing the username on each row.

namespace bank::db {

/// @brief Looks up the id of the user with the given @p username, if any.
///
/// Uses the relation-free `UserRow` projection: `UserRecord` carries a
/// `HasMany`, which the fluent `Query<>()` builder can't select in this
/// Lightweight version (see `UserRecord`'s warning).
[[nodiscard]] inline std::optional<std::uint64_t> findUserId(Lightweight::DataMapper& mapper,
                                                             std::string_view username) {
    auto row = mapper.Query<UserRow>()
                   .Where(Lightweight::FieldNameOf<&UserRow::username>, "=", std::string{username})
                   .First();
    if (!row.has_value()) {
        return std::nullopt;
    }
    return row->id.Value();
}

/// @brief Resolves @p username to its user id, or throws if no such user exists.
/// @throws Unauthorized when the principal has no backing `users` row.
[[nodiscard]] inline std::uint64_t requireUserId(Lightweight::DataMapper& mapper, std::string_view username) {
    if (auto id = findUserId(mapper, username); id.has_value()) {
        return *id;
    }
    throw Unauthorized{"unknown user: " + std::string{username}};
}

/// @brief Unwraps a relation access on a record this model just read.
///
/// The record came from a live query on the current connection, so an
/// unavailable relation means the database failed under us, not that the
/// caller did something wrong.
/// @tparam T What the relation access yields.
/// @param result The relation accessor's result.
/// @param relation The relation's name, for the message.
/// @return The loaded value.
/// @throws BankError when the relation could not be loaded.
template <typename T>
[[nodiscard]] T requireLoaded(Lightweight::RelationResult<T> result, std::string_view relation) {
    if (!result.has_value()) {
        throw BankError{std::format("{} could not be loaded: {}", relation, Lightweight::to_string(result.error()))};
    }
    return std::move(*result);
}

/// @brief Returns the id of the user named @p username, creating a minimal row
///        if none exists yet (demo convenience used by `App::login`).
///
/// A real app would require explicit registration; here, "logging in" as a
/// principal provisions it so example flows and tests don't each have to
/// register first. The password hash is left empty — `AuthModel` owns real
/// credential handling for users created through registration.
inline std::uint64_t ensureUser(Lightweight::DataMapper& mapper, std::string_view username,
                                std::string_view displayName = {}) {
    if (auto id = findUserId(mapper, username); id.has_value()) {
        return *id;
    }
    UserRow rec;
    rec.username = Light::SqlAnsiString<64>{std::string{username}};
    rec.passwordHash = Light::SqlAnsiString<32>{};
    rec.displayName = Light::SqlAnsiString<128>{std::string{displayName.empty() ? username : displayName}};
    rec.status = 0;
    mapper.Create(rec);
    return rec.id.Value();
}

}  // namespace bank::db
