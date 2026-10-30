// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A short label in a tone, with an optional icon, drawn as a tag: a small strong label on the tone's fill, with
// square-ish corners and no border. Outlined, it is drawn on the surface's fill with a border in the tone's outline
// colour. A removable badge is a chip, a control's label rather than a status: the control surface and border, the
// body font, and a remove button after the text, which takes the focus; a click on it, or Delete, Backspace, Enter
// or Space while it has the focus, removes the badge.
MorphWidget {
    id: root

    fillsAcross: false

    property int tone: 0
    property string text: ""
    property string icon: ""
    // Outlined: the surface's fill, with the tone's outline colour as border and its colour as text; set once.
    property bool outline: false
    // Whether the badge has a remove button, and the button's accessible name; set by C++ before the first frame.
    property bool removable: false
    property string removeLabel: ""
    // A neutral outline or chip takes the text colour and the control border; a toned one, the tone's colours.
    readonly property bool plain: root.tone === 0 && (root.outline || root.removable)
    readonly property bool framed: root.outline || root.removable

    focusTarget: root.removable ? remove : root

    function removeIt() {
        root.fire("remove", undefined)
    }

    // The icon's URL in the theme's set; without one, the icon's name is drawn before the text, as text.
    readonly property string iconSource: Theme.iconSource(root.icon)

    // A tag insets its label 1 px above and below and one and a half spacing units at each side. A chip is the
    // control height less 6 px, with a spacing step of 2 before the label and its remove button against the end.
    readonly property real chipHeight: Theme.targetHeight - 6
    implicitWidth: content.implicitWidth + (root.removable ? Theme.spaceOf(2) : Theme.spaceOf(1) * 3)
    implicitHeight: root.removable ? root.chipHeight : label.implicitHeight + 2

    Rectangle {
        objectName: "fill"
        anchors.fill: parent
        radius: Theme.radiusBadge
        color: root.removable ? root.scheme.controlSurface
                              : root.outline ? root.scheme.surface : root.toneBgOf(root.tone)
        border.width: root.framed ? 1 : 0
        border.color: root.tone === 0 ? root.scheme.controlBorder : root.toneBorderOf(root.tone)
    }

    Row {
        id: content
        x: root.removable ? Theme.spaceOf(2) : Theme.spaceOf(1) * 1.5
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.spaceOf(1)

        Image {
            objectName: "icon"
            visible: root.iconSource !== ""
            source: root.iconSource
            sourceSize.height: label.implicitHeight
            height: label.implicitHeight
            width: visible ? height : 0
            anchors.verticalCenter: parent.verticalCenter
        }

        Label {
            textFormat: Text.PlainText
            id: label
            objectName: "label"
            font: root.removable ? Theme.bodyFont : Theme.smallStrongFont
            text: (root.icon !== "" && root.iconSource === "" ? root.icon + " " : "") + root.text
            color: root.plain ? root.scheme.text : root.toneOf(root.tone)
            anchors.verticalCenter: parent.verticalCenter
        }

        ToolButton {
            id: remove
            objectName: "remove"
            visible: root.removable
            focusPolicy: Qt.StrongFocus
            padding: 0
            height: root.chipHeight - 2
            width: visible ? height : 0
            anchors.verticalCenter: parent.verticalCenter
            icon.source: Theme.iconSource("close")
            icon.color: "transparent"
            text: icon.source.toString() !== "" ? "" : "×"
            font: Theme.bodyFont
            palette.buttonText: root.scheme.muted
            Accessible.name: root.removeLabel
            Accessible.role: Accessible.Button
            onClicked: root.removeIt()
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace || event.key === Qt.Key_Return
                        || event.key === Qt.Key_Enter) {
                    event.accepted = true
                    root.removeIt()
                }
            }
        }
    }
}
