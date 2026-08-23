// SPDX-License-Identifier: Apache-2.0
#include "budget_presenter.hpp"
#include "gui/error_text.hpp"

#include <utility>

namespace ledger::gui {

BudgetPresenter::BudgetPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                 QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {
    trackBound(_handler.whenBound());
}

void BudgetPresenter::reportError(const std::exception_ptr& err) {
    emit failed(::morph::ladder::gui::errorText(err));
}

void BudgetPresenter::createCategory(LedgerId ledgerId, const QString& name) {
    track<CategoryId>(
        _handler.execute(CreateCategory{.ledgerId = ledgerId, .name = name.toStdString()}),
        [this](CategoryId id) { emit categoryCreated(id); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BudgetPresenter::linkAccount(AccountId accountId, CategoryId categoryId) {
    track<AccountId>(
        _handler.execute(LinkAccountToCategory{.accountId = accountId, .categoryId = categoryId}),
        [this](AccountId id) { emit accountLinked(id); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BudgetPresenter::createBudget(LedgerId ledgerId, const QString& name, CategoryId categoryId) {
    track<BudgetId>(
        _handler.execute(
            CreateBudget{.ledgerId = ledgerId, .name = name.toStdString(), .categoryId = categoryId}),
        [this](BudgetId id) { emit budgetCreated(id); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BudgetPresenter::setBudgetLimit(BudgetId budgetId, const QString& month,
                                     const morph::math::Rational& limit, Currency currency) {
    track<BudgetId>(
        _handler.execute(SetBudgetLimit{
            .budgetId = budgetId, .month = month.toStdString(), .limit = limit, .currency = currency}),
        [this](BudgetId id) { emit limitSet(id); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BudgetPresenter::getBudgetReport(BudgetId budgetId, const QString& month) {
    track<GetBudgetReportResult>(
        _handler.execute(GetBudgetReport{.budgetId = budgetId, .month = month.toStdString()}),
        [this](GetBudgetReportResult result) { emit reportReady(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace ledger::gui
