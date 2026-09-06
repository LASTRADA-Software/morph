// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>

// `detail::isValidYearMonth` moved to ledger/core/time_util.hpp when
// `ListTransactions` gained the same "YYYY-MM" bound (morph#428); it is still
// `ledger::detail::isValidYearMonth`, still called from the two validate()s
// below, and now has one definition rather than two.
#include "ledger/core/time_util.hpp"
#include "ledger/core/types.hpp"
#include "ledger/core/units.hpp"

namespace ledger {

struct CreateCategory {
    LedgerId ledgerId;
    std::string name;

    [[nodiscard]] bool validate() const noexcept { return ledgerId.hasValue() && !name.empty(); }
};

struct LinkAccountToCategory {
    AccountId accountId;
    CategoryId categoryId;

    [[nodiscard]] bool validate() const noexcept { return accountId.hasValue() && categoryId.hasValue(); }
};

struct CreateBudget {
    LedgerId ledgerId;
    std::string name;
    CategoryId categoryId;

    [[nodiscard]] bool validate() const noexcept {
        return ledgerId.hasValue() && !name.empty() && categoryId.hasValue();
    }
};

struct SetBudgetLimit {
    BudgetId budgetId;
    std::string month;  // "YYYY-MM"
    morph::math::Rational limit;
    Currency currency;

    [[nodiscard]] bool validate() const noexcept { return budgetId.hasValue() && detail::isValidYearMonth(month); }
};

struct GetBudgetReport {
    BudgetId budgetId;
    std::string month;

    [[nodiscard]] bool validate() const noexcept { return budgetId.hasValue() && detail::isValidYearMonth(month); }
};

struct GetBudgetReportResult {
    morph::math::Rational limit;
    morph::math::Rational spent;
    Currency currency;
};

}  // namespace ledger
