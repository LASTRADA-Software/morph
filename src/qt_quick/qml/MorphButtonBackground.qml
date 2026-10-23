// SPDX-License-Identifier: Apache-2.0
import QtQuick

// What a button is drawn on: the control height and radius, outlined on the surface colour, a shade darker while
// hovered or pressed. A list item's is unframed: clear, the neutral colour while hovered, and while selected an accent
// tint with a bar at its leading edge. A ghost button is unframed the same way but has no "current" concept: just a
// neutral tint while hovered or pressed, transparent otherwise.
Rectangle {
    id: background

    // The scheme of the widget the button belongs to.
    property MorphScheme scheme: Theme.themeScheme
    property bool hovered: false
    property bool pressed: false
    property bool focused: false
    // A filled button (primary, danger) is drawn in `fill` with no outline.
    property bool filled: false
    property color fill: scheme.surface
    // A list item, and whether it is the current entry.
    property bool item: false
    property bool selected: false
    // A ghost button: no border or fill, no accent-bar "current" concept, just a neutral tint on hover or press.
    property bool ghost: false

    implicitHeight: Theme.controlHeight
    implicitWidth: ghost ? 0 : Theme.unitWidth * 8
    radius: Theme.radiusControl
    color: item ? (selected ? Qt.alpha(scheme.accent, 0.12) : hovered || pressed ? scheme.neutral : "transparent")
                : ghost ? (hovered || pressed ? scheme.neutral : "transparent")
                        : pressed ? Qt.darker(fill, 1.12) : hovered ? Qt.darker(fill, 1.05) : fill
    border.width: focused ? 2 : filled || item || ghost ? 0 : 1
    border.color: focused ? scheme.focus : scheme.border

    Rectangle {
        visible: background.item && background.selected
        width: 3
        height: parent.height
        color: background.scheme.accent
    }
}
