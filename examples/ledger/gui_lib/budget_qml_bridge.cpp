// SPDX-License-Identifier: Apache-2.0
#include "budget_qml_bridge.hpp"
#include "gui/id_qml.hpp"

#include "ledger/core/units.hpp"

#include <string>
#include <utility>

namespace ledger::gui {

namespace {

using ::morph::ladder::gui::idFromText;
using ::morph::ladder::gui::idText;

/// @brief An exact `Rational` as the triple QML binds to, under @p prefix.
///
///        Never a single pre-divided number: design spec §7's no-float rule
///        holds at this boundary, so the view receives the exact parts and
///        formats them itself.
/// @param out    The map to write into.
/// @param prefix The key prefix, e.g. `"limit"`.
/// @param value  The exact value to publish.
void putRational(QVariantMap& out, const QString& prefix, const morph::math::Rational& value) {
    out.insert(prefix + "Numerator", static_cast<qlonglong>(value.numerator));
    out.insert(prefix + "Denominator", static_cast<qlonglong>(value.denominator));
    out.insert(prefix + "DecimalPlaces", static_cast<qlonglong>(value.decimalPlaces.value));
}

}  // namespace

BudgetQmlBridge::BudgetQmlBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                 QObject* parent)
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
        putRational(report, QStringLiteral("limit"), result.limit);
        putRational(report, QStringLiteral("spent"), result.spent);
        const auto code = currencyToCode(result.currency);
        report.insert(QStringLiteral("currency"),
                      QString::fromUtf8(code.data(), static_cast<qsizetype>(code.size())));
        _report = std::move(report);
        emit reportChanged();
    });
    connect(&_presenter, &BudgetPresenter::failed, this,
            [this](const QString& message) { publishError(message); });
    connect(&_presenter, &BudgetPresenter::idle, this, &BudgetQmlBridge::busyChanged);
}

bool BudgetQmlBridge::busy() const {
    return _presenter.busy();
}

void BudgetQmlBridge::publishError(const QString& message) {
    _lastError = message;
    emit lastErrorChanged();
}

QString BudgetQmlBridge::lastCategoryId() const {
    return idText(_lastCategoryId);
}

QString BudgetQmlBridge::lastBudgetId() const {
    return idText(_lastBudgetId);
}

void BudgetQmlBridge::openLedger(const QString& ledgerId) {
    _ledgerId = idFromText<LedgerId>(ledgerId);
}

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
    // Minor units in, exact Rational out -- cents over a denominator of 1 at
    // two decimal places, so the limit a user typed is the limit stored.
    constexpr std::uint32_t kMinorUnitPlaces = 2;
    const auto limit = morph::math::Rational{Numerator{static_cast<std::int64_t>(limitMinor)}, Denominator{1},
                                             DecimalPlaces{kMinorUnitPlaces}};
    _presenter.setBudgetLimit(idFromText<BudgetId>(budgetId), month, limit,
                              codeToCurrency(currency.toStdString()));
    emit busyChanged();
}

void BudgetQmlBridge::getBudgetReport(const QString& budgetId, const QString& month) {
    _presenter.getBudgetReport(idFromText<BudgetId>(budgetId), month);
    emit busyChanged();
}

}  // namespace ledger::gui
