// SPDX-License-Identifier: Apache-2.0
#include "rule_presenter.hpp"
#include "gui/error_text.hpp"

#include <utility>

namespace ledger::gui {

RulePresenter::RulePresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                             QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {
    trackBound(_handler.whenBound());
}

void RulePresenter::reportError(const std::exception_ptr& err) {
    emit failed(::morph::ladder::gui::errorText(err));
}

void RulePresenter::createRule(LedgerId ledgerId, RuleTrigger trigger, const QString& matchText,
                               RuleAction action, const QString& actionValue) {
    track<RuleId>(
        _handler.execute(CreateRule{.ledgerId = ledgerId,
                                    .trigger = trigger,
                                    .matchText = matchText.toStdString(),
                                    .action = action,
                                    .actionValue = actionValue.toStdString()}),
        [this](RuleId id) { emit ruleCreated(id); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void RulePresenter::updateRule(RuleId ruleId, const QString& matchText, const QString& actionValue) {
    track<RuleInfo>(
        _handler.execute(UpdateRule{.ruleId = ruleId,
                                    .matchText = matchText.toStdString(),
                                    .actionValue = actionValue.toStdString()}),
        [this](RuleInfo rule) { emit ruleUpdated(std::move(rule)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace ledger::gui
