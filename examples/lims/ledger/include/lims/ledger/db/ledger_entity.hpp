// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <optional>
#include <string_view>

/// @file
/// The ledger's tables, prefixed `ledger_`. Companies are org's: a company id is stored as a plain column, since no
/// module reads another's tables.

namespace lims::db {

/// @brief One account of one company's chart.
struct AccountRecord {
    static constexpr std::string_view TableName = "ledger_accounts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"code"}> code;                               // 2
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 3
    /// @brief `AccountType` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"type"}> type{0};  // 4
};

/// @brief One month of one company's books.
struct PeriodRecord {
    static constexpr std::string_view TableName = "ledger_periods";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    /// @brief Midnight UTC on the month's first day, in epoch milliseconds.
    Light::Field<std::int64_t, Light::SqlRealName{"start_ms"}> startMs{0};  // 2
    /// @brief `PeriodState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};  // 3
};

}  // namespace lims::db
