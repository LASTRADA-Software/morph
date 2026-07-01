// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic

// A themed single-line text input.
TextField {
    id: ctrl
    color: theme.ink
    placeholderTextColor: theme.inkSoft
    selectionColor: theme.accent
    selectedTextColor: "#FFFFFF"
    topPadding: 10
    bottomPadding: 10
    leftPadding: 12
    rightPadding: 12

    background: Rectangle {
        radius: 9
        color: theme.surface
        border.width: 1
        border.color: ctrl.activeFocus ? theme.accent : theme.border
    }
}
