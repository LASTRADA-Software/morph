// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

// Guarded exactly like `ledger_qml_bridge.hpp`'s own includes, and for the
// same reason: moc must not be pointed at the presenter's transitive
// bridge.hpp or the model header's Lightweight ORM.
#ifndef Q_MOC_RUN
#include "rule_presenter.hpp"
#endif

/// @file
/// `RuleQmlBridge` -- the QML-facing face of `RulePresenter`, a
/// MembersView-style CRUD surface for categorisation rules.

namespace ledger::gui {

/// @brief Adapts `RulePresenter` for QML: typed results become
///        `QVariantMap`/`QString` properties, calls become `Q_INVOKABLE`s.
class RuleQmlBridge : public QObject {
    Q_OBJECT

    /// @brief The most recently created or updated rule, as
    ///        `id`/`trigger`/`matchText`/`action`/`actionValue`/`version`.
    Q_PROPERTY(QVariantMap lastRule READ lastRule NOTIFY lastRuleChanged)

    /// @brief `true` while any dispatch is in flight.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    /// @brief The last error surfaced by the presenter, or empty.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    RuleQmlBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                  QObject* parent = nullptr);

    /// @return The last created or updated rule, as a QML-ready map.
    [[nodiscard]] QVariantMap lastRule() const { return _lastRule; }

    /// @return Whether a dispatch is in flight.
    [[nodiscard]] bool busy() const;

    /// @return The last error message, or an empty string.
    [[nodiscard]] QString lastError() const { return _lastError; }

    /// @brief Binds this bridge to a ledger.
    /// @param ledgerId The ledger's id, as the plain number QML holds.
    Q_INVOKABLE void openLedger(const QString& ledgerId);

    /// @brief Creates a rule that sets a category when a transaction's
    ///        description contains @p matchText.
    ///
    ///        Trigger and action are fixed rather than parameters: the model
    ///        has exactly one of each today (`DescriptionContains`,
    ///        `SetCategory`), so exposing them as QML strings would invent a
    ///        vocabulary the model cannot honour. A second trigger arrives
    ///        with its own parameter here.
    /// @param matchText  The text to match in a description.
    /// @param categoryId The category to set, as a plain-number string.
    Q_INVOKABLE void createRule(const QString& matchText, const QString& categoryId);

    /// @brief Updates a rule's match text and category.
    /// @param ruleId     The rule to update, as a plain-number string.
    /// @param matchText  The new match text.
    /// @param categoryId The new category, as a plain-number string.
    Q_INVOKABLE void updateRule(const QString& ruleId, const QString& matchText, const QString& categoryId);

  signals:
    /// @brief `lastRule()` changed.
    void lastRuleChanged();

    /// @brief `busy()` changed.
    void busyChanged();

    /// @brief `lastError()` changed.
    void lastErrorChanged();

    /// @brief A rule was created; see `lastRule()`.
    void ruleCreated();

    /// @brief A rule was updated; see `lastRule()`, whose `version` has been
    ///        bumped.
    void ruleUpdated();

  private:
    /// @brief Records @p message as `lastError` and notifies QML.
    /// @param message The presenter's error text.
    void publishError(const QString& message);

    RulePresenter _presenter;
    LedgerId _ledgerId;
    QVariantMap _lastRule;
    QString _lastError;
};

}  // namespace ledger::gui
