// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

#include "bank/db/account_entity.hpp"

namespace bank::db {

/// @brief One row of the `transactions` ledger table.
///
/// @note `account` sits at member index 3 to match `AccountRecord::transactions`
/// (the parent's `HasMany` at index 3). `counterparty` is a second, *nullable*
/// `BelongsTo` to the same `accounts` table — used only by transfers — and is
/// intentionally NOT the relation `AccountRecord::transactions` resolves
/// against, so a transfer's counterparty never shows up in the other account's
/// ledger.
struct TxnRecord {
    static constexpr std::string_view TableName = "transactions";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// The other account in a transfer; NULL for deposits/withdrawals.
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"counterparty_id"}, Light::SqlNullable::Null>
        counterparty;  // 1
    /// `TxnDirection` as integer.
    Light::Field<int, Light::SqlRealName{"direction"}> direction;  // 2
    /// Account this ledger entry belongs to.
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;  // 3
    /// `TxnKind` as integer.
    Light::Field<int, Light::SqlRealName{"kind"}> kind;  // 4
    /// Absolute amount moved, in minor units.
    Light::Field<std::int64_t, Light::SqlRealName{"amount_minor"}> amountMinor;  // 5
    /// `Currency` as integer.
    Light::Field<int, Light::SqlRealName{"currency"}> currency;  // 6
    /// Account balance immediately after this entry, in minor units.
    Light::Field<std::int64_t, Light::SqlRealName{"balance_after_minor"}> balanceAfterMinor;  // 7
    /// Free-text memo.
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"description"}> description;  // 8
    /// Creation time as Unix epoch milliseconds (used for ordering/display).
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs;  // 9
};

}  // namespace bank::db
