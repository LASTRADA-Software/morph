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
    // The neutral tone's text, fill, outline and marker: `color.neutral` and its `.bg`, `.border` and `.dot`.
    property color neutral: "#6e6e73"
    property color neutralBg: "#266e6e73"
    property color neutralBorder: "#6e6e73"
    property color neutralDot: "#6e6e73"
    // The fill under a hovered or pressed list item, ghost button, segmented option and menu entry; and the unfilled
    // part of a progress bar and the segmented select's track.
    property color hover: "#141d1d1f"
    property color track: "#e5e5ea"
    // A text input's, a text area's, a select's, a date input's, a check box's and a normal button's fill and outline;
    // the rule between the rows of a table or a divided list.
    property color controlSurface: "#ffffff"
    property color controlBorder: "#c7c7cc"
    property color divider: "#dcdce0"
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
    // A chart's series colours, in drawing order: `color.chart.1` to `color.chart.4`.
    property color chart1: "#0072b2"
    property color chart2: "#c25400"
    property color chart3: "#00805e"
    property color chart4: "#a8508a"
}
