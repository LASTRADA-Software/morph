// SPDX-License-Identifier: Apache-2.0
pragma Singleton
import QtQuick

// The colours, units and spacing every MorphUi component reads. A unit is the default font's average character width
// horizontally and its line height vertically. The colours, the stale opacity and the spacing are the ui-theme/1
// defaults; qt_quick::applyTheme sets them from a theme's tokens, and a token the theme lacks keeps its default here.
QtObject {
    readonly property FontMetrics metrics: FontMetrics {}
    readonly property real unitWidth: metrics.averageCharacterWidth
    readonly property real unitHeight: metrics.height
    readonly property real spacing: unitHeight / 2

    property color text: "#1d1d1f"
    property color muted: "#6e6e73"
    property color error: "#c62828"
    property color success: "#2e7d32"
    property color info: "#1565c0"
    property color warn: "#b26a00"
    property color neutral: "#e5e5ea"
    property color surface: "#ffffff"
    property color border: "#c7c7cc"
    property color selection: "#cfe3ff"
    property color accent: "#1565c0"
    property color focus: "#1565c0"
    property real staleOpacity: 0.55
    // The spacing scale in px, one entry per spacing step 0 to 5: the `space.<step>` defaults.
    property list<real> space: [0, 4, 8, 12, 16, 24]

    // A spacing step's distance; a step outside 0 to 5 is the nearest end, as the mount already clamps it.
    function spaceOf(step: int): real {
        return space[Math.max(0, Math.min(space.length - 1, step))]
    }

    // A text role's colour: Normal, Muted, Heading, Error, Success.
    function textColor(role: int): color {
        switch (role) {
        case 1:
            return muted
        case 3:
            return error
        case 4:
            return success
        default:
            return text
        }
    }

    // A tone's colour: Neutral, Info, Ok, Warn, Err.
    function tone(index: int): color {
        switch (index) {
        case 1:
            return info
        case 2:
            return success
        case 3:
            return warn
        case 4:
            return error
        default:
            return muted
        }
    }
}
