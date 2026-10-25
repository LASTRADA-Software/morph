// SPDX-License-Identifier: Apache-2.0
import QtQuick

// What an input (a text field, a text area, a select) is drawn on: the control height, the control radius, and a
// border that takes the focus colour while the input has focus. A read-only input draws on the page's background
// rather than the input fill, so it does not invite typing; it keeps the border and the focus ring, since it still
// takes focus for selection.
Rectangle {
    // The scheme of the widget the input belongs to.
    property MorphScheme scheme: Theme.themeScheme
    property bool focused: false
    property bool readOnly: false

    implicitHeight: Theme.targetHeight
    implicitWidth: Theme.unitWidth * 20
    radius: Theme.radiusControl
    color: readOnly ? Theme.background : scheme.surface
    border.width: focused ? 2 : 1
    border.color: focused ? scheme.focus : scheme.border
}
