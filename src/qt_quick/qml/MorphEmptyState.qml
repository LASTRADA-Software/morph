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

    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.fill: parent

        Label {
            Layout.alignment: Qt.AlignHCenter
            visible: root.icon !== ""
            text: root.icon
            font.pixelSize: Theme.unitHeight * 2
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: root.title
            font.bold: true
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            visible: root.text !== ""
            text: root.text
            color: root.scheme.muted
            wrapMode: Text.Wrap
        }

        Button {
            objectName: "action"
            Layout.alignment: Qt.AlignHCenter
            visible: root.actionLabel !== ""
            text: root.actionLabel
            onClicked: root.fire("action", undefined)
        }
    }
}
