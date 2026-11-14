// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/ledger/core/types.hpp"
#include "lims/ledger/dto/entry_dto.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/core/types.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// The ledger's reports, as the wire sees them. Every figure is in the company's currency and totals posted entries
/// only; a total that does not fit is refused, never clamped.

namespace lims {

/// @brief Every account's debit and credit totals up to an instant.
struct TrialBalance {
    /// @brief The company.
    CompanyId company;
    /// @brief The last instant counted.
    ::morph::time::Timestamp asOf;

    /// @brief Whether the query is well-formed.
    /// @return `true` when a company and an instant are named.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue() && asOf.hasValue(); }
};

/// @brief One account's row of a trial balance.
struct TrialBalanceRow {
    /// @brief The account's code.
    std::string accountCode;
    /// @brief The account's name.
    std::string accountName;
    /// @brief What the account records.
    AccountType type = AccountType::Asset;
    /// @brief Its debit total.
    Amount debit;
    /// @brief Its credit total.
    Amount credit;
};

/// @brief A trial balance: the accounts that moved, by code, and the totals of the whole company.
struct TrialBalanceView {
    /// @brief The company.
    CompanyId company;
    /// @brief The last instant counted.
    ::morph::time::Timestamp asOf;
    /// @brief The accounts with any posted line, by code.
    std::vector<TrialBalanceRow> rows;
    /// @brief The debit total of every row.
    Amount totalDebit;
    /// @brief The credit total of every row.
    Amount totalCredit;
};

/// @brief One account's totals and balance up to an instant.
struct AccountBalance {
    /// @brief The company.
    CompanyId company;
    /// @brief The account's code.
    std::string accountCode;
    /// @brief The last instant counted.
    ::morph::time::Timestamp asOf;

    /// @brief Whether the query is well-formed.
    /// @return `true` when a company, an account code and an instant are named.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && !accountCode.empty() && asOf.hasValue();
    }
};

/// @brief One account's totals and balance.
struct AccountBalanceView {
    /// @brief The account's code.
    std::string accountCode;
    /// @brief Its debit total.
    Amount debit;
    /// @brief Its credit total.
    Amount credit;
    /// @brief Debit less credit.
    Amount balance;
};

/// @brief One account's posted lines between two instants, with a running balance.
struct AccountLedger {
    /// @brief The company.
    CompanyId company;
    /// @brief The account's code.
    std::string accountCode;
    /// @brief The first instant listed.
    ::morph::time::Timestamp from;
    /// @brief The last instant listed.
    ::morph::time::Timestamp to;

    /// @brief Whether the query is well-formed.
    /// @return `true` when a company, an account code and both instants are named.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && !accountCode.empty() && from.hasValue() && to.hasValue();
    }
};

/// @brief One posted line of an account.
struct AccountLedgerLine {
    /// @brief The entry the line belongs to.
    JournalEntryId entryId;
    /// @brief The entry's date.
    ::morph::time::Timestamp date;
    /// @brief What the entry shows as its source.
    std::string sourceReference;
    /// @brief The side the line is on.
    PostingSide side = PostingSide::Debit;
    /// @brief The line's value in company currency.
    Amount companyAmount;
    /// @brief The account's balance (debit less credit) after this line, from everything posted before the range
    ///        and every line of it up to this one.
    Amount balance;
};

/// @brief An account's lines in the range, by date then entry.
struct AccountLedgerView {
    /// @brief The account's code.
    std::string accountCode;
    /// @brief The lines.
    std::vector<AccountLedgerLine> lines;
};

}  // namespace lims

// A report is computed from the posted entries, so whatever changes an entry changes each report.
BRIDGE_REGISTER_RECORD(lims::TrialBalanceRow, "Ledger_TrialBalanceRow", ::morph::record::keyless,
                       ::morph::record::derivedFrom<lims::JournalEntryView>)
BRIDGE_REGISTER_RECORD(lims::AccountBalanceView, "Ledger_AccountBalance", ::morph::record::keyless,
                       ::morph::record::derivedFrom<lims::JournalEntryView>)
BRIDGE_REGISTER_RECORD(lims::AccountLedgerLine, "Ledger_AccountLedgerLine", ::morph::record::keyless,
                       ::morph::record::derivedFrom<lims::JournalEntryView>)
