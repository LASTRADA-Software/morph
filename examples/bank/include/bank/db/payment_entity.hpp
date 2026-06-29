// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

namespace bank::db {

/// @brief One row of the `payments` table (one-off, scheduled, or standing).
struct PaymentRecord {
    static constexpr std::string_view TableName = "payments";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner"}> owner;
    Light::Field<std::int64_t, Light::SqlRealName{"from_account_id"}> fromAccountId;
    Light::Field<std::int64_t, Light::SqlRealName{"payee_id"}> payeeId;
    Light::Field<std::int64_t, Light::SqlRealName{"amount_minor"}> amountMinor;
    Light::Field<int, Light::SqlRealName{"currency"}> currency;
    /// `PaymentSchedule` as integer.
    Light::Field<int, Light::SqlRealName{"schedule"}> schedule;
    /// `PaymentStatus` as integer.
    Light::Field<int, Light::SqlRealName{"status"}> status;
    /// Due time (epoch ms) for scheduled/standing payments; 0 for one-off.
    Light::Field<std::int64_t, Light::SqlRealName{"due_at_ms"}> dueAtMs{0};
    /// Recurrence period in days for standing orders; 0 otherwise.
    Light::Field<int, Light::SqlRealName{"interval_days"}> intervalDays{0};
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"description"}> description;
};

}  // namespace bank::db
