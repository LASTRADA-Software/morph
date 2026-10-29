// SPDX-License-Identifier: Apache-2.0
import QtQuick

// The frame of a popup that opens from a control (a menu, the date picker, a select's drop-down): the card scheme's
// surface and border, the control radius, and the popup shadow below it.
Rectangle {
    id: root

    property MorphScheme scheme: Theme.cardScheme

    color: root.scheme.surface
    border.width: 1
    border.color: root.scheme.border
    radius: Theme.radiusControl

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: Theme.popupShadowOffsetX - Theme.popupShadowSpread
        anchors.rightMargin: -Theme.popupShadowOffsetX - Theme.popupShadowSpread
        anchors.topMargin: Theme.popupShadowOffsetY - Theme.popupShadowSpread
        anchors.bottomMargin: -Theme.popupShadowOffsetY - Theme.popupShadowSpread
        z: -1
        radius: root.radius + Theme.popupShadowBlur / 4
        color: Theme.popupShadowColor
    }
}
