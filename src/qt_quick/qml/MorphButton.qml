// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A button: a click, Space or Enter activates it.
MorphWidget {
    id: root

    property string label: ""

    focusTarget: button
    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight

    Button {
        id: button
        anchors.fill: parent
        text: root.label
        onClicked: root.fire("click", undefined)
        Keys.onReturnPressed: root.fire("click", undefined)
        Keys.onEnterPressed: root.fire("click", undefined)
    }
}
