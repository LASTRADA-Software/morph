// SPDX-License-Identifier: Apache-2.0
import QtQuick

// The colour roles of one surface, with the ui-theme/1 defaults. Theme binds the roles a surface does not set to its
// own, and applyTheme sets every role of a theme's own surface from the theme resolved with that surface's layer.
QtObject {
    property color surface: "#ffffff"
    property color text: "#1d1d1f"
    property color muted: "#6e6e73"
    property color accent: "#1565c0"
    property color focus: "#1565c0"
    property color selection: "#cfe3ff"
    property color border: "#c7c7cc"
    property color neutral: "#e5e5ea"
    property color error: "#c62828"
    property color success: "#2e7d32"
    property color info: "#1565c0"
    property color warn: "#b26a00"
    // A tone's fill, outline and marker: `color.<ok|warn|err|info>.<bg|border|dot>`.
    property color successBg: "#e8f5e9"
    property color successBorder: "#a5d6a7"
    property color successDot: "#2e7d32"
    property color warnBg: "#fff4e5"
    property color warnBorder: "#ffcc80"
    property color warnDot: "#b26a00"
    property color errorBg: "#fdecea"
    property color errorBorder: "#ef9a9a"
    property color errorDot: "#c62828"
    property color infoBg: "#e3f2fd"
    property color infoBorder: "#90caf9"
    property color infoDot: "#1565c0"
    // A table's header background; transparent draws no fill, matching a table with no theme-set header.
    property color tableHeader: "#00000000"
}
