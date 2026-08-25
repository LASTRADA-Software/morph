// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <optional>
#include <string>

#include "ledger/dto/rule_dto.hpp"

namespace ledger {

/// @brief Rule CRUD, keyed by LedgerId. Plain default-constructible, per
///        LedgerModel's own real shape (Task 7) -- the key lives in each
///        action. Rule *evaluation* during StoreTransaction lives in
///        LedgerModel (Task 12's cascade step), which reads RuleRecord rows
///        via a direct Query<RuleRecord> rather than calling back into a
///        live RuleModel instance -- there is no established cross-model
///        read pattern in this codebase to reuse instead.
class RuleModel {
public:
    RuleId execute(const CreateRule& action);
    RuleInfo execute(const UpdateRule& action);

    /// @brief Attaches a durable action log and this instance's stable
    ///        identity, so every subsequent mutating `execute()` records a
    ///        `morph::journal::LogEntry`. Model-level mirror of
    ///        `morph::model::detail::IModelHolder::attachActionLog`, for the
    ///        same reason `LedgerModel::attachActionLog`/
    ///        `BudgetModel::attachActionLog` exist (see their own doc
    ///        comments): a plain-constructed `RuleModel` never goes through
    ///        the framework's registry/dispatcher path, so
    ///        `recordIfAttached`'s auto-append never fires for it.
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

    std::optional<std::string> _entityKeyStr;
    std::shared_ptr<::morph::journal::IActionLog> _log;
};

}  // namespace ledger

BRIDGE_REGISTER_MODEL(ledger::RuleModel, "RuleModel")
BRIDGE_REGISTER_ACTION(ledger::RuleModel, ledger::CreateRule, "CreateRule")
BRIDGE_REGISTER_ACTION(ledger::RuleModel, ledger::UpdateRule, "UpdateRule")

// Hand-written ModelKeyTraits/ActionKeyTraits, per Task 7's real,
// verified discovery: LedgerId fails morph::model::ModelKey's
// std::integral/std::string constraint, so BRIDGE_MODEL_KEY/
// BRIDGE_KEY_FROM cannot be used.
template <>
struct morph::model::ModelKeyTraits<ledger::RuleModel> {
    using PrimaryKey = std::int64_t;
};
template <>
struct morph::model::ActionKeyTraits<ledger::CreateRule> {
    static constexpr bool hasKey = true;
    static constexpr bool fromResult = false;
    static std::string key(const ledger::CreateRule& action) { return morph::model::keyToString(*action.ledgerId); }
};

// UpdateRule carries a ruleId, not a ledgerId, so it cannot share
// CreateRule's key type -- and per model_key.hpp's real primary-template
// default (hasKey = false, confirmed the same way Task 10 confirmed it for
// LinkAccountToCategory), an action with no natural single ledger-scoped key
// is simply left keyless: no ActionKeyTraits specialization here.
