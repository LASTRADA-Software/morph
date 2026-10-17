// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A button: a click, Space or Enter activates it.
MorphWidget {
    id: root

    fillsAcross: false

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
        leftPadding: Theme.spaceOf(4)
        rightPadding: Theme.spaceOf(4)
        palette.buttonText: root.scheme.text
        background: MorphButtonBackground {
            scheme: root.scheme
            hovered: button.hovered
            pressed: button.down
            focused: button.visualFocus
        }
        Accessible.ignored: true
        onClicked: root.fire("click", undefined)
        Keys.onReturnPressed: root.fire("click", undefined)
        Keys.onEnterPressed: root.fire("click", undefined)
    }
}
