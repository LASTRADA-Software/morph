// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/ledger/core/types.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// The chart of accounts and the periods, as the wire sees them.

namespace lims {

/// @brief Adds an account to a company's chart.
struct CreateAccount {
    /// @brief The company.
    CompanyId company;
    /// @brief Its code, unique in the company: 1 to 16 letters and digits.
    std::string code;
    /// @brief Its name.
    std::string name;
    /// @brief What it records.
    AccountType type = AccountType::Asset;

    /// @brief Whether the account is well-formed.
    /// @return `true` when a company, a code of 1 to 16 letters and digits and a name of 1 to 128 bytes are named.
    [[nodiscard]] bool validate() const noexcept {
        bool const codeOk = !code.empty() && code.size() <= 16 && std::ranges::all_of(code, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        });
        return company.hasValue() && codeOk && !name.empty() && name.size() <= 128;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One account.
struct AccountView {
    /// @brief Its id.
    AccountId id;
    /// @brief The company.
    CompanyId company;
    /// @brief Its code.
    std::string code;
    /// @brief Its name.
    std::string name;
    /// @brief What it records.
    AccountType type = AccountType::Asset;
};

/// @brief Lists a company's accounts.
struct ListAccounts {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief The accounts, by code.
struct ListAccountsResult {
    /// @brief The accounts.
    std::vector<AccountView> accounts;
};

/// @brief Opens the month an instant falls in, for a company.
struct OpenPeriod {
    /// @brief The company.
    CompanyId company;
    /// @brief Any instant in the month.
    ::morph::time::Timestamp month;

    /// @brief Whether the request is well-formed.
    /// @return `true` when a company and an instant are named.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue() && month.hasValue(); }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One month of a company's books.
struct PeriodView {
    /// @brief Its id.
    PeriodId id;
    /// @brief The company.
    CompanyId company;
    /// @brief Midnight UTC on the month's first day.
    ::morph::time::Timestamp start;
    /// @brief `YYYY-MM`.
    std::string label;
    /// @brief Where it stands.
    PeriodState state = PeriodState::Open;
};

/// @brief Lists a company's periods.
struct ListPeriods {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief The periods, by start.
struct ListPeriodsResult {
    /// @brief The periods.
    std::vector<PeriodView> periods;
};

/// @brief `Open → Closing`: no new drafts while the month is reviewed.
struct BeginClosePeriod {
    /// @brief The period.
    PeriodId periodId;

    /// @brief Whether a period is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return periodId.hasValue(); }
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::AccountView, "Ledger_Account")
BRIDGE_REGISTER_RECORD(lims::PeriodView, "Ledger_Period")
