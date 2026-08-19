// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/dto/budget_dto.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>

namespace ledger {

/// @brief Budgets, limits, and in-model spent-so-far aggregation (design
///        spec §3). Plain default-constructible, per LedgerModel's own
///        corrected shape (Task 7) -- every action carries its own key.
class BudgetModel {
  public:
    CategoryId execute(const CreateCategory& action);
    AccountId execute(const LinkAccountToCategory& action);
    BudgetId execute(const CreateBudget& action);
    BudgetId execute(const SetBudgetLimit& action);
    GetBudgetReportResult execute(const GetBudgetReport& action);
};

}  // namespace ledger

BRIDGE_REGISTER_MODEL(ledger::BudgetModel, "BudgetModel")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::CreateCategory, "CreateCategory")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::LinkAccountToCategory, "LinkAccountToCategory")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::CreateBudget, "CreateBudget")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::SetBudgetLimit, "SetBudgetLimit")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::GetBudgetReport, "GetBudgetReport", ::morph::model::Loggable::No)

// Hand-written ActionKeyTraits per action, exactly as Task 7's real,
// verified discovery established for LedgerModel: LEDGER_DEFINE_STRONG_ID
// types (LedgerId, BudgetId, AccountId, CategoryId) all fail
// morph::model::ModelKey's std::integral/std::string constraint, so
// BRIDGE_MODEL_KEY/BRIDGE_KEY_FROM cannot be used for any of them.
// BudgetModel's actions carry genuinely different key TYPES
// (LedgerId for CreateCategory/CreateBudget, BudgetId for
// SetBudgetLimit/GetBudgetReport) -- since ModelKeyTraits<BudgetModel>
// declares one PrimaryKey type for the whole model (see Task 7's own
// ModelKeyTraits<LedgerModel> -- std::int64_t, specialized exactly once),
// and every one of these ids already unwraps to the same underlying
// std::int64_t, PrimaryKey = std::int64_t here too; each action's own
// key() just unwraps whichever field it carries.
//
// LinkAccountToCategory carries two ids (accountId, categoryId) and no
// single natural "the" key. Checked against ActionKeyTraits's real
// contract (include/morph/core/model_key.hpp): the primary template
// already declares `hasKey = false` as the default -- an action with no
// specialization at all is simply keyless, dispatched without routing to a
// specific shared instance. That default is exactly what an action with
// two co-equal ids and no natural single key needs, so LinkAccountToCategory
// deliberately gets no ActionKeyTraits specialization here (adding one that
// just restates the default would be a no-op, not a correction).
template <>
struct morph::model::ModelKeyTraits<ledger::BudgetModel> {
    using PrimaryKey = std::int64_t;
};
template <>
struct morph::model::ActionKeyTraits<ledger::CreateCategory> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::CreateCategory& action) {
        return morph::model::keyToString(*action.ledgerId);
    }
};
template <>
struct morph::model::ActionKeyTraits<ledger::CreateBudget> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::CreateBudget& action) {
        return morph::model::keyToString(*action.ledgerId);
    }
};
template <>
struct morph::model::ActionKeyTraits<ledger::SetBudgetLimit> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::SetBudgetLimit& action) {
        return morph::model::keyToString(*action.budgetId);
    }
};
template <>
struct morph::model::ActionKeyTraits<ledger::GetBudgetReport> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::GetBudgetReport& action) {
        return morph::model::keyToString(*action.budgetId);
    }
};
