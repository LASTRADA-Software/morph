// SPDX-License-Identifier: Apache-2.0
pragma Singleton
import QtQuick

// The colours and units every MorphUi component reads. A unit is the default font's average character width
// horizontally and its line height vertically.
QtObject {
    readonly property FontMetrics metrics: FontMetrics {}
    readonly property real unitWidth: metrics.averageCharacterWidth
    readonly property real unitHeight: metrics.height
    readonly property real spacing: unitHeight / 2

    readonly property color text: "#1d1d1f"
    readonly property color muted: "#6e6e73"
    readonly property color error: "#c62828"
    readonly property color success: "#2e7d32"
    readonly property color info: "#1565c0"
    readonly property color warn: "#b26a00"
    readonly property color neutral: "#e5e5ea"
    readonly property color surface: "#ffffff"
    readonly property color border: "#c7c7cc"
    readonly property color selection: "#cfe3ff"
    readonly property real staleOpacity: 0.55

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
