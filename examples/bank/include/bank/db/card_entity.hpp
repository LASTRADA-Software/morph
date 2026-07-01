// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

#include "bank/db/account_entity.hpp"
#include "bank/db/user_entity.hpp"

namespace bank::db {

/// @brief One row of the `cards` table.
///
/// @note `account` sits at member index 1 to match `AccountRecord::cards`.
struct CardRecord {
    static constexpr std::string_view TableName = "cards";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// Account this card draws on.
    Light::BelongsTo<&AccountRecord::id, Light::SqlRealName{"account_id"}> account;  // 1
    /// Owning user.
    Light::BelongsTo<&UserRecord::id, Light::SqlRealName{"user_id"}> user;  // 2
    /// `CardKind` as integer.
    Light::Field<int, Light::SqlRealName{"kind"}> kind;  // 3
    /// Last four digits of the (fictional) card number.
    Light::Field<Light::SqlAnsiString<4>, Light::SqlRealName{"pan_last4"}> panLast4;  // 4
    /// `CardStatus` as integer.
    Light::Field<int, Light::SqlRealName{"status"}> status{0};  // 5
    /// Daily spend limit in minor units (0 = no limit).
    Light::Field<std::int64_t, Light::SqlRealName{"daily_limit_minor"}> dailyLimitMinor{0};  // 6
    /// Hash of the card PIN (demo-grade).
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"pin_hash"}> pinHash;  // 7
};

}  // namespace bank::db
