// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

#include "bank/db/account_entity.hpp"
#include "bank/db/user_entity.hpp"

namespace bank::db {

/// @brief One row of the `loans` table.
///
/// @note `account` sits at member index 2 to match `AccountRecord::loans`.
struct LoanRecord {
    static constexpr std::string_view TableName = "loans";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// Owning user.
    Light::BelongsTo<&UserRecord::id, Light::SqlRealName{"user_id"}> user;  // 1
    /// Account the loan was disbursed into / is repaid from.
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;  // 2
    Light::Field<std::int64_t, Light::SqlRealName{"principal_minor"}> principalMinor;  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"outstanding_minor"}> outstandingMinor;  // 4
    Light::Field<int, Light::SqlRealName{"currency"}> currency;  // 5
    /// Annual interest rate in basis points.
    Light::Field<int, Light::SqlRealName{"rate_bps"}> rateBps;  // 6
    Light::Field<int, Light::SqlRealName{"term_months"}> termMonths;  // 7
    /// `LoanStatus` as integer.
    Light::Field<int, Light::SqlRealName{"status"}> status{0};  // 8
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs;  // 9
};

}  // namespace bank::db
