// SPDX-License-Identifier: Apache-2.0
#include "budget_qml_bridge.hpp"

#include <string>
#include <utility>

#include "gui/id_qml.hpp"
#include "ledger/core/money.hpp"
#include "ledger/core/units.hpp"

namespace ledger::gui {

namespace {

using ::morph::ladder::gui::idFromText;
using ::morph::ladder::gui::idText;

/// @brief An exact `Rational` as the triple QML binds to, under @p prefix,
///        plus the text the view actually displays.
///
///        Never a single pre-divided number: design spec §7's no-float rule
///        holds at this boundary, so the exact parts cross it. The rendering
///        happens here rather than in the view because QML has only IEEE
///        doubles -- `numerator / denominator / Math.pow(10, places)` in a
///        `.qml` file undoes `Rational`'s exactness in the last three lines
///        of the path. `ledger::formatMoney` is exact integer long division
///        through `Money<C>` and `morph::units::toDecimalString`.
/// @param out      The map to write into.
/// @param prefix   The key prefix, e.g. `"limit"`.
/// @param value    The exact value to publish.
/// @param currency The currency @p value is denominated in, for the text.
void putRational(QVariantMap& out, const QString& prefix, const morph::math::Rational& value, Currency currency) {
    out.insert(prefix + "Numerator", static_cast<qlonglong>(value.numerator));
    out.insert(prefix + "Denominator", static_cast<qlonglong>(value.denominator));
    out.insert(prefix + "DecimalPlaces", static_cast<qlonglong>(value.decimalPlaces.value));
    out.insert(prefix + "Text", QString::fromStdString(formatMoney(currency, value)));
}

}  // namespace

BudgetQmlBridge::BudgetQmlBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    connect(&_presenter, &BudgetPresenter::categoryCreated, this, [this](CategoryId id) {
        _lastCategoryId = id;
        emit categoryCreated();
    });
    connect(&_presenter, &BudgetPresenter::budgetCreated, this, [this](BudgetId id) {
        _lastBudgetId = id;
        emit budgetCreated();
    });
    connect(&_presenter, &BudgetPresenter::limitSet, this, [this](BudgetId) { emit limitSet(); });
    connect(&_presenter, &BudgetPresenter::reportReady, this, [this](const GetBudgetReportResult& result) {
        QVariantMap report;
        putRational(report, QStringLiteral("limit"), result.limit, result.currency);
        putRational(report, QStringLiteral("spent"), result.spent, result.currency);
        const auto code = currencyToCode(result.currency);
        report.insert(QStringLiteral("currency"), QString::fromUtf8(code.data(), static_cast<qsizetype>(code.size())));
        _report = std::move(report);
        emit reportChanged();
    });
    connect(&_presenter, &BudgetPresenter::failed, this, [this](const QString& message) { publishError(message); });
    connect(&_presenter, &BudgetPresenter::idle, this, &BudgetQmlBridge::busyChanged);
}

bool BudgetQmlBridge::busy() const { return _presenter.busy(); }

void BudgetQmlBridge::publishError(const QString& message) {
    _lastError = message;
    emit lastErrorChanged();
}

QString BudgetQmlBridge::lastCategoryId() const { return idText(_lastCategoryId); }

QString BudgetQmlBridge::lastBudgetId() const { return idText(_lastBudgetId); }

void BudgetQmlBridge::openLedger(const QString& ledgerId) { _ledgerId = idFromText<LedgerId>(ledgerId); }

void BudgetQmlBridge::createCategory(const QString& name) {
    _presenter.createCategory(_ledgerId, name);
    emit busyChanged();
}

void BudgetQmlBridge::linkAccount(const QString& accountId, const QString& categoryId) {
    _presenter.linkAccount(idFromText<AccountId>(accountId), idFromText<CategoryId>(categoryId));
    emit busyChanged();
}

void BudgetQmlBridge::createBudget(const QString& name, const QString& categoryId) {
    _presenter.createBudget(_ledgerId, name, idFromText<CategoryId>(categoryId));
    emit busyChanged();
}

void BudgetQmlBridge::setBudgetLimit(const QString& budgetId, const QString& month, qlonglong limitMinor,
                                     const QString& currency) {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // Minor units in, exact Rational out -- a whole number of the chosen
    // currency's own minor units, so the limit a user typed is the limit
    // stored. The scale comes from the currency rather than a hardcoded 2:
    // a JPY limit is counted in whole yen, and tagging it dp 2 would have
    // `BudgetModel` restate it a hundredfold smaller.
    const auto chosen = codeToCurrency(currency.toStdString());
    const auto limit = morph::math::Rational{Numerator{static_cast<std::int64_t>(limitMinor)}, Denominator{1},
                                             DecimalPlaces{currencyDecimalPlaces(chosen)}};
    _presenter.setBudgetLimit(idFromText<BudgetId>(budgetId), month, limit, chosen);
    emit busyChanged();
}

void BudgetQmlBridge::getBudgetReport(const QString& budgetId, const QString& month) {
    _presenter.getBudgetReport(idFromText<BudgetId>(budgetId), month);
    emit busyChanged();
}

}  // namespace ledger::gui
