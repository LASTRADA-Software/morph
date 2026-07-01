// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

#include "bank/db/user_entity.hpp"

/// @file
/// Lightweight entity for the `accounts` table. This is the *persistence* shape
/// — `Light::Field<>`/`Light::BelongsTo<>`-wrapped members mapped to columns —
/// and is distinct from the wire DTOs in `bank/dto/`. Models translate between
/// the two.

namespace bank::db {

/// @brief One row of the `accounts` table.
///
/// @note `user` sits at member index 5 to match `UserRecord::accounts` (the
/// parent's `HasMany` at index 5) — Lightweight resolves a `HasMany<Child>` by
/// the child column at the *same ordinal member index* as the relation field.
///
/// AccountRecord intentionally carries no inverse `HasMany` of its own: rows are
/// updated on every balance change, and (in this Lightweight version's
/// non-reflection build) `DataMapper::Update` cannot be instantiated for a
/// record that has a `HasMany` member. Children are reached via their
/// `account_id` foreign key instead (see e.g. `TransactionModel::History`).
struct AccountRecord {
    static constexpr std::string_view TableName = "accounts";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// Generated account number (also used as the IBAN-ish identifier).
    Light::Field<Light::SqlAnsiString<34>, Light::SqlRealName{"number"}> number;  // 1
    /// `AccountKind` as integer.
    Light::Field<int, Light::SqlRealName{"kind"}> kind;  // 2
    /// `Currency` as integer.
    Light::Field<int, Light::SqlRealName{"currency"}> currency;  // 3
    /// Current balance in minor units.
    Light::Field<std::int64_t, Light::SqlRealName{"balance_minor"}> balanceMinor{0};  // 4
    /// Owning user.
    Light::BelongsTo<&UserRecord::id, Light::SqlRealName{"user_id"}> user;  // 5
    /// Allowed overdraft (positive number of minor units below zero) for the account.
    Light::Field<std::int64_t, Light::SqlRealName{"overdraft_minor"}> overdraftMinor{0};  // 6
    /// `AccountStatus` as integer.
    Light::Field<int, Light::SqlRealName{"status"}> status{0};  // 7
    /// Annual interest rate in basis points (1% = 100 bps).
    Light::Field<int, Light::SqlRealName{"interest_bps"}> interestBps{0};  // 8
};

}  // namespace bank::db
