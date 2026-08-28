// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <optional>
#include <string>

#include "ledger/dto/budget_dto.hpp"

namespace ledger {

/// @brief Budgets, limits, and in-model spent-so-far aggregation (design
///        spec §3). Plain default-constructible, per LedgerModel's own
///        corrected shape (Task 7) -- every action carries its own key.
class BudgetModel {
public:
    /// @brief This model's primary-key type, declared rather than deduced.
    ///
    ///        Same call as `LedgerModel::PrimaryKey` (see that alias for the
    ///        full reasoning): the keyed actions here carry a `LedgerId` and a
    ///        `BudgetId`, two different strong id types over one directory
    ///        namespace, so no single strong id would be honest as *the* key
    ///        type. Both unwrap to this scalar, and a nested alias wins over
    ///        anything a `BRIDGE_MODEL_KEY` line would deduce.
    using PrimaryKey = std::int64_t;

    CategoryId execute(const CreateCategory& action);
    AccountId execute(const LinkAccountToCategory& action);
    BudgetId execute(const CreateBudget& action);
    BudgetId execute(const SetBudgetLimit& action);
    GetBudgetReportResult execute(const GetBudgetReport& action);

    /// @brief Attaches a durable action log and this instance's stable
    ///        identity, so every subsequent mutating `execute()` records
    ///        a `morph::journal::LogEntry`. Model-level mirror of
    ///        `morph::model::detail::IModelHolder::attachActionLog`, for
    ///        the same reason `LedgerModel::attachActionLog` exists (see
    ///        its own doc comment): a plain-constructed `BudgetModel`
    ///        never goes through the framework's registry/dispatcher
    ///        path, so `recordIfAttached`'s auto-append never fires for
    ///        it.
    /// @param log Sink entries are forwarded to.
    /// @param entityKey Stable identity stamped onto every LogEntry this
    ///        instance produces (this rung's ledger id, as a string).
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey);

private:
    /// @brief Records @p action/@p result as a LogEntry if a log is
    ///        attached; no-op otherwise.
    /// @tparam Action Concrete action type.
    /// @tparam Result Concrete result type.
    /// @param action The executed action.
    /// @param result The action's result.
    /// @param causalParentId Empty (the default) for every ordinary call
    ///        site.
    template <typename Action, typename Result>
    void logAction(const Action& action, const Result& result, std::string causalParentId = {}) const;

    /// @brief Records a rejected @p action as a `LogEntry` with
    ///        `Outcome::Failed` and @p error, if a log is attached; no-op
    ///        otherwise -- the refused-attempt counterpart to `logAction`
    ///        above. See `LedgerModel::logFailure`'s identical doc comment
    ///        for the rationale (`ledger_model.hpp`).
    /// @tparam Action Concrete action type.
    /// @param action The rejected action.
    /// @param error The rejecting exception's `what()`.
    template <typename Action>
    void logFailure(const Action& action, const std::string& error) const;

    std::optional<std::string> _entityKeyStr;
    std::shared_ptr<::morph::journal::IActionLog> _log;
};

}  // namespace ledger

BRIDGE_REGISTER_MODEL(ledger::BudgetModel, "BudgetModel")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::CreateCategory, "CreateCategory")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::LinkAccountToCategory, "LinkAccountToCategory")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::CreateBudget, "CreateBudget")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::SetBudgetLimit, "SetBudgetLimit")
BRIDGE_REGISTER_ACTION(ledger::BudgetModel, ledger::GetBudgetReport, "GetBudgetReport", ::morph::model::Loggable::No)

// `BRIDGE_KEY_FROM` per keyed action, and no `BRIDGE_MODEL_KEY`: this model's
// actions carry genuinely different key TYPES (`LedgerId` for
// CreateCategory/CreateBudget, `BudgetId` for SetBudgetLimit/GetBudgetReport),
// and a model has one `PrimaryKey` for all of them. Every one of these ids
// unwraps to the same underlying `std::int64_t`, which is what the directory
// keys on, so `BudgetModel` declares that scalar in its own class body --
// where `morph::model::PrimaryKeyOf` prefers it over any deduced type
// (model_key.hpp's `KeyTypeOf`) -- and each line below records only that the
// action carries the key.
//
// Until morph#183 these were hand-written specialisations, because
// `morph::model::ModelKey` admitted only `std::integral`/`std::string` and no
// `LEDGER_DEFINE_STRONG_ID` type qualified. morph#163 widened it, and with the
// hand-written bodies went their `*action.ledgerId`/`*action.budgetId` --
// `operator*` on a possibly-disengaged `std::optional`, undefined behaviour
// for an action carrying an empty id. `morph::model::keyToString` refuses an
// empty strong id instead of unwrapping it.
//
// LinkAccountToCategory carries two ids (accountId, categoryId) and no
// single natural "the" key. `ActionKeyTraits`'s primary template
// (include/morph/core/model_key.hpp) already declares `hasKey = false` as the
// default -- an action with no declaration at all is simply keyless,
// dispatched without routing to a specific shared instance. That default is
// exactly what an action with two co-equal ids needs, so
// LinkAccountToCategory deliberately gets no line here.
BRIDGE_KEY_FROM(ledger::CreateCategory, &ledger::CreateCategory::ledgerId);
BRIDGE_KEY_FROM(ledger::CreateBudget, &ledger::CreateBudget::ledgerId);
BRIDGE_KEY_FROM(ledger::SetBudgetLimit, &ledger::SetBudgetLimit::budgetId);
BRIDGE_KEY_FROM(ledger::GetBudgetReport, &ledger::GetBudgetReport::budgetId);
