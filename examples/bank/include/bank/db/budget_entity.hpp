// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>
#include <cstdint>
#include <string_view>

#include "bank/db/user_entity.hpp"

namespace bank::db {

/// @brief One row of the `budgets` table (a per-category monthly limit).
struct BudgetRecord {
    static constexpr std::string_view TableName = "budgets";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// Owning user.
    Light::BelongsTo<&UserRecord::id, Light::SqlRealName{"user_id"}> user;                    // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"category"}> category;          // 2
    Light::Field<std::int64_t, Light::SqlRealName{"monthly_limit_minor"}> monthlyLimitMinor;  // 3
    Light::Field<int, Light::SqlRealName{"currency"}> currency;                               // 4
};

}  // namespace bank::db
