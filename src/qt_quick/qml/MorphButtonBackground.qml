// SPDX-License-Identifier: Apache-2.0
import QtQuick

// What a button is drawn on: the control height and radius, outlined on the surface colour, a shade darker while
// hovered or pressed. A list item's is unframed: clear, the hover colour while hovered, and while selected the selection
// colour with an accent bar at its leading edge, unless the surface's list selection is a fill, which has no bar. A selected push button keeps its frame and size and draws an accent ring outside
// its border; unless filled it also takes the item's accent tint, since the default theme's focus and accent colours
// are the same and a ring alone would read as focus. A ghost button is unframed the same way but has no "current"
// concept: just the hover tint while hovered or pressed, transparent otherwise. A round button's corners are half its
// height; a compact one has no least width. On a surface whose outlines are dashed, a framed button's outline is
// stroked in dashes instead of drawn solid.
Rectangle {
    id: background

    // The scheme of the widget the button belongs to.
    property MorphScheme scheme: Theme.themeScheme
    property bool hovered: false
    property bool pressed: false
    property bool focused: false
    // A filled button (primary, danger) is drawn in `fill` with no outline.
    property bool filled: false
    property color fill: scheme.controlSurface
    // A list item, and whether it is the current entry; a push button, whether it is pressed in.
    property bool item: false
    property bool selected: false
    // A ghost button: no border or fill, no accent-bar "current" concept, just the hover tint on hover or press.
    property bool ghost: false
    // The outline's colour while it is not focused: the border, or the error colour's for a danger outlined button.
    property color borderColor: scheme.controlBorder
    property bool round: false
    // A dense button: no least width of its own.
    property bool compact: false
    // A square button for a glyph: no least width, and the control's own corners.
    property bool square: false
    // Whether the outline is stroked in dashes: the surface's `border.style`, for a button that has an outline.
    readonly property bool dashed: scheme.borderDashed && !filled && !item && !ghost

    implicitHeight: Theme.targetHeight
    implicitWidth: ghost || compact || square ? 0 : Theme.unitWidth * 8
    radius: round ? height / 2 : Theme.radiusControl
    // The fill a push button shades while hovered or pressed: the tint while selected, unless the button is filled.
    readonly property color restFill: selected && !filled ? Qt.alpha(scheme.accent, 0.12) : fill

    color: item ? (selected ? scheme.selection : hovered || pressed ? scheme.hover : "transparent")
                : ghost ? (hovered || pressed ? scheme.hover : "transparent")
                        : pressed ? Qt.darker(restFill, 1.12) : hovered ? Qt.darker(restFill, 1.05) : restFill
    border.width: focused ? 2 : filled || item || ghost || dashed ? 0 : 1
    border.color: focused ? scheme.focus : borderColor

    MorphDashedOutline {
        objectName: "dashedOutline"
        visible: background.dashed && !background.focused
        color: background.borderColor
        radius: background.radius
    }

    Rectangle {
        visible: background.item && background.selected && !background.scheme.listSelectionFill
        width: 3
        height: parent.height
        color: background.scheme.accent
    }

    // Outside the border, so the button's size and its label's place do not change with the selection.
    Rectangle {
        objectName: "selectionRing"
        visible: !background.item && !background.ghost && background.selected
        anchors.fill: parent
        anchors.margins: -3
        radius: background.radius + 3
        color: "transparent"
        border.width: 3
        border.color: background.scheme.accent
    }
}
