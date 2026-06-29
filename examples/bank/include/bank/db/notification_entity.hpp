// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

namespace bank::db {

/// @brief One row of the `notifications` table.
struct NotificationRecord {
    static constexpr std::string_view TableName = "notifications";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner"}> owner;
    /// 0 = info, 1 = warning, 2 = alert.
    Light::Field<int, Light::SqlRealName{"severity"}> severity{0};
    Light::Field<Light::SqlAnsiString<256>, Light::SqlRealName{"message"}> message;
    Light::Field<bool, Light::SqlRealName{"is_read"}> read{false};
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs;
};

}  // namespace bank::db
