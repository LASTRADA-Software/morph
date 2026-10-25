// SPDX-License-Identifier: Apache-2.0
import QtQuick

// What a button is drawn on: the control height and radius, outlined on the surface colour, a shade darker while
// hovered or pressed. A list item's is unframed: clear, the neutral colour while hovered, and while selected an accent
// tint with a bar at its leading edge. A selected push button keeps its frame and size and draws an accent ring outside
// its border; unless filled it also takes the item's accent tint, since the default theme's focus and accent colours
// are the same and a ring alone would read as focus. A ghost button is unframed the same way but has no "current" concept: just a
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
    // A list item, and whether it is the current entry; a push button, whether it is pressed in.
    property bool item: false
    property bool selected: false
    // A ghost button: no border or fill, no accent-bar "current" concept, just a neutral tint on hover or press.
    property bool ghost: false

    implicitHeight: Theme.targetHeight
    implicitWidth: ghost ? 0 : Theme.unitWidth * 8
    radius: Theme.radiusControl
    // The fill a push button shades while hovered or pressed: the tint while selected, unless the button is filled.
    readonly property color restFill: selected && !filled ? Qt.alpha(scheme.accent, 0.12) : fill

    color: item ? (selected ? Qt.alpha(scheme.accent, 0.12) : hovered || pressed ? scheme.neutral : "transparent")
                : ghost ? (hovered || pressed ? scheme.neutral : "transparent")
                        : pressed ? Qt.darker(restFill, 1.12) : hovered ? Qt.darker(restFill, 1.05) : restFill
    border.width: focused ? 2 : filled || item || ghost ? 0 : 1
    border.color: focused ? scheme.focus : scheme.border

    Rectangle {
        visible: background.item && background.selected
        width: 3
        height: parent.height
        color: background.scheme.accent
    }

    // Outside the border, so the button's size and its label's place do not change with the selection.
    Rectangle {
        objectName: "selectionRing"
        visible: !background.item && !background.ghost && background.selected
        anchors.fill: parent
        anchors.margins: -2
        radius: background.radius + 2
        color: "transparent"
        border.width: 2
        border.color: background.scheme.accent
    }
}
