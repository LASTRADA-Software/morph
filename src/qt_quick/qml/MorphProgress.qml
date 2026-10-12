// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A progress bar with a label; a negative value is indeterminate.
MorphWidget {
    id: root

    property real value: -1
    property string label: ""

    implicitWidth: Math.max(bar.implicitWidth, caption.implicitWidth)
    implicitHeight: bar.implicitHeight + (caption.visible ? caption.implicitHeight : 0)

    Column {
        anchors.fill: parent

        ProgressBar {
            id: bar
            width: parent.width
            indeterminate: root.value < 0
            value: root.value < 0 ? 0 : root.value
        }

        Label {
            id: caption
            visible: root.label !== ""
            text: root.label
        }
    }
}
