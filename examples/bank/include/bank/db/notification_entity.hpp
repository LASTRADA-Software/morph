// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

#include "bank/db/user_entity.hpp"

namespace bank::db {

/// @brief One row of the `notifications` table.
struct NotificationRecord {
    static constexpr std::string_view TableName = "notifications";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// Owning user.
    Light::BelongsTo<&UserRecord::id, Light::SqlRealName{"user_id"}> user;  // 1
    /// 0 = info, 1 = warning, 2 = alert.
    Light::Field<int, Light::SqlRealName{"severity"}> severity{0};  // 2
    Light::Field<Light::SqlAnsiString<256>, Light::SqlRealName{"message"}> message;  // 3
    Light::Field<bool, Light::SqlRealName{"is_read"}> read{false};  // 4
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs;  // 5
};

}  // namespace bank::db
