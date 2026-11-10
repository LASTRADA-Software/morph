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

/// @brief One invoice.
struct InvoiceRecord {
    static constexpr std::string_view TableName = "billing_invoices";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"site_id"}> siteId{0};                                   // 2
    Light::Field<std::int64_t, Light::SqlRealName{"client_id"}> clientId{0};                               // 3
    /// @brief `CurrencyCode` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};  // 4
    /// @brief `InvoiceState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};                                 // 5
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"number"}> number;             // 6
    Light::Field<std::int64_t, Light::SqlRealName{"net_num"}> netNum{0};                     // 7
    Light::Field<std::int64_t, Light::SqlRealName{"net_den"}> netDen{1};                     // 8
    Light::Field<std::int64_t, Light::SqlRealName{"tax_num"}> taxNum{0};                     // 9
    Light::Field<std::int64_t, Light::SqlRealName{"tax_den"}> taxDen{1};                     // 10
    Light::Field<std::int64_t, Light::SqlRealName{"gross_num"}> grossNum{0};                 // 11
    Light::Field<std::int64_t, Light::SqlRealName{"gross_den"}> grossDen{1};                 // 12
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"reference"}> reference;      // 13
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"created_by"}> createdBy;      // 14
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"submitted_by"}> submittedBy;  // 15
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"approved_by"}> approvedBy;    // 16
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"posted_by"}> postedBy;        // 17
    /// @brief Epoch milliseconds; 0 until it happens.
    Light::Field<std::int64_t, Light::SqlRealName{"created_at"}> createdAt{0};      // 18
    Light::Field<std::int64_t, Light::SqlRealName{"submitted_at"}> submittedAt{0};  // 19
    Light::Field<std::int64_t, Light::SqlRealName{"deadline_at"}> deadlineAt{0};    // 20
    /// @brief The gross in the company's currency, set at submission.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"gross_company_num"}> grossCompanyNum;  // 21
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"gross_company_den"}> grossCompanyDen;  // 22
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"entry_id"}> entryId;                   // 23
};

/// @brief An event billing has consumed, with the line it made: one row per scope and event id.
struct ConsumedEventRecord {
    static constexpr std::string_view TableName = "billing_consumed_events";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"scope"}> scope;                             // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"event_id"}> eventId;                       // 2
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"line"}> line;                               // 3
};

/// @brief One billing run: a company's invoices for one calendar month.
struct BillingRunRecord {
    static constexpr std::string_view TableName = "billing_runs";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    /// @brief The month, `YYYY-MM`.
    Light::Field<Light::SqlAnsiString<7>, Light::SqlRealName{"label"}> label;  // 2
    /// @brief `BillingRunState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};                             // 3
    Light::Field<std::int64_t, Light::SqlRealName{"started_at"}> startedAt{0};           // 4
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"started_by"}> startedBy;  // 5
};

/// @brief One (client, site) a run has to draft an invoice for: the run's cursor.
struct BillingRunItemRecord {
    static constexpr std::string_view TableName = "billing_run_items";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"run_id"}> runId{0};                                     // 1
    Light::Field<std::int64_t, Light::SqlRealName{"client_id"}> clientId{0};                               // 2
    Light::Field<std::int64_t, Light::SqlRealName{"site_id"}> siteId{0};                                   // 3
    /// @brief 0 while pending, 1 once drafted, 2 once it failed.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};  // 4
    /// @brief The invoice drafted for it; empty when the item was done and no line was left to invoice.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"invoice_id"}> invoiceId;  // 5
    /// @brief The refusal that failed the item; empty otherwise.
    Light::Field<std::optional<Light::SqlAnsiString<255>>, Light::SqlRealName{"error"}> error;  // 6
};

/// @brief The lease of one draft invoice: who may edit it, and which editing instance holds it.
struct InvoiceLeaseRecord {
    static constexpr std::string_view TableName = "billing_invoice_leases";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"invoice_id"}> invoiceId{0};                             // 1
    /// @brief The principal who took the lease.
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"holder"}> holder;  // 2
    /// @brief Names the editing instance, so a second instance of one principal is another editor.
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"token"}> token;  // 3
    /// @brief Epoch milliseconds the lease lapses at.
    Light::Field<std::int64_t, Light::SqlRealName{"expires_at"}> expiresAt{0};  // 4
};

/// @brief The next invoice number of one company's fiscal year.
struct NumberSeriesRecord {
    static constexpr std::string_view TableName = "billing_number_series";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    /// @brief The calendar year the fiscal year begins in.
    Light::Field<std::int64_t, Light::SqlRealName{"fiscal_year"}> fiscalYear{0};  // 2
    /// @brief The number the next invoice takes.
    Light::Field<std::int64_t, Light::SqlRealName{"next_number"}> nextNumber{1};  // 3
};

}  // namespace lims::db
