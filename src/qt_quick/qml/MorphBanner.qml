// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A message across the content in a tone, with an optional action button and, when dismissible, a close button.
// A dismissal changes nothing it shows: the application hides it.
MorphWidget {
    id: root

    property int tone: 0
    // The characters shown: the message, or its runs joined.
    property string text: ""
    // The message's runs, as a text's; while there are any, the label shows their markup.
    property var runs: []
    property string actionLabel: ""
    property bool dismissible: false

    implicitWidth: row.implicitWidth + 2 * Theme.unitWidth
    implicitHeight: row.implicitHeight + Theme.unitHeight
    Layout.fillWidth: true

    Rectangle {
        anchors.fill: parent
        // Over the surface, so a translucent fill (neutral's) still hides what is behind.
        color: Qt.tint(root.scheme.surface, root.toneBgOf(root.tone))
        border.color: root.toneBorderOf(root.tone)
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Theme.unitWidth
        anchors.rightMargin: Theme.unitWidth

        // The tone's icon, when the theme's icon set names it.
        Image {
            objectName: "toneIcon"
            readonly property string name: ["", "tone.info", "tone.ok", "tone.warn", "tone.err"][root.tone] ?? ""
            visible: source.toString() !== ""
            source: Theme.iconSource(name)
            sourceSize.height: Theme.unitHeight
            Layout.preferredHeight: Theme.unitHeight
            Layout.preferredWidth: visible ? Theme.unitHeight : 0
        }

        Label {
            objectName: "message"
            textFormat: root.runs.length > 0 ? Text.RichText : Text.PlainText
            font: Theme.bodyFont
            Layout.fillWidth: true
            text: root.runs.length > 0 ? root.runsHtml(root.runs) : root.text
            wrapMode: Text.Wrap
        }

        Button {
            id: action
            font: Theme.bodyFont
            objectName: "action"
            visible: root.actionLabel !== ""
            text: root.actionLabel
            contentItem: MorphPlainContent {
                control: action
            }
            onClicked: root.fire("action", undefined)
        }

        ToolButton {
            font: Theme.bodyFont
            objectName: "dismiss"
            visible: root.dismissible
            icon.source: Theme.iconSource("close")
            icon.color: "transparent"
            text: icon.source.toString() !== "" ? "" : "×"
            onClicked: root.fire("dismiss", undefined)
        }
    }
}
