// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

#include "bank/db/account_entity.hpp"
#include "bank/db/payee_entity.hpp"
#include "bank/db/user_entity.hpp"

namespace bank::db {

/// @brief One row of the `payments` table (one-off, scheduled, or standing).
///
/// @note `fromAccount` sits at member index 4 to match `AccountRecord::payments`,
/// and `payee` at index 5 to match `PayeeRecord::payments`.
struct PaymentRecord {
    static constexpr std::string_view TableName = "payments";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// Owning user.
    Light::BelongsTo<&UserRecord::id, Light::SqlRealName{"user_id"}> user;  // 1
    Light::Field<int, Light::SqlRealName{"currency"}> currency;             // 2
    Light::Field<std::int64_t, Light::SqlRealName{"amount_minor"}> amountMinor;  // 3
    /// Account the payment is drawn from.
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"from_account_id"}> fromAccount;  // 4
    /// Beneficiary the payment is addressed to.
    Light::BelongsTo<&PayeeRecord::id, Light::SqlRealName{"payee_id"}> payee;  // 5
    /// `PaymentSchedule` as integer.
    Light::Field<int, Light::SqlRealName{"schedule"}> schedule;  // 6
    /// `PaymentStatus` as integer.
    Light::Field<int, Light::SqlRealName{"status"}> status;  // 7
    /// Due time (epoch ms) for scheduled/standing payments; 0 for one-off.
    Light::Field<std::int64_t, Light::SqlRealName{"due_at_ms"}> dueAtMs{0};  // 8
    /// Recurrence period in days for standing orders; 0 otherwise.
    Light::Field<int, Light::SqlRealName{"interval_days"}> intervalDays{0};  // 9
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"description"}> description;  // 10
};

}  // namespace bank::db
