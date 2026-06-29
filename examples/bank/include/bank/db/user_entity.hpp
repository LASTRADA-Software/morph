// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

namespace bank::db {

/// @brief One row of the `users` table.
struct UserRecord {
    static constexpr std::string_view TableName = "users";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    /// Login name; also the session principal. Unique.
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"username"}> username;
    /// Salted hash of the password (demo-grade, not real crypto).
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"password_hash"}> passwordHash;
    /// Human-friendly display name.
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"display_name"}> displayName;
    /// 0 = active, 1 = disabled.
    Light::Field<int, Light::SqlRealName{"status"}> status{0};
};

}  // namespace bank::db
