// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#ifndef Q_MOC_RUN
#include "report_presenter.hpp"
#endif

/// @file
/// `ReportQmlBridge` -- the QML-facing face of the submit->poll report flow.

namespace ledger::gui {

/// @brief Adapts `ReportPresenter` for QML: a submitted report's progress and
///        eventual body become bindable properties.
///
/// `status` is the property a view actually drives off -- `idle`, `pending`,
/// `done`, or `failed` -- because "is my report ready?" is the only question
/// this screen asks, and answering it with a bare `busy` flag would conflate
/// "still computing" with "never asked".
class ReportQmlBridge : public QObject {
    Q_OBJECT

    /// @brief One of `idle`, `pending`, `done`, `failed`.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

    /// @brief The finished report's lines, each carrying its currency and an
    ///        exact total triple. Empty until `status` is `done`.
    Q_PROPERTY(QVariantList lines READ lines NOTIFY linesChanged)

    /// @brief The last error surfaced, or empty.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

  public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    ReportQmlBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                    QObject* parent = nullptr);

    /// @return The current status token.
    [[nodiscard]] QString status() const { return _status; }

    /// @return The finished report's lines.
    [[nodiscard]] QVariantList lines() const { return _lines; }

    /// @return The last error message, or an empty string.
    [[nodiscard]] QString lastError() const { return _lastError; }

    /// @brief Binds this bridge to a ledger.
    /// @param ledgerId The ledger's id, as the plain number QML holds.
    Q_INVOKABLE void openLedger(const QString& ledgerId);

    /// @brief Submits a monthly statement and polls it to completion.
    /// @param year  The local calendar year.
    /// @param month The local calendar month, 1-12.
    /// @param timezoneOffsetMinutes The client's offset from UTC -- what
    ///        decides whether a 23:30-local transaction counts (Task 17).
    Q_INVOKABLE void requestMonthlyStatement(int year, int month, int timezoneOffsetMinutes);

  signals:
    /// @brief `status()` changed.
    void statusChanged();

    /// @brief `lines()` changed.
    void linesChanged();

    /// @brief `lastError()` changed.
    void lastErrorChanged();

  private:
    /// @brief Sets `status` and notifies QML.
    /// @param status The new status token.
    void setStatus(QString status);

    ReportPresenter _presenter;
    LedgerId _ledgerId;
    QString _status{QStringLiteral("idle")};
    QVariantList _lines;
    QString _lastError;
};

}  // namespace ledger::gui
