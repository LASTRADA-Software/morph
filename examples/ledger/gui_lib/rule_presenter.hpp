// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gui/presenter.hpp"
#include "ledger/dto/rule_dto.hpp"

#include <QObject>
#include <QString>

#include <exception>

// Guarded exactly like `ledger_presenter.hpp`'s own includes, and for the
// same reason it documents: moc must not be pointed at morph's
// template-heavy bridge.hpp nor the model header's Lightweight ORM.
#ifndef Q_MOC_RUN
#include "ledger/models/rule_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

/// @file
/// `RulePresenter` -- the transport half of rung 5's categorisation-rules
/// screen.

namespace ledger::gui {

/// @brief Dispatches `RuleModel`'s action surface and re-emits each result as
///        a Qt signal.
///
/// `AllowShared` for the reason `LedgerPresenter` documents: `RuleModel` is
/// keyed, so clients working the same ledger's rules must join one
/// shared-instance directory rather than each registering a private instance.
class RulePresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    RulePresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                  QObject* parent = nullptr);

    /// @brief Creates a categorisation rule. Emits `ruleCreated` on success,
    ///        `failed` on error.
    /// @param ledgerId    The ledger the rule belongs to.
    /// @param trigger     What the rule matches on.
    /// @param matchText   The text the trigger compares against.
    /// @param action      What the rule does when it matches.
    /// @param actionValue The action's argument, e.g. the category id to set.
    void createRule(LedgerId ledgerId, RuleTrigger trigger, const QString& matchText, RuleAction action,
                    const QString& actionValue);

    /// @brief Updates an existing rule's match text and action value. Emits
    ///        `ruleUpdated` on success, `failed` on error.
    ///
    ///        The result carries the rule's new `version`, which is what an
    ///        already-categorised transaction is stamped with: a rule edit
    ///        does not retroactively recategorise history, so the version is
    ///        the thing a view needs to show "rules changed since".
    /// @param ruleId      The rule to update.
    /// @param matchText   The new match text.
    /// @param actionValue The new action value.
    void updateRule(RuleId ruleId, const QString& matchText, const QString& actionValue);

  signals:
    /// @brief A rule was created.
    /// @param ruleId The new rule's id.
    void ruleCreated(ledger::RuleId ruleId);

    /// @brief A rule was updated.
    /// @param rule The rule's new state, including its bumped version.
    void ruleUpdated(ledger::RuleInfo rule);

    /// @brief Any dispatch above failed.
    /// @param message The exception's `what()`.
    void failed(QString message);

  private:
    /// @brief Re-emits @p err as `failed` carrying its `what()`.
    /// @param err The exception the completion carried.
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::BridgeHandler<RuleModel, ::morph::bridge::AllowShared> _handler;
};

}  // namespace ledger::gui
