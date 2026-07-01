// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @file
/// Wire DTOs for the Budget model: per-category monthly limits plus simple
/// spending analytics derived from the ledger.

namespace bank::dto {

/// @brief A monthly spending limit for a named category.
struct BudgetInfo {
    std::int64_t id = 0;
    std::string owner;
    std::string category;
    std::int64_t monthlyLimitMinor = 0;
    int currency = 0;
};

/// @brief Create or update (upsert) a category budget for the current owner.
struct SetBudget {
    std::string category;
    std::int64_t monthlyLimitMinor = 0;
    int currency = 0;

    [[nodiscard]] bool validate() const { return !category.empty() && monthlyLimitMinor >= 0; }
};

/// @brief Delete a category budget by id.
struct DeleteBudget {
    std::int64_t id = 0;
};

/// @brief List the current owner's budgets.
struct ListBudgets {
    std::string owner;  ///< empty => session principal
};

/// @brief Result of `ListBudgets`.
struct BudgetList {
    std::vector<BudgetInfo> budgets;
};

/// @brief Total spend for one ledger kind.
struct KindSpend {
    int kind = 0;  ///< bank::TxnKind
    std::int64_t totalMinor = 0;
    int count = 0;
};

/// @brief Ask for spending grouped by ledger kind on an account.
struct SpendingByKind {
    std::int64_t accountId = 0;
    std::int64_t sinceMs = 0;  ///< only entries at/after this epoch-ms (0 = all time)
};

/// @brief Result of `SpendingByKind` (debit kinds only).
struct SpendingReport {
    std::int64_t accountId = 0;
    std::int64_t totalDebitsMinor = 0;
    std::vector<KindSpend> byKind;
};

}  // namespace bank::dto
