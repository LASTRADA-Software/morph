// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A busy indicator with a label, both shown while active; inactive, it draws nothing and takes no room.
MorphWidget {
    id: root

    property bool active: false
    property string label: ""

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    Row {
        id: row
        spacing: Theme.unitWidth

        BusyIndicator {
            running: root.active
            visible: root.active
            height: Theme.unitHeight * 1.5
            width: height
        }

        Label {
            objectName: "label"
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            visible: root.active
        }
    }
}
