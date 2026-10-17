// SPDX-License-Identifier: Apache-2.0
import QtQuick

// The colour roles of one surface. A role a surface does not set is its base's, the theme itself; applyTheme sets
// every role of a theme's own surface from the theme resolved with that surface's layer.
QtObject {
    property QtObject base: null

    property color surface: base ? base.surface : "#ffffff"
    property color text: base ? base.text : "#1d1d1f"
    property color muted: base ? base.muted : "#6e6e73"
    property color accent: base ? base.accent : "#1565c0"
    property color focus: base ? base.focus : "#1565c0"
    property color selection: base ? base.selection : "#cfe3ff"
    property color border: base ? base.border : "#c7c7cc"
    property color neutral: base ? base.neutral : "#e5e5ea"
    property color error: base ? base.error : "#c62828"
    property color success: base ? base.success : "#2e7d32"
    property color info: base ? base.info : "#1565c0"
    property color warn: base ? base.warn : "#b26a00"
}
