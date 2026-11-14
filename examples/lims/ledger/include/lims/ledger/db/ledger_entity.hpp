// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "lims/ledger/core/types.hpp"
#include "lims/org/core/errors.hpp"

/// @file
/// The ledger's tables, prefixed `ledger_`. Companies are org's: a company id is stored as a plain column, since no
/// module reads another's tables.

namespace lims::db {

/// @brief One account of one company's chart.
struct AccountRecord {
    static constexpr std::string_view TableName = "ledger_accounts";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"code"}> code;                               // 2
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 3
    /// @brief `AccountType` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"type"}> type{0};  // 4
};

/// @brief One month of one company's books.
struct PeriodRecord {
    static constexpr std::string_view TableName = "ledger_periods";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    /// @brief Midnight UTC on the month's first day, in epoch milliseconds.
    Light::Field<std::int64_t, Light::SqlRealName{"start_ms"}> startMs{0};  // 2
    /// @brief `PeriodState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};  // 3
};

/// @brief One journal entry's header.
struct EntryRecord {
    static constexpr std::string_view TableName = "ledger_entries";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    Light::Field<std::int64_t, Light::SqlRealName{"period_id"}> period{0};                                 // 2
    Light::Field<std::int64_t, Light::SqlRealName{"date_ms"}> dateMs{0};                                   // 3
    /// @brief `EntryState` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"state"}> state{0};                                          // 4
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"source_key"}> sourceKey;              // 5
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"source_reference"}> sourceReference;  // 6
    Light::Field<Light::SqlAnsiString<255>, Light::SqlRealName{"memo"}> memo;                         // 7
    /// @brief `CurrencyCode` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"currency"}> currency{0};                                 // 8
    Light::Field<std::int64_t, Light::SqlRealName{"rate_num"}> rateNum{1};                         // 9
    Light::Field<std::int64_t, Light::SqlRealName{"rate_den"}> rateDen{1};                         // 10
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"reverses_id"}> reversesId;       // 11
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"reversed_by_id"}> reversedById;  // 12
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"created_by"}> createdBy;            // 13
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"posted_by"}> postedBy;              // 14
};

/// @brief One line of one entry.
struct EntryLineRecord {
    static constexpr std::string_view TableName = "ledger_entry_lines";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"entry_id"}> entry{0};                                   // 1
    Light::Field<int, Light::SqlRealName{"position"}> position{0};                                         // 2
    Light::Field<std::int64_t, Light::SqlRealName{"account_id"}> account{0};                               // 3
    /// @brief `PostingSide` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"side"}> side{0};                        // 4
    Light::Field<std::int64_t, Light::SqlRealName{"amount_num"}> amountNum{0};    // 5
    Light::Field<std::int64_t, Light::SqlRealName{"amount_den"}> amountDen{1};    // 6
    Light::Field<std::int64_t, Light::SqlRealName{"company_num"}> companyNum{0};  // 7
    Light::Field<std::int64_t, Light::SqlRealName{"company_den"}> companyDen{1};  // 8
};

/// @brief A poster's source key, recorded once per company with the entry it produced.
struct PostingSourceRecord {
    static constexpr std::string_view TableName = "ledger_posting_sources";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// @brief The company id as text: the replay ledger's scope.
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"scope"}> scope;            // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"source_key"}> sourceKey;  // 2
    /// @brief The entry's id as text: the replay ledger's payload.
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"entry"}> entry;  // 3
};

/// @brief One account's frozen totals in one closed period, in company currency.
struct PeriodBalanceRecord {
    static constexpr std::string_view TableName = "ledger_period_balances";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"period_id"}> period{0};                                 // 1
    Light::Field<std::int64_t, Light::SqlRealName{"account_id"}> account{0};                               // 2
    Light::Field<std::int64_t, Light::SqlRealName{"debit_num"}> debitNum{0};                               // 3
    Light::Field<std::int64_t, Light::SqlRealName{"debit_den"}> debitDen{1};                               // 4
    Light::Field<std::int64_t, Light::SqlRealName{"credit_num"}> creditNum{0};                             // 5
    Light::Field<std::int64_t, Light::SqlRealName{"credit_den"}> creditDen{1};                             // 6
};

/// @brief One dated rate of one currency pair for one company.
struct ExchangeRateRecord {
    static constexpr std::string_view TableName = "ledger_exchange_rates";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::int64_t, Light::SqlRealName{"company_id"}> companyId{0};                             // 1
    /// @brief `CurrencyCode` of the transaction currency as its underlying integer.
    Light::Field<int, Light::SqlRealName{"from_currency"}> fromCurrency{0};  // 2
    /// @brief `CurrencyCode` of the company-side currency as its underlying integer.
    Light::Field<int, Light::SqlRealName{"to_currency"}> toCurrency{0};  // 3
    /// @brief The first instant the rate applies, in epoch milliseconds.
    Light::Field<std::int64_t, Light::SqlRealName{"valid_from_ms"}> validFromMs{0};  // 4
    Light::Field<std::int64_t, Light::SqlRealName{"rate_num"}> rateNum{1};           // 5
    Light::Field<std::int64_t, Light::SqlRealName{"rate_den"}> rateDen{1};           // 6
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"set_by"}> setBy;      // 7
};

/// @brief The account type a row stores.
/// @param row The row.
/// @return Its type.
/// @throws ValidationError if the row stores a type outside its enumeration.
[[nodiscard]] inline AccountType accountTypeOf(AccountRecord const& row) {
    int const type = row.type.Value();
    if (type < 0 || type > static_cast<int>(AccountType::Expense)) {
        throw ValidationError{"account " + std::to_string(row.id.Value()) + " stores a type outside its range"};
    }
    return static_cast<AccountType>(type);
}

}  // namespace lims::db
