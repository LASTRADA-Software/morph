// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QColor>
#include <QString>

/// @file
/// The visual design system for the bank GUI: a warm, modern "Claude-inspired"
/// palette (paper background, clay/coral accent, soft cards, dark warm sidebar)
/// expressed as a single application-wide Qt style sheet.

namespace bankgui::theme {

// ── Palette ──────────────────────────────────────────────────────────────────
inline constexpr const char* kPaper = "#FAF9F5";      // app background (warm paper)
inline constexpr const char* kSurface = "#FFFFFF";    // cards / inputs
inline constexpr const char* kSurfaceAlt = "#F2F0E9";  // subtle fills, hover
inline constexpr const char* kInk = "#1F1E1D";        // primary text
inline constexpr const char* kInkSoft = "#6B6862";    // secondary text
inline constexpr const char* kBorder = "#E7E4DB";     // hairline borders
inline constexpr const char* kAccent = "#C96442";     // Claude clay/coral
inline constexpr const char* kAccentHover = "#B5573698";  // (unused placeholder)
inline constexpr const char* kSidebar = "#262624";    // warm near-black sidebar
inline constexpr const char* kSidebarText = "#C9C6BE";
inline constexpr const char* kSuccess = "#2F9E66";
inline constexpr const char* kDanger = "#C0392B";

/// @brief Returns the application-wide style sheet.
inline QString styleSheet() {
    return QStringLiteral(R"QSS(
* {
    font-family: "Inter", "Segoe UI", "Helvetica Neue", sans-serif;
    font-size: 14px;
    color: #1F1E1D;
}

QWidget#Root, QStackedWidget, QScrollArea, QScrollArea > QWidget > QWidget {
    background: #FAF9F5;
}

/* ── Sidebar ──────────────────────────────────────────────────────────────── */
QWidget#Sidebar {
    background: #262624;
    border: none;
}
QLabel#Brand {
    color: #FAF9F5;
    font-size: 19px;
    font-weight: 700;
    padding: 22px 20px 6px 20px;
}
QLabel#BrandSub {
    color: #8C887F;
    font-size: 12px;
    padding: 0 20px 18px 20px;
}
QPushButton#NavButton {
    color: #C9C6BE;
    background: transparent;
    border: none;
    border-radius: 9px;
    padding: 11px 14px;
    margin: 2px 12px;
    text-align: left;
    font-size: 14px;
    font-weight: 500;
}
QPushButton#NavButton:hover {
    background: #34322F;
    color: #FAF9F5;
}
QPushButton#NavButton:checked {
    background: #C96442;
    color: #FFFFFF;
    font-weight: 600;
}
QLabel#SidebarUser {
    color: #FAF9F5;
    font-weight: 600;
    padding: 0 20px;
}
QLabel#SidebarUserSub {
    color: #8C887F;
    font-size: 12px;
    padding: 0 20px 12px 20px;
}

/* ── Headings & text ─────────────────────────────────────────────────────── */
QLabel#H1 { font-size: 26px; font-weight: 700; color: #1F1E1D; }
QLabel#H2 { font-size: 17px; font-weight: 600; color: #1F1E1D; }
QLabel#Muted { color: #6B6862; }
QLabel#Danger { color: #C0392B; font-weight: 500; }
QLabel#Success { color: #2F9E66; font-weight: 500; }

/* ── Cards ───────────────────────────────────────────────────────────────── */
QFrame#Card {
    background: #FFFFFF;
    border: 1px solid #E7E4DB;
    border-radius: 14px;
}
QFrame#StatCard {
    background: #262624;
    border: none;
    border-radius: 14px;
}
QLabel#StatValue { color: #FFFFFF; font-size: 28px; font-weight: 700; }
QLabel#StatLabel { color: #A7A39A; font-size: 13px; font-weight: 500; }

/* ── Inputs ──────────────────────────────────────────────────────────────── */
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
    background: #FFFFFF;
    border: 1px solid #E7E4DB;
    border-radius: 9px;
    padding: 9px 12px;
    selection-background-color: #C96442;
    selection-color: #FFFFFF;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border: 1px solid #C96442;
}
QComboBox::drop-down { border: none; width: 26px; }
QComboBox QAbstractItemView {
    background: #FFFFFF;
    border: 1px solid #E7E4DB;
    border-radius: 8px;
    selection-background-color: #F2F0E9;
    selection-color: #1F1E1D;
    outline: none;
}

/* ── Buttons ─────────────────────────────────────────────────────────────── */
QPushButton {
    background: #FFFFFF;
    color: #1F1E1D;
    border: 1px solid #E7E4DB;
    border-radius: 9px;
    padding: 9px 16px;
    font-weight: 600;
}
QPushButton:hover { background: #F2F0E9; }
QPushButton:disabled { color: #B6B2A9; background: #F2F0E9; }

QPushButton[variant="primary"] {
    background: #C96442;
    color: #FFFFFF;
    border: none;
}
QPushButton[variant="primary"]:hover { background: #B5572F; }
QPushButton[variant="primary"]:disabled { background: #DDB8A8; color: #FFFFFF; }

QPushButton[variant="ghost"] {
    background: transparent;
    border: none;
    color: #C96442;
    padding: 6px 8px;
}
QPushButton[variant="ghost"]:hover { color: #B5572F; background: transparent; }

QPushButton[variant="danger"] {
    background: transparent;
    color: #C0392B;
    border: 1px solid #E7C9C5;
}
QPushButton[variant="danger"]:hover { background: #FBEDEB; }

/* ── Tables ──────────────────────────────────────────────────────────────── */
QTableWidget {
    background: #FFFFFF;
    border: 1px solid #E7E4DB;
    border-radius: 12px;
    gridline-color: transparent;
    outline: none;
}
QTableWidget::item { padding: 8px 10px; border-bottom: 1px solid #F0EEE7; }
QTableWidget::item:selected { background: #F7EDE8; color: #1F1E1D; }
QHeaderView::section {
    background: #FFFFFF;
    color: #6B6862;
    border: none;
    border-bottom: 1px solid #E7E4DB;
    padding: 10px;
    font-weight: 600;
    text-transform: uppercase;
    font-size: 11px;
}
QTableCornerButton::section { background: #FFFFFF; border: none; }

/* ── Pills / badges ──────────────────────────────────────────────────────── */
QLabel[pill="neutral"] { background: #F2F0E9; color: #6B6862; border-radius: 10px; padding: 3px 10px; font-size: 12px; font-weight: 600; }
QLabel[pill="good"]    { background: #E5F4EC; color: #2F9E66; border-radius: 10px; padding: 3px 10px; font-size: 12px; font-weight: 600; }
QLabel[pill="warn"]    { background: #FBEDE8; color: #C96442; border-radius: 10px; padding: 3px 10px; font-size: 12px; font-weight: 600; }
QLabel[pill="bad"]     { background: #FBEDEB; color: #C0392B; border-radius: 10px; padding: 3px 10px; font-size: 12px; font-weight: 600; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #D9D5CB; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #C2BDB1; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
)QSS");
}

}  // namespace bankgui::theme
