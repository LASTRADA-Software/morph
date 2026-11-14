// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <vector>

#include "lims/ledger/core/types.hpp"
#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/types.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/dto/shell_dto.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// The chart of accounts and the periods, as the wire sees them.

namespace lims {

/// @brief Adds an account to a company's chart.
struct CreateAccount {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The company.
    CompanyId company;
    /// @brief Its code, unique in the company: 1 to 16 letters and digits.
    std::string code;
    /// @brief Its name.
    std::string name;
    /// @brief What it records.
    AccountType type = AccountType::Asset;

    /// @brief Whether the account is well-formed.
    /// @return `true` when a company, a code of 1 to 16 upper-case letters and digits and a name of 1 to 128 bytes
    /// are named. Codes are upper-case so that two codes differing only in case cannot coexist.
    [[nodiscard]] bool validate() const noexcept {
        return company.hasValue() && isAccountCode(code) && !name.empty() && name.size() <= 128;
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
    using guard = ::lims::DeclaredRuleGuard;

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
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The period.
    PeriodId periodId;

    /// @brief Whether a period is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return periodId.hasValue(); }
};

/// @brief Counts a company's periods that are being closed: the ledger's open work in the shell.
struct CountClosingPeriods {
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

/// @brief Finds accounts at a company by the start of their code or a word of their name.
struct SearchAccounts {
    /// @brief The text typed.
    std::string query;
    /// @brief The company.
    CompanyId company;

    /// @brief Whether a company is named.
    /// @return `true` when it is.
    [[nodiscard]] bool validate() const noexcept { return company.hasValue(); }
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::AccountView, "Ledger_Account")
BRIDGE_REGISTER_RECORD(lims::PeriodView, "Ledger_Period")
BRIDGE_REGISTER_RECORD(lims::OpenWork<lims::PeriodView>, "Ledger_OpenPeriodWork", ::morph::record::keyless,
                       ::morph::record::derivedFrom<lims::PeriodView, lims::RoleAssignmentView>)
BRIDGE_REGISTER_RECORD(lims::SearchHits<lims::AccountView>, "Ledger_AccountHits", ::morph::record::keyless,
                       ::morph::record::derivedFrom<lims::AccountView, lims::RoleAssignmentView>)
