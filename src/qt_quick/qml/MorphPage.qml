// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A sheet of fixed physical size: 96 device-independent pixels to 25.4 mm. The sheet is laid out at its own size in
// `sheet`, and the item displays it scaled by `scaleFactor`, which C++ computes (`ui::layoutPage`) from the width the
// layout gives the item. The item's own size is the displayed size: its natural width is the sheet's, it can be
// narrower and has no least width, and its height follows the width it is given.
MorphWidget {
    id: root

    // The sheet's size in pixels, written once by C++.
    property real sheetWidth: 794
    property real sheetHeight: 1123
    // `min(1, available / sheet width)`.
    property real scaleFactor: 1
    readonly property Item content: sheet

    implicitWidth: root.sheetWidth
    implicitHeight: root.sheetHeight * root.scaleFactor
    Layout.fillWidth: true
    Layout.minimumWidth: 0
    Layout.maximumWidth: root.sheetWidth
    defaultRole: Accessible.Document

    // The width the layout gave, in millimetres, for C++ to scale by. The scale changes the height, which the layout
    // may answer with another width, so the report is made once the layout pass is over and says the width it ends
    // with.
    function reportWidth() {
        if (root.width > 0)
            root.fire("available", root.width * 25.4 / 96)
    }
    onWidthChanged: Qt.callLater(root.reportWidth)

    Item {
        id: sheet
        width: root.sheetWidth
        height: root.sheetHeight
        scale: root.scaleFactor
        transformOrigin: Item.TopLeft
        clip: true
    }

    // The sheet's edge, over its content.
    Rectangle {
        anchors.fill: parent
        z: 90
        color: "transparent"
        border.width: 1
        border.color: root.scheme.border
    }
    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: Theme.cardShadowOffsetX - Theme.cardShadowSpread
        anchors.rightMargin: -Theme.cardShadowOffsetX - Theme.cardShadowSpread
        anchors.topMargin: Theme.cardShadowOffsetY - Theme.cardShadowSpread
        anchors.bottomMargin: -Theme.cardShadowOffsetY - Theme.cardShadowSpread
        z: -2
        color: Theme.cardShadowColor
        radius: Theme.cardShadowBlur / 4
    }
}
