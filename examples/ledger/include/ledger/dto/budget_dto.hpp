// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cctype>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "ledger/core/types.hpp"
#include "ledger/core/units.hpp"

namespace ledger {

namespace detail {

/// @brief Checks that `month` is a well-formed `"YYYY-MM"` string: exactly
///        7 characters, digits in the year/month positions, a literal `-`
///        at index 4, and a month value in `[1, 12]`. Used by
///        `SetBudgetLimit::validate()`/`GetBudgetReport::validate()` to
///        reject malformed input at the DTO boundary, before it ever
///        reaches `monthRangeMs`'s date arithmetic (a malformed month like
///        `"2026-13"` would otherwise silently produce a garbage range).
/// @param month The candidate month string to check.
/// @return `true` if `month` is a syntactically valid `"YYYY-MM"` string
///         naming a real calendar month (1-12).
[[nodiscard]] inline bool isValidYearMonth(const std::string& month) noexcept {
    if (month.size() != 7 || month[4] != '-') {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(month[i]))) {
            return false;
        }
    }
    if (!std::isdigit(static_cast<unsigned char>(month[5])) || !std::isdigit(static_cast<unsigned char>(month[6]))) {
        return false;
    }
    const int monthNum = (month[5] - '0') * 10 + (month[6] - '0');
    return monthNum >= 1 && monthNum <= 12;
}

}  // namespace detail

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
