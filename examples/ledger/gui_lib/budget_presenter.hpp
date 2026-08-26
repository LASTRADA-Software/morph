// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <exception>

#include "gui/presenter.hpp"
#include "ledger/dto/budget_dto.hpp"

// Guarded exactly like `ledger_presenter.hpp`'s own includes, and for the
// same reason it documents: moc must not be pointed at morph's
// template-heavy bridge.hpp nor the model header's Lightweight ORM, whose
// namespace structure it mis-parses.
#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include "ledger/models/budget_model.hpp"
#endif

/// @file
/// `BudgetPresenter` -- the transport half of rung 5's budget screen.

namespace ledger::gui {

/// @brief Dispatches `BudgetModel`'s action surface and re-emits each result
///        as a Qt signal.
///
/// `AllowShared` for the same reason `LedgerPresenter` uses it: `BudgetModel`
/// is keyed (its own `PrimaryKey` alias, budget_model.hpp), so a second client
/// working the same ledger's budgets must join the same shared-instance
/// directory rather than registering a private instance.
class BudgetPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    BudgetPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief Creates a spending category. Emits `categoryCreated` on
    ///        success, `failed` on error.
    /// @param ledgerId The ledger to create it on.
    /// @param name     The category's display name.
    void createCategory(LedgerId ledgerId, const QString& name);

    /// @brief Links @p accountId's spending to @p categoryId, so a budget
    ///        over that category counts it. Emits `accountLinked` on
    ///        success, `failed` on error.
    /// @param accountId  The account to link.
    /// @param categoryId The category to link it to.
    void linkAccount(AccountId accountId, CategoryId categoryId);

    /// @brief Creates a budget over @p categoryId. Emits `budgetCreated` on
    ///        success, `failed` on error.
    /// @param ledgerId   The ledger to create it on.
    /// @param name       The budget's display name.
    /// @param categoryId The category it budgets.
    void createBudget(LedgerId ledgerId, const QString& name, CategoryId categoryId);

    /// @brief Sets @p budgetId's limit for one month. Emits `limitSet` on
    ///        success, `failed` on error.
    /// @param budgetId The budget to set a limit on.
    /// @param month    The month, as `YYYY-MM`.
    /// @param limit    The limit, exact -- never a float (design spec §7).
    /// @param currency The limit's currency.
    void setBudgetLimit(BudgetId budgetId, const QString& month, const morph::math::Rational& limit,
                        Currency currency);

    /// @brief Reads @p budgetId's limit-versus-spent for one month. Emits
    ///        `reportReady` on success, `failed` on error.
    /// @param budgetId The budget to report on.
    /// @param month    The month, as `YYYY-MM`.
    void getBudgetReport(BudgetId budgetId, const QString& month);

signals:
    /// @brief A category was created.
    /// @param categoryId The new category's id.
    void categoryCreated(ledger::CategoryId categoryId);

    /// @brief An account was linked to a category.
    /// @param accountId The account that was linked.
    void accountLinked(ledger::AccountId accountId);

    /// @brief A budget was created.
    /// @param budgetId The new budget's id.
    void budgetCreated(ledger::BudgetId budgetId);

    /// @brief A month's limit was set.
    /// @param budgetId The budget whose limit was set.
    void limitSet(ledger::BudgetId budgetId);

    /// @brief A budget report was computed.
    /// @param result The month's limit, spent, and currency.
    void reportReady(ledger::GetBudgetReportResult result);

    /// @brief Any dispatch above failed.
    /// @param message The exception's `what()`.
    void failed(QString message);

private:
    /// @brief Re-emits @p err as `failed` carrying its `what()`.
    /// @param err The exception the completion carried.
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::BridgeHandler<BudgetModel, ::morph::bridge::AllowShared> _handler;
};

}  // namespace ledger::gui
