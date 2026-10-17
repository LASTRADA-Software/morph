// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A button: a click, Space or Enter activates it.
MorphWidget {
    id: root

    fillsAcross: false

    property string label: ""
    // 0 normal (outlined), 1 primary (accent-filled), 2 danger (error-filled); set once.
    property int emphasis: 0
    readonly property color fill: emphasis === 1 ? root.scheme.accent : emphasis === 2 ? root.scheme.error
                                                                                       : root.scheme.surface

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
        palette.buttonText: root.emphasis === 0 ? root.scheme.text : "white"
        font.weight: root.emphasis === 0 ? Font.Normal : Font.DemiBold
        background: MorphButtonBackground {
            scheme: root.scheme
            fill: root.fill
            filled: root.emphasis !== 0
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
