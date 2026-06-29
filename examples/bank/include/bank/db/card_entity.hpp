// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

namespace bank::db {

/// @brief One row of the `cards` table.
struct CardRecord {
    static constexpr std::string_view TableName = "cards";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner"}> owner;
    Light::Field<std::int64_t, Light::SqlRealName{"account_id"}> accountId;
    /// `CardKind` as integer.
    Light::Field<int, Light::SqlRealName{"kind"}> kind;
    /// Last four digits of the (fictional) card number.
    Light::Field<Light::SqlAnsiString<4>, Light::SqlRealName{"pan_last4"}> panLast4;
    /// `CardStatus` as integer.
    Light::Field<int, Light::SqlRealName{"status"}> status{0};
    /// Daily spend limit in minor units (0 = no limit).
    Light::Field<std::int64_t, Light::SqlRealName{"daily_limit_minor"}> dailyLimitMinor{0};
    /// Hash of the card PIN (demo-grade).
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"pin_hash"}> pinHash;
};

}  // namespace bank::db
