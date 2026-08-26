// SPDX-License-Identifier: Apache-2.0
#include "report_qml_bridge.hpp"

#include <glaze/glaze.hpp>
#include <string>
#include <utility>
#include <vector>

#include "gui/id_qml.hpp"
#include "ledger/core/money.hpp"
#include "ledger/core/units.hpp"

namespace ledger::gui {

namespace {

using ::morph::ladder::gui::idFromText;

}  // namespace

ReportQmlBridge::ReportQmlBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    connect(&_presenter, &ReportPresenter::submitted, this,
            [this](ReportJobId) { setStatus(QStringLiteral("pending")); });
    connect(&_presenter, &ReportPresenter::reportReady, this, [this](const QString& resultJson) {
        // Decoded into the same `ReportLine` the model encoded from, then
        // republished as QML maps -- the totals stay exact triples rather
        // than becoming one pre-divided number (design spec §7).
        std::vector<ReportLine> decoded;
        if (auto err = glz::read_json(decoded, resultJson.toStdString()); err) {
            _lastError = QStringLiteral("could not decode report body");
            emit lastErrorChanged();
            setStatus(QStringLiteral("failed"));
            return;
        }
        QVariantList lines;
        lines.reserve(static_cast<qsizetype>(decoded.size()));
        for (const auto& line : decoded) {
            // `amountText` is what `ReportView` binds; the exact triple stays
            // alongside it for any view that wants to format differently. The
            // rendering cannot happen in QML, which has only IEEE doubles --
            // see `ledger::formatMoney`.
            const auto total = morph::math::Rational{morph::math::Numerator{line.numerator},
                                                     morph::math::Denominator{line.denominator},
                                                     morph::math::DecimalPlaces{line.decimalPlaces}};
            lines.push_back(QVariantMap{
                {"currency", QString::fromStdString(line.currency)},
                {"numerator", static_cast<qlonglong>(line.numerator)},
                {"denominator", static_cast<qlonglong>(line.denominator)},
                {"decimalPlaces", static_cast<qlonglong>(line.decimalPlaces)},
                {"amountText", QString::fromStdString(formatMoney(codeToCurrency(line.currency), total))},
                {"transactionCount", static_cast<qlonglong>(line.transactionCount)},
            });
        }
        _lines = std::move(lines);
        emit linesChanged();
        setStatus(QStringLiteral("done"));
    });
    connect(&_presenter, &ReportPresenter::failed, this, [this](const QString& message) {
        _lastError = message;
        emit lastErrorChanged();
        setStatus(QStringLiteral("failed"));
    });
}

void ReportQmlBridge::setStatus(QString status) {
    if (_status == status) {
        return;
    }
    _status = std::move(status);
    emit statusChanged();
}

void ReportQmlBridge::openLedger(const QString& ledgerId) { _ledgerId = idFromText<LedgerId>(ledgerId); }

void ReportQmlBridge::requestMonthlyStatement(int year, int month, int timezoneOffsetMinutes) {
    _lines.clear();
    emit linesChanged();
    setStatus(QStringLiteral("pending"));
    _presenter.submitMonthlyStatement(_ledgerId, year, static_cast<unsigned>(month), timezoneOffsetMinutes);
}

}  // namespace ledger::gui
