// SPDX-License-Identifier: Apache-2.0
import QtQuick

// What a button is drawn on: the control height and radius, outlined on the surface colour, a shade darker while
// hovered or pressed.
Rectangle {
    // The scheme of the widget the button belongs to.
    property MorphScheme scheme: Theme.themeScheme
    property bool hovered: false
    property bool pressed: false
    property bool focused: false
    // A filled button (primary, danger) is drawn in `fill` with no outline.
    property bool filled: false
    property color fill: scheme.surface

    implicitHeight: Theme.controlHeight
    implicitWidth: Theme.unitWidth * 8
    radius: Theme.radiusControl
    color: pressed ? Qt.darker(fill, 1.12) : hovered ? Qt.darker(fill, 1.05) : fill
    border.width: focused ? 2 : filled ? 0 : 1
    border.color: focused ? scheme.focus : scheme.border
}
