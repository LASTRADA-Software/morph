// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A short label in a tone, with an optional icon: filled in the tone, or outlined on the surface. A removable badge
// draws a remove button after its text, which takes the focus; a click on it, or Delete, Backspace, Enter or Space
// while it has the focus, removes the badge.
MorphWidget {
    id: root

    fillsAcross: false

    property int tone: 0
    property string text: ""
    property string icon: ""
    // Outlined: the surface's fill, with the tone's colour as border and text; set once.
    property bool outline: false
    // Whether the badge has a remove button, and the button's accessible name; set by C++ before the first frame.
    property bool removable: false
    property string removeLabel: ""

    focusTarget: root.removable ? remove : root

    function removeIt() {
        root.fire("remove", undefined)
    }

    // The icon's URL in the theme's set; without one, the icon's name is drawn before the text, as text.
    readonly property string iconSource: Theme.iconSource(root.icon)

    // A fixed space step, not a font-metric-derived width: a short label (a status word) still reads as an
    // oblong pill rather than nearly as wide as it is tall.
    implicitWidth: content.implicitWidth + Theme.spaceOf(2) * 2
    implicitHeight: label.implicitHeight + Theme.spaceOf(1) * 2

    Rectangle {
        objectName: "fill"
        anchors.fill: parent
        radius: height / 2
        color: root.outline ? root.scheme.surface : root.toneBgOf(root.tone)
        border.color: root.outline ? root.toneOf(root.tone) : root.toneBorderOf(root.tone)
    }

    Row {
        id: content
        anchors.centerIn: parent
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
            font: Theme.bodyFont
            text: (root.icon !== "" && root.iconSource === "" ? root.icon + " " : "") + root.text
            color: root.toneOf(root.tone)
        }

        ToolButton {
            id: remove
            objectName: "remove"
            visible: root.removable
            focusPolicy: Qt.StrongFocus
            padding: 0
            height: label.implicitHeight
            width: visible ? label.implicitHeight : 0
            anchors.verticalCenter: parent.verticalCenter
            icon.source: Theme.iconSource("close")
            icon.color: "transparent"
            text: icon.source.toString() !== "" ? "" : "×"
            font: Theme.bodyFont
            palette.buttonText: root.toneOf(root.tone)
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
