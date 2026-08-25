// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

// Guarded exactly like `ledger_qml_bridge.hpp`'s own includes, and for the
// same reason: moc must not be pointed at the presenter's transitive
// bridge.hpp or the model header's Lightweight ORM.
#ifndef Q_MOC_RUN
#include "budget_presenter.hpp"
#endif

/// @file
/// `BudgetQmlBridge` -- the QML-facing face of `BudgetPresenter`.

namespace ledger::gui {

/// @brief Adapts `BudgetPresenter` for QML: the current month's budget report
///        becomes a `QVariantMap` property, and its calls become
///        `Q_INVOKABLE`s taking the plain types QML can pass.
class BudgetQmlBridge : public QObject {
    Q_OBJECT

    /// @brief The last computed report: `limitNumerator`/`limitDenominator`/
    ///        `limitDecimalPlaces`, the same triple for `spent`, and
    ///        `currency`. Empty until `getBudgetReport` returns.
    Q_PROPERTY(QVariantMap report READ report NOTIFY reportChanged)

    /// @brief `true` while any dispatch is in flight.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    /// @brief The last error surfaced by the presenter, or empty.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    BudgetQmlBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @return The last computed report, as a QML-ready map.
    [[nodiscard]] QVariantMap report() const { return _report; }

    /// @return Whether a dispatch is in flight.
    [[nodiscard]] bool busy() const;

    /// @return The last error message, or an empty string.
    [[nodiscard]] QString lastError() const { return _lastError; }

    /// @brief Binds this bridge to a ledger, for the calls that need one.
    /// @param ledgerId The ledger's id, as the plain number QML holds.
    Q_INVOKABLE void openLedger(const QString& ledgerId);

    /// @brief Creates a spending category on the bound ledger.
    /// @param name The category's display name.
    Q_INVOKABLE void createCategory(const QString& name);

    /// @brief Links an account's spending to a category, so a budget over
    ///        that category counts it.
    /// @param accountId  The account, as a plain-number string.
    /// @param categoryId The category, as a plain-number string.
    Q_INVOKABLE void linkAccount(const QString& accountId, const QString& categoryId);

    /// @brief Creates a budget over @p categoryId on the bound ledger.
    /// @param name       The budget's display name.
    /// @param categoryId The category it budgets.
    Q_INVOKABLE void createBudget(const QString& name, const QString& categoryId);

    /// @brief Sets a month's limit, in minor units.
    /// @param budgetId   The budget, as a plain-number string.
    /// @param month      The month, as `YYYY-MM`.
    /// @param limitMinor The limit in minor units (cents) -- an integer, so
    ///        nothing is rounded crossing this boundary (design spec §7).
    /// @param currency   The ISO code, e.g. `USD`.
    Q_INVOKABLE void setBudgetLimit(const QString& budgetId, const QString& month, qlonglong limitMinor,
                                    const QString& currency);

    /// @brief Reads a month's limit-versus-spent into `report`.
    /// @param budgetId The budget, as a plain-number string.
    /// @param month    The month, as `YYYY-MM`.
    Q_INVOKABLE void getBudgetReport(const QString& budgetId, const QString& month);

    /// @return The most recently created category's id, for a view that
    ///         chains creation into a budget without a round trip.
    [[nodiscard]] Q_INVOKABLE QString lastCategoryId() const;

    /// @return The most recently created budget's id, for the same reason.
    [[nodiscard]] Q_INVOKABLE QString lastBudgetId() const;

signals:
    /// @brief `report()` changed.
    void reportChanged();

    /// @brief `busy()` changed.
    void busyChanged();

    /// @brief `lastError()` changed.
    void lastErrorChanged();

    /// @brief A category was created; its id is available from
    ///        `lastCategoryId()`.
    void categoryCreated();

    /// @brief A budget was created; its id is available from
    ///        `lastBudgetId()`.
    void budgetCreated();

    /// @brief A month's limit was stored.
    void limitSet();

private:
    /// @brief Records @p message as `lastError` and notifies QML.
    /// @param message The presenter's error text.
    void publishError(const QString& message);

    BudgetPresenter _presenter;
    LedgerId _ledgerId;
    CategoryId _lastCategoryId;
    BudgetId _lastBudgetId;
    QVariantMap _report;
    QString _lastError;
};

}  // namespace ledger::gui
