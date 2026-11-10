// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <optional>
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

/// @brief One published result, to be billed.
struct BillableLineRecord {
    static constexpr std::string_view TableName = "billing_lines";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"site_id"}> siteId{0};                                   // 2
    Light::Field<std::int64_t, Light::SqlRealName{"client_id"}> clientId{0};                               // 3
    Light::Field<std::int64_t, Light::SqlRealName{"sample_id"}> sampleId{0};                               // 4
    Light::Field<std::int64_t, Light::SqlRealName{"result_id"}> resultId{0};                               // 5
    Light::Field<std::int64_t, Light::SqlRealName{"analysis_id"}> analysisId{0};                           // 6
    Light::Field<std::int64_t, Light::SqlRealName{"published_at"}> publishedAt{0};                         // 7
    /// @brief `BillableLineState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};                                // 8
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"amount_num"}> amountNum;  // 9
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"amount_den"}> amountDen;  // 10
    /// @brief `CurrencyCode` as its underlying integer; meaningful only when priced.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};                          // 11
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"invoice_id"}> invoiceId;  // 12
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"event_id"}> eventId;        // 13
};

/// @brief An event billing has consumed, with the line it made: one row per scope and event id.
struct ConsumedEventRecord {
    static constexpr std::string_view TableName = "billing_consumed_events";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"scope"}> scope;                             // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"event_id"}> eventId;                       // 2
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"line"}> line;                               // 3
};

}  // namespace lims::db
