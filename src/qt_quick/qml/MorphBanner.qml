// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A message across the content in a tone, with an optional action button and, when dismissible, a close button.
// A dismissal changes nothing it shows: the application hides it.
MorphWidget {
    id: root

    property int tone: 0
    property string text: ""
    property string actionLabel: ""
    property bool dismissible: false

    implicitWidth: row.implicitWidth + 2 * Theme.unitWidth
    implicitHeight: row.implicitHeight + Theme.unitHeight
    Layout.fillWidth: true

    Rectangle {
        anchors.fill: parent
        // Over the surface, so a translucent fill (neutral's) still hides what is behind.
        color: Qt.tint(root.scheme.surface, root.toneBgOf(root.tone))
        border.color: root.toneBorderOf(root.tone)
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Theme.unitWidth
        anchors.rightMargin: Theme.unitWidth

        Label {
            Layout.fillWidth: true
            text: root.text
            wrapMode: Text.Wrap
        }

        Button {
            objectName: "action"
            visible: root.actionLabel !== ""
            text: root.actionLabel
            onClicked: root.fire("action", undefined)
        }

        ToolButton {
            objectName: "dismiss"
            visible: root.dismissible
            text: "×"
            onClicked: root.fire("dismiss", undefined)
        }
    }
}
