// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A button: a click, Space or Enter activates it.
MorphWidget {
    id: root

    property string label: ""

    // The widget's item is the one node a driver finds by test id, so it is the button: it carries the role, the
    // label as its name and the press action, and the control inside it is not a second node.
    defaultName: label
    defaultRole: Accessible.Button
    Accessible.onPressAction: root.fire("click", undefined)

    focusTarget: button
    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight

    Button {
        id: button
        anchors.fill: parent
        text: root.label
        Accessible.ignored: true
        onClicked: root.fire("click", undefined)
        Keys.onReturnPressed: root.fire("click", undefined)
        Keys.onEnterPressed: root.fire("click", undefined)
    }
}
