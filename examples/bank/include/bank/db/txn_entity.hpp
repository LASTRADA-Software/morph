// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

namespace bank::db {

/// @brief One row of the `transactions` ledger table.
struct TxnRecord {
    static constexpr std::string_view TableName = "transactions";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    /// Account this ledger entry belongs to.
    Light::Field<std::int64_t, Light::SqlRealName{"account_id"}> accountId;
    /// The other account in a transfer (0 if none).
    Light::Field<std::int64_t, Light::SqlRealName{"counterparty_id"}> counterpartyId{0};
    /// `TxnDirection` as integer.
    Light::Field<int, Light::SqlRealName{"direction"}> direction;
    /// `TxnKind` as integer.
    Light::Field<int, Light::SqlRealName{"kind"}> kind;
    /// Absolute amount moved, in minor units.
    Light::Field<std::int64_t, Light::SqlRealName{"amount_minor"}> amountMinor;
    /// `Currency` as integer.
    Light::Field<int, Light::SqlRealName{"currency"}> currency;
    /// Account balance immediately after this entry, in minor units.
    Light::Field<std::int64_t, Light::SqlRealName{"balance_after_minor"}> balanceAfterMinor;
    /// Free-text memo.
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"description"}> description;
    /// Creation time as Unix epoch milliseconds (used for ordering/display).
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs;
};

}  // namespace bank::db
