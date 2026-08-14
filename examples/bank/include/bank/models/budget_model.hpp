// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/registry.hpp>
#include <morph/core/bridge.hpp>

#include "bank/dto/budget_dto.hpp"
#include "bank/dto/common.hpp"

/// @file
/// The Budget model: per-category monthly limits and simple spending analytics
/// computed from the ledger (debits grouped by kind).

namespace bank {

/// @brief Manages budgets and derives spending analytics.
///
/// Holds no database state itself: each `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration and
/// returns it before returning, rather than owning one for its own lifetime.
class BudgetModel {
public:
    dto::BudgetInfo execute(const dto::SetBudget& action);
    dto::CommandResult execute(const dto::DeleteBudget& action);
    dto::BudgetList execute(const dto::ListBudgets& action);
    dto::SpendingReport execute(const dto::SpendingByKind& action);
};

}  // namespace bank

using bank::BudgetModel;
using bank::dto::DeleteBudget;
using bank::dto::ListBudgets;
using bank::dto::SetBudget;
using bank::dto::SpendingByKind;

BRIDGE_REGISTER_MODEL(BudgetModel, "BudgetModel")
BRIDGE_REGISTER_ACTION(BudgetModel, SetBudget, "SetBudget")
BRIDGE_REGISTER_ACTION(BudgetModel, DeleteBudget, "DeleteBudget")
BRIDGE_REGISTER_ACTION(BudgetModel, ListBudgets, "ListBudgets", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(BudgetModel, SpendingByKind, "SpendingByKind", ::morph::model::Loggable::No)
