// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic

// A themed button. `variant`: "default" | "primary" | "ghost" | "danger".
Button {
    id: ctrl
    property string variant: "default"

    font.weight: Font.DemiBold
    topPadding: 10
    bottomPadding: 10
    leftPadding: 18
    rightPadding: 18
    hoverEnabled: true

    contentItem: Text {
        text: ctrl.text
        font: ctrl.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: ctrl.variant === "primary" ? "#FFFFFF"
             : ctrl.variant === "danger" ? theme.bad
             : ctrl.variant === "ghost" ? theme.accent
             : theme.ink
    }

    background: Rectangle {
        radius: 9
        color: ctrl.variant === "primary" ? (ctrl.down ? theme.accentHover : theme.accent)
             : ctrl.variant === "ghost" ? "transparent"
             : ctrl.variant === "danger" ? (ctrl.hovered ? theme.badBg : "transparent")
             : (ctrl.hovered ? theme.surfaceAlt : theme.surface)
        border.width: (ctrl.variant === "primary" || ctrl.variant === "ghost") ? 0 : 1
        border.color: ctrl.variant === "danger" ? theme.dangerBorder : theme.border
    }
}
