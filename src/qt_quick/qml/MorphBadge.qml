// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A short label in a tone, with an optional icon.
MorphWidget {
    id: root

    fillsAcross: false

    property int tone: 0
    property string text: ""
    property string icon: ""

    // The icon's URL in the theme's set; without one, the icon's name is drawn before the text, as text.
    readonly property string iconSource: Theme.iconSource(root.icon)

    implicitWidth: content.implicitWidth + Theme.unitWidth * 2
    implicitHeight: label.implicitHeight + 2

    Rectangle {
        objectName: "fill"
        anchors.fill: parent
        radius: height / 2
        color: root.toneBgOf(root.tone)
        border.color: root.toneBorderOf(root.tone)
    }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: Theme.spaceOf(1)

        Image {
            objectName: "icon"
            visible: root.iconSource !== ""
            source: root.iconSource
            sourceSize.height: label.implicitHeight
            height: label.implicitHeight
            width: visible ? height : 0
            anchors.verticalCenter: parent.verticalCenter
        }

        Label {
            id: label
            font: Theme.bodyFont
            text: (root.icon !== "" && root.iconSource === "" ? root.icon + " " : "") + root.text
            color: root.toneOf(root.tone)
        }
    }
}
