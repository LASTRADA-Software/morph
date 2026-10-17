// SPDX-License-Identifier: Apache-2.0
import QtQuick

// What an input (a text field, a text area, a select) is drawn on: the control height, the control radius, and a
// border that takes the focus colour while the input has focus.
Rectangle {
    // The scheme of the widget the input belongs to.
    property MorphScheme scheme: Theme.themeScheme
    property bool focused: false

    implicitHeight: Theme.controlHeight
    implicitWidth: Theme.unitWidth * 20
    radius: Theme.radiusControl
    color: scheme.surface
    border.width: focused ? 2 : 1
    border.color: focused ? scheme.focus : scheme.border
}
