// SPDX-License-Identifier: Apache-2.0
#include "ledger/models/rule_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>

#include "clock.hpp"
#include "ledger/core/errors.hpp"
#include "ledger/db/book_access.hpp"
#include "ledger/db/ledger_entity.hpp"

namespace ledger {

void RuleModel::attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
    _log = std::move(log);
    _entityKeyStr = std::move(entityKey);
}

template <typename Action, typename Result>
void RuleModel::logAction(const Action& action, const Result& result, std::string causalParentId) const {
    if (!_log) {
        return;
    }
    ::morph::journal::LogEntry entry;
    entry.modelType = "RuleModel";
    entry.entityKey = _entityKeyStr.value_or(std::string{});
    entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    // See LedgerModel::logAction's identical comment for why an unstamped
    // entry is an unverifiable one.
    entry.schema = ::morph::model::detail::actionPayloadSchema<Action>();
    entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
    entry.outcome = ::morph::journal::Outcome::Succeeded;
    if (const auto* ctx = ::morph::session::current()) {
        entry.principal = ctx->principal;
    }
    entry.timestampMs = (*morph::ladder::now().value).value.time_since_epoch().count();  // server-stamped audit
                                                                                         // timestamp -- see
                                                                                         // LedgerModel::logAction's
                                                                                         // identical comment
    entry.causalParentId = std::move(causalParentId);
    _log->append(std::move(entry));
    // See LedgerModel::logAction's identical comment for why this flush is
    // load-bearing, not optional.
    _log->flush();
}

template <typename Action>
void RuleModel::logFailure(const Action& action, const std::string& error) const {
    if (!_log) {
        return;
    }
    ::morph::journal::LogEntry entry;
    entry.modelType = "RuleModel";
    entry.entityKey = _entityKeyStr.value_or(std::string{});
    entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.schema = ::morph::model::detail::actionPayloadSchema<Action>();
    // See LedgerModel::logFailure's identical comment: no `result` -- there
    // was none -- and `error` carries the rejecting exception's text.
    entry.error = error;
    entry.outcome = ::morph::journal::Outcome::Failed;
    if (const auto* ctx = ::morph::session::current()) {
        entry.principal = ctx->principal;
    }
    entry.timestampMs = (*morph::ladder::now().value).value.time_since_epoch().count();
    _log->append(std::move(entry));
    _log->flush();
}

RuleId RuleModel::execute(const CreateRule& action) {
    try {
        const auto* ctx = morph::session::current();
        if (ctx == nullptr || ctx->principal.empty()) {
            throw EmptyPrincipalError{};
        }
        if (!action.validate()) {
            throw ValidationError{"CreateRule: ledgerId and matchText are required"};
        }
        Lightweight::DataMapper mapper;
        const auto ledgerRow = db::requireOwnedBook(mapper, action.ledgerId, ctx->principal, "CreateRule");
        db::RuleRecord ruleRow;
        ruleRow.ledger = ledgerRow;
        ruleRow.trigger = static_cast<int>(action.trigger);
        ruleRow.matchText = action.matchText;
        ruleRow.action = static_cast<int>(action.action);
        ruleRow.actionValue = action.actionValue;
        ruleRow.version = 1;
        mapper.Create(ruleRow);
        auto ruleId = RuleId{static_cast<std::int64_t>(ruleRow.id.Value())};
        logAction(action, ruleId);
        return ruleId;
    } catch (const LedgerError& error) {
        logFailure(action, error.what());
        throw;
    }
}

RuleInfo RuleModel::execute(const UpdateRule& action) {
    try {
        const auto* ctx = morph::session::current();
        if (ctx == nullptr || ctx->principal.empty()) {
            throw EmptyPrincipalError{};
        }
        if (!action.validate()) {
            throw ValidationError{"UpdateRule: ruleId and matchText are required"};
        }
        Lightweight::DataMapper mapper;
        auto ruleRows = mapper.Query<db::RuleRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::RuleRecord::id>, "=", *action.ruleId)
                            .All();
        if (ruleRows.empty()) {
            throw NotFound{"UpdateRule: no such rule"};
        }
        auto& ruleRow = ruleRows.front();
        db::requireOwnedParentBook(mapper, ruleRow.ledger.Value(), ctx->principal, "UpdateRule");
        // Optimistic concurrency (design spec §10, Scenario B). An engaged
        // expectedVersion that no longer matches means the row moved on after the
        // client read it: refuse outright rather than overwrite the change that
        // landed in between, or merge the two. A disengaged one applies
        // unconditionally, which is what every caller predating this did.
        if (action.expectedVersion.has_value() && *action.expectedVersion != ruleRow.version.Value()) {
            throw VersionConflict{};
        }
        ruleRow.matchText = action.matchText;
        ruleRow.actionValue = action.actionValue;
        ruleRow.version = ruleRow.version.Value() + 1;
        mapper.Update(ruleRow);
        RuleInfo result{.id = action.ruleId,
                        .trigger = static_cast<RuleTrigger>(ruleRow.trigger.Value()),
                        .matchText = std::string{ruleRow.matchText.Value().ToStringView()},
                        .action = static_cast<RuleAction>(ruleRow.action.Value()),
                        .actionValue = std::string{ruleRow.actionValue.Value().ToStringView()},
                        .version = ruleRow.version.Value()};
        logAction(action, result);
        return result;
    } catch (const LedgerError& error) {
        logFailure(action, error.what());
        throw;
    }
}

}  // namespace ledger
