// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

namespace bank::db {

/// @brief One row of the `budgets` table (a per-category monthly limit).
struct BudgetRecord {
    static constexpr std::string_view TableName = "budgets";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner"}> owner;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"category"}> category;
    Light::Field<std::int64_t, Light::SqlRealName{"monthly_limit_minor"}> monthlyLimitMinor;
    Light::Field<int, Light::SqlRealName{"currency"}> currency;
};

}  // namespace bank::db
