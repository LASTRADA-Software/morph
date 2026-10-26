// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What a region shows when it has nothing to show: an icon, a title, a text and an optional action.
MorphWidget {
    id: root

    property string title: ""
    property string text: ""
    property string icon: ""
    property string actionLabel: ""

    // The icon's URL in the theme's set: the node's own, else the set's `empty`.
    readonly property string iconSource: Theme.iconSource(root.icon !== "" ? root.icon : "empty")

    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.fill: parent

        Image {
            objectName: "icon"
            Layout.alignment: Qt.AlignHCenter
            visible: root.iconSource !== ""
            source: root.iconSource
            sourceSize.height: Theme.unitHeight * 2
            Layout.preferredHeight: Theme.unitHeight * 2
            Layout.preferredWidth: Theme.unitHeight * 2
        }

        Label {
            textFormat: Text.PlainText
            Layout.alignment: Qt.AlignHCenter
            visible: root.icon !== "" && root.iconSource === ""
            text: root.icon
            font.pixelSize: Theme.unitHeight * 2
        }

        Label {
            textFormat: Text.PlainText
            font: Theme.strongFont
            Layout.alignment: Qt.AlignHCenter
            text: root.title
        }

        Label {
            textFormat: Text.PlainText
            font: Theme.bodyFont
            Layout.alignment: Qt.AlignHCenter
            visible: root.text !== ""
            text: root.text
            color: root.scheme.muted
            wrapMode: Text.Wrap
        }

        Button {
            id: action
            font: Theme.bodyFont
            objectName: "action"
            contentItem: MorphPlainContent {
                control: action
            }
            Layout.alignment: Qt.AlignHCenter
            visible: root.actionLabel !== ""
            text: root.actionLabel
            onClicked: root.fire("action", undefined)
        }
    }
}
