// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <QVariantMap>

/// @file
/// The palette every `.qml` file reads as the `theme` context property.
///
/// It lives in `bank_gui_lib` rather than in `main.cpp` because `main.cpp` is
/// not linkable: it is the desktop client's entry point, compiled into the
/// `bank_gui` executable. Anything else that instantiates one of the shipped
/// `.qml` files — the QML-layer test binary, today — would otherwise have to
/// keep its own transcription of the map, and a key added here but not there
/// would surface as an unresolved `theme.<key>` in the test only.

namespace bankgui {

/// @brief The warm, "Claude-inspired" palette handed to QML as `theme`.
///
/// Keys are read directly by the `.qml` files (`theme.ink`, `theme.accent`,
/// …); a `QVariantMap` context property supports that member access, so no
/// `QObject` wrapper is needed.
///
/// @return The palette, one entry per name the QML layer reads.
[[nodiscard]] inline QVariantMap makeTheme() {
    return QVariantMap{
        {QStringLiteral("paper"), QStringLiteral("#FAF9F5")},
        {QStringLiteral("surface"), QStringLiteral("#FFFFFF")},
        {QStringLiteral("surfaceAlt"), QStringLiteral("#F2F0E9")},
        {QStringLiteral("ink"), QStringLiteral("#1F1E1D")},
        {QStringLiteral("inkSoft"), QStringLiteral("#6B6862")},
        {QStringLiteral("border"), QStringLiteral("#E7E4DB")},
        {QStringLiteral("accent"), QStringLiteral("#C96442")},
        {QStringLiteral("accentHover"), QStringLiteral("#B5572F")},
        {QStringLiteral("sidebar"), QStringLiteral("#262624")},
        {QStringLiteral("sidebarText"), QStringLiteral("#C9C6BE")},
        {QStringLiteral("sidebarHover"), QStringLiteral("#34322F")},
        {QStringLiteral("good"), QStringLiteral("#2F9E66")},
        {QStringLiteral("warn"), QStringLiteral("#C96442")},
        {QStringLiteral("bad"), QStringLiteral("#C0392B")},
        {QStringLiteral("goodBg"), QStringLiteral("#E5F4EC")},
        {QStringLiteral("warnBg"), QStringLiteral("#FBEDE8")},
        {QStringLiteral("badBg"), QStringLiteral("#FBEDEB")},
        {QStringLiteral("neutralBg"), QStringLiteral("#F2F0E9")},
        {QStringLiteral("dangerBorder"), QStringLiteral("#E7C9C5")},
        {QStringLiteral("radius"), 12},
    };
}

}  // namespace bankgui
