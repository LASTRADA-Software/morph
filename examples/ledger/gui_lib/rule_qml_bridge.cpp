// SPDX-License-Identifier: Apache-2.0
#include "rule_qml_bridge.hpp"

#include <string>
#include <utility>

#include "gui/id_qml.hpp"

namespace ledger::gui {

namespace {

using ::morph::ladder::gui::idFromText;
using ::morph::ladder::gui::idText;

/// @brief A rule as the map QML binds to.
/// @param rule The rule to render.
/// @return The QML-ready map.
[[nodiscard]] QVariantMap toVariantMap(const RuleInfo& rule) {
    return QVariantMap{
        {"id", idText(rule.id)},
        {"matchText", QString::fromStdString(rule.matchText)},
        {"actionValue", QString::fromStdString(rule.actionValue)},
        // The version an already-categorised transaction was stamped with. A
        // rule edit does not recategorise history, so this is what a view
        // needs to show "rules changed since"; dropping it here would make
        // that unanswerable without a re-query.
        {"version", static_cast<qlonglong>(rule.version)},
    };
}

}  // namespace

RuleQmlBridge::RuleQmlBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    connect(&_presenter, &RulePresenter::ruleCreated, this, [this](RuleId id) {
        // CreateRule returns only the id; the rest of the row is whatever was
        // just submitted, so the map is completed on the next update rather
        // than guessed at here.
        _lastRule = QVariantMap{{"id", idText(id)}};
        emit lastRuleChanged();
        emit ruleCreated();
    });
    connect(&_presenter, &RulePresenter::ruleUpdated, this, [this](const RuleInfo& rule) {
        _lastRule = toVariantMap(rule);
        emit lastRuleChanged();
        emit ruleUpdated();
    });
    connect(&_presenter, &RulePresenter::failed, this, [this](const QString& message) { publishError(message); });
    connect(&_presenter, &RulePresenter::idle, this, &RuleQmlBridge::busyChanged);
}

bool RuleQmlBridge::busy() const { return _presenter.busy(); }

void RuleQmlBridge::publishError(const QString& message) {
    _lastError = message;
    emit lastErrorChanged();
}

void RuleQmlBridge::openLedger(const QString& ledgerId) { _ledgerId = idFromText<LedgerId>(ledgerId); }

void RuleQmlBridge::createRule(const QString& matchText, const QString& categoryId) {
    _presenter.createRule(_ledgerId, RuleTrigger::DescriptionContains, matchText, RuleAction::SetCategory, categoryId);
    emit busyChanged();
}

void RuleQmlBridge::updateRule(const QString& ruleId, const QString& matchText, const QString& categoryId) {
    _presenter.updateRule(idFromText<RuleId>(ruleId), matchText, categoryId);
    emit busyChanged();
}

}  // namespace ledger::gui
