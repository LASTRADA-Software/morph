// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QCursor>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QSizePolicy>
#include <QString>
#include <QStyle>
#include <QWidget>

#include <cstdint>
#include <exception>
#include <optional>

#include "bank/core/money.hpp"
#include "bank/core/types.hpp"

/// @file
/// Small UI helpers: money formatting/parsing, error extraction, and factory
/// functions for the themed widgets used across the views.

namespace bankgui::ui {

/// @brief Formats minor units for a currency, e.g. (1234, USD) -> "12.34 USD".
inline QString formatMinor(std::int64_t minor, int currency) {
    const auto cur = static_cast<bank::Currency>(currency);
    return QString::fromStdString(bank::format(bank::Money{.minor = minor, .currency = cur}));
}

/// @brief Parses a user-entered amount (major units) into minor units.
/// @return std::nullopt if the text is not a valid non-negative number.
inline std::optional<std::int64_t> parseToMinor(const QString& text, int decimals) {
    bool ok = false;
    const double major = text.trimmed().toDouble(&ok);
    if (!ok || major < 0.0) {
        return std::nullopt;
    }
    double scale = 1.0;
    for (int idx = 0; idx < decimals; ++idx) {
        scale *= 10.0;
    }
    return static_cast<std::int64_t>(major * scale + 0.5);
}

/// @brief Extracts a human-readable message from an exception_ptr.
inline QString errorText(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& exc) {
        return QString::fromUtf8(exc.what());
    } catch (...) {
        return QStringLiteral("unknown error");
    }
}

/// @brief Re-applies the style sheet to @p widget after a dynamic property change.
inline void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

/// @brief Creates a button with a style variant ("primary", "ghost", "danger", or "").
inline QPushButton* button(const QString& text, const QString& variant = {}) {
    auto* btn = new QPushButton(text);
    btn->setCursor(Qt::PointingHandCursor);
    if (!variant.isEmpty()) {
        btn->setProperty("variant", variant);
    }
    return btn;
}

/// @brief Creates a label with a style objectName ("H1", "H2", "Muted", ...).
inline QLabel* label(const QString& text, const QString& role = {}) {
    auto* lbl = new QLabel(text);
    if (!role.isEmpty()) {
        lbl->setObjectName(role);
    }
    return lbl;
}

/// @brief Creates a coloured status pill ("neutral", "good", "warn", "bad").
inline QLabel* pill(const QString& text, const QString& kind) {
    auto* lbl = new QLabel(text);
    lbl->setProperty("pill", kind);
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    return lbl;
}

/// @brief Creates an empty rounded "Card" frame.
inline QFrame* card() {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("Card"));
    return frame;
}

/// @brief Removes and deletes every item (and child widget) in @p layout.
inline void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        if (QLayout* child = item->layout()) {
            clearLayout(child);
        }
        delete item;
    }
}

}  // namespace bankgui::ui
