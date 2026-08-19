// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/core/types.hpp"
#include "ledger/core/units.hpp"

#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>

#include <string>

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

    [[nodiscard]] bool validate() const noexcept { return budgetId.hasValue() && month.size() == 7; }
};

struct GetBudgetReport {
    BudgetId budgetId;
    std::string month;

    [[nodiscard]] bool validate() const noexcept { return budgetId.hasValue() && month.size() == 7; }
};

struct GetBudgetReportResult {
    morph::math::Rational limit;
    morph::math::Rational spent;
    Currency currency;
};

}  // namespace ledger
