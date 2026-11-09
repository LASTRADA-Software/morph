// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <string_view>

/// @file
/// Billing's tables, prefixed `billing_`. Companies, clients and analyses are other modules' rows: their ids are
/// stored as plain columns, since no module reads another's tables.

namespace lims::db {

/// @brief One company's billing settings.
struct BillingSettingsRecord {
    static constexpr std::string_view TableName = "billing_settings";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"receivable_account"}> receivableAccount;    // 2
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"revenue_account"}> revenueAccount;          // 3
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"tax_account"}> taxAccount;                  // 4
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"rounding_account"}> roundingAccount;        // 5
    Light::Field<std::int64_t, Light::SqlRealName{"tax_rate_num"}> taxRateNum{0};                          // 6
    Light::Field<std::int64_t, Light::SqlRealName{"tax_rate_den"}> taxRateDen{1};                          // 7
    Light::Field<std::int64_t, Light::SqlRealName{"approval_hours"}> approvalHours{0};                     // 8
    Light::Field<std::int64_t, Light::SqlRealName{"billing_day"}> billingDay{0};                           // 9
};

/// @brief One client registered for billing at one company.
struct BillingClientRecord {
    static constexpr std::string_view TableName = "billing_clients";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"client_id"}> clientId{0};                               // 1
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 2
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 3
    /// @brief `CurrencyCode` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};  // 4
};

/// @brief One price list of one company.
struct PriceListRecord {
    static constexpr std::string_view TableName = "billing_price_lists";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 2
    /// @brief `CurrencyCode` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};  // 3
    /// @brief The first instant the list applies, in epoch milliseconds.
    Light::Field<std::int64_t, Light::SqlRealName{"valid_from_ms"}> validFromMs{0};  // 4
};

/// @brief One price in one list.
struct PriceRecord {
    static constexpr std::string_view TableName = "billing_prices";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"price_list_id"}> priceList{0};                          // 1
    Light::Field<std::int64_t, Light::SqlRealName{"analysis_id"}> analysisId{0};                           // 2
    /// @brief The client the price is for; 0 for the list's own price, so that the unique index covers both kinds.
    Light::Field<std::int64_t, Light::SqlRealName{"client_id"}> clientId{0};    // 3
    Light::Field<std::int64_t, Light::SqlRealName{"amount_num"}> amountNum{0};  // 4
    Light::Field<std::int64_t, Light::SqlRealName{"amount_den"}> amountDen{1};  // 5
};

}  // namespace lims::db
