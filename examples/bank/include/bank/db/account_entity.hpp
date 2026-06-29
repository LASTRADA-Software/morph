// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

/// @file
/// Lightweight entity for the `accounts` table. This is the *persistence* shape
/// — `Light::Field<>`-wrapped members mapped to columns — and is distinct from
/// the wire DTOs in `bank/dto/`. Models translate between the two.

namespace bank::db {

/// @brief One row of the `accounts` table.
struct AccountRecord {
    static constexpr std::string_view TableName = "accounts";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    /// Owning principal (the logged-in user's session principal).
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner"}> owner;
    /// Generated account number (also used as the IBAN-ish identifier).
    Light::Field<Light::SqlAnsiString<34>, Light::SqlRealName{"number"}> number;
    /// `AccountKind` as integer.
    Light::Field<int, Light::SqlRealName{"kind"}> kind;
    /// `Currency` as integer.
    Light::Field<int, Light::SqlRealName{"currency"}> currency;
    /// Current balance in minor units.
    Light::Field<std::int64_t, Light::SqlRealName{"balance_minor"}> balanceMinor{0};
    /// Allowed overdraft (positive number of minor units below zero) for the account.
    Light::Field<std::int64_t, Light::SqlRealName{"overdraft_minor"}> overdraftMinor{0};
    /// `AccountStatus` as integer.
    Light::Field<int, Light::SqlRealName{"status"}> status{0};
    /// Annual interest rate in basis points (1% = 100 bps).
    Light::Field<int, Light::SqlRealName{"interest_bps"}> interestBps{0};
};

}  // namespace bank::db
