// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic

// A themed ComboBox; pairs with QVariantList models using textRole/valueRole.
ComboBox {
    id: ctrl
    font.weight: Font.Medium

    background: Rectangle {
        radius: 9
        implicitHeight: 40
        color: theme.surface
        border.width: 1
        border.color: ctrl.activeFocus ? theme.accent : theme.border
    }

    leftPadding: 12
    rightPadding: 32

    contentItem: Text {
        text: ctrl.displayText
        color: theme.ink
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
