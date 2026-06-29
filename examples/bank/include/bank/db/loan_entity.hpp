// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

namespace bank::db {

/// @brief One row of the `loans` table.
struct LoanRecord {
    static constexpr std::string_view TableName = "loans";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner"}> owner;
    /// Account the loan was disbursed into / is repaid from.
    Light::Field<std::int64_t, Light::SqlRealName{"account_id"}> accountId;
    Light::Field<std::int64_t, Light::SqlRealName{"principal_minor"}> principalMinor;
    Light::Field<std::int64_t, Light::SqlRealName{"outstanding_minor"}> outstandingMinor;
    Light::Field<int, Light::SqlRealName{"currency"}> currency;
    /// Annual interest rate in basis points.
    Light::Field<int, Light::SqlRealName{"rate_bps"}> rateBps;
    Light::Field<int, Light::SqlRealName{"term_months"}> termMonths;
    /// `LoanStatus` as integer.
    Light::Field<int, Light::SqlRealName{"status"}> status{0};
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs;
};

}  // namespace bank::db
