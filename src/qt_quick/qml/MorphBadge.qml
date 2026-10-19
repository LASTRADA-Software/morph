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

    implicitWidth: label.implicitWidth + Theme.unitWidth * 2
    implicitHeight: label.implicitHeight + 2

    Rectangle {
        objectName: "fill"
        anchors.fill: parent
        radius: height / 2
        color: root.toneBgOf(root.tone)
        border.color: root.toneBorderOf(root.tone)
    }

    Label {
        font: Theme.bodyFont
        id: label
        anchors.centerIn: parent
        text: (root.icon !== "" ? root.icon + " " : "") + root.text
        color: root.toneOf(root.tone)
    }
}
