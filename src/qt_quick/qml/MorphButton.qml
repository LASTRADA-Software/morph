// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A button: a click, Space or Enter activates it. A list item (`style` 1) is the same button drawn as a line of a
// list: as wide as its line, its label at the start, unframed, and marked while `selected`.
MorphWidget {
    id: root

    fillsAcross: root.style === 1

    property string label: ""
    // An icon of the theme's icon set, drawn beside the label when the set names it; set once.
    property string icon: ""
    // 0 normal (outlined), 1 primary (accent-filled), 2 danger (error-filled), 3 ghost (no chrome of its own), 4
    // danger outlined (a normal frame with the error colour's border and label); set once.
    property int emphasis: 0
    // 0 push, 1 item; set once.
    property int style: 0
    // Whether an item is the current entry of its list, or a push button is pressed in.
    property bool selected: false
    // Whether the document gave the button a selection: set with the first `selected`. Such a push button is a toggle
    // to assistive technology. The control is not `checkable`, since a click would flip `checked` whatever the
    // document answers.
    property bool selectable: false
    readonly property bool ghost: emphasis === 3
    readonly property color fill: emphasis === 1 ? root.scheme.accent
                                 : emphasis === 2 ? root.scheme.error
                                 : ghost ? "transparent"
                                         : root.scheme.surface

    // The widget's item is the one node a driver finds by test id, so it is the button: it carries the role, the
    // label as its name and the press action, and the control inside it is not a second node.
    defaultName: label
    defaultRole: Accessible.Button
    Accessible.checkable: root.selectable && root.style === 0
    Accessible.checked: root.selectable && root.style === 0 && root.selected
    Accessible.onPressAction: root.fire("click", undefined)

    focusTarget: button
    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight

    // An item's content: the icon, then the label at the start of the line, in the accent while selected. The push
    // button keeps its style's own centred content.
    onStyleChanged: {
        if (root.style === 1)
            button.contentItem = itemContent.createObject(button)
    }

    Component {
        id: itemContent

        RowLayout {
            spacing: Theme.spaceOf(2)

            Image {
                source: Theme.iconSource(root.icon)
                visible: root.icon !== "" && source.toString() !== ""
                Layout.preferredWidth: Theme.bodyPixels
                Layout.preferredHeight: Theme.bodyPixels
            }
            Label {
                textFormat: Text.PlainText
                text: root.label
                font: Theme.bodyFont
                color: root.selected ? root.scheme.accent : root.scheme.text
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignVCenter
                Layout.fillWidth: true
                Accessible.ignored: true
            }
        }
    }

    Button {
        id: button
        anchors.fill: parent
        text: root.label
        leftPadding: root.ghost ? Theme.spaceOf(1) : root.style === 1 ? Theme.spaceOf(3) : Theme.spaceOf(4)
        rightPadding: root.ghost ? Theme.spaceOf(1) : root.style === 1 ? Theme.spaceOf(3) : Theme.spaceOf(4)
        palette.buttonText: root.emphasis === 4 ? root.scheme.error
                                                : root.emphasis === 0 || root.ghost ? root.scheme.text : "white"
        font: root.emphasis === 0 || root.emphasis === 4 || root.ghost ? Theme.bodyFont : Theme.strongFont
        icon.source: Theme.iconSource(root.icon)
        icon.color: "transparent"
        contentItem: MorphPlainContent {
            control: button
            color: button.palette.buttonText
        }
        background: MorphButtonBackground {
            scheme: root.scheme
            fill: root.fill
            filled: root.emphasis === 1 || root.emphasis === 2
            item: root.style === 1
            ghost: root.ghost
            borderColor: root.emphasis === 4 ? root.scheme.errorBorder : root.scheme.border
            selected: root.selected
            hovered: button.hovered
            pressed: button.down
            focused: button.visualFocus
        }
        Accessible.ignored: true
        onClicked: root.fire("click", undefined)
        // A chord the button declares comes before its own click; Return and Enter click it otherwise.
        Keys.onReturnPressed: event => {
            if (!root.takeOwnChord(event))
                root.fire("click", undefined)
        }
        Keys.onEnterPressed: event => {
            if (!root.takeOwnChord(event))
                root.fire("click", undefined)
        }
        Keys.onPressed: event => event.accepted = root.takeOwnChord(event)
    }
}
