// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

#include "bank/db/user_entity.hpp"

namespace bank::db {

// "Many" side of the Payee→Payment relation; forward declaration is enough to
// declare the `HasMany<>` member below.
struct PaymentRecord;

/// @brief The `payees` table as a navigable aggregate: scalar columns, the
///        owning-user `BelongsTo`, and the `HasMany<PaymentRecord>` inverse of
///        `PaymentRecord::payee`.
///
/// @note `payments` sits at member index 5 so it matches `PaymentRecord::payee`
/// (its back-referencing `BelongsTo` at index 5).
///
/// @warning As with `UserRecord`, the `HasMany` member means this record only
/// works with `Create`/`Delete`/`QuerySingle`(+navigation). Fluent list queries
/// use the relation-free `PayeeRow` projection below.
struct PayeeRecord {
    static constexpr std::string_view TableName = "payees";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;       // 1
    Light::Field<Light::SqlAnsiString<34>, Light::SqlRealName{"iban"}> iban;         // 2
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"bank_name"}> bankName;  // 3
    /// Owning user.
    Light::BelongsTo<&UserRecord::id, Light::SqlRealName{"user_id"}> user;  // 4
    /// Payments addressed to this payee (inverse of `PaymentRecord::payee`).
    Light::HasMany<PaymentRecord> payments;  // 5
};

/// @brief Relation-free projection over the same `payees` table for fluent
///        queries / updates (see the warning on `PayeeRecord`).
struct PayeeRow {
    static constexpr std::string_view TableName = "payees";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;
    Light::Field<Light::SqlAnsiString<34>, Light::SqlRealName{"iban"}> iban;
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"bank_name"}> bankName;
    Light::BelongsTo<&UserRecord::id, Light::SqlRealName{"user_id"}> user;
};

}  // namespace bank::db
