// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A message across the content in a tone, with an optional action and, when dismissible, a close button. The frame
// is rounded like a control, and the message is drawn in the tone's colour (the text colour for a neutral banner),
// its strong runs strong in that colour. The action is by default a ghost button at the trailing edge: unframed, its
// label in the accent. `actionStyle` 1 frames it; 2 draws it as a link inside the message, at the run marked `action`
// or else after the message, with an accessible proxy that keeps it a button to assistive technology; `actionStretch`
// puts the button on a line of its own under the message, as wide as the banner. A dismissal changes nothing it shows:
// the application hides it.
MorphWidget {
    id: root

    property int tone: 0
    // The characters shown: the message, or its runs joined.
    property string text: ""
    // The message's runs, as a text's; while there are any, the label shows their markup.
    property var runs: []
    property string actionLabel: ""
    property bool dismissible: false
    // How the action is drawn: 0 ghost, 1 button, 2 link.
    property int actionStyle: 0
    property bool actionStretch: false
    readonly property bool link: root.actionStyle === 2 && root.actionLabel !== ""
    readonly property bool stretched: root.actionStretch && root.actionStyle !== 2

    // The runs the label draws. A link banner marks its action among them; one whose runs name none gets the label
    // after the message.
    readonly property var messageRuns: {
        if (!root.link)
            return root.runs
        const base = root.runs.length > 0 ? root.runs : (root.text !== "" ? [plainRun(escaped(root.text))] : [])
        if (base.some(run => run.action))
            return base
        const action = plainRun(escaped(root.actionLabel))
        action.action = true
        return base.concat([plainRun(" "), action])
    }

    function escaped(value: string): string {
        return value.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    // A run of already escaped text in the message's own weight, face and colour.
    function plainRun(value: string): var {
        return {text: value, strong: false, mono: false, tone: 0, action: false}
    }

    implicitWidth: grid.implicitWidth + 2 * Theme.spaceOf(3)
    implicitHeight: grid.implicitHeight + 2 * Theme.spaceOf(2)
    Layout.fillWidth: true

    Rectangle {
        objectName: "frame"
        anchors.fill: parent
        radius: Theme.radiusControl
        // Over the surface, so a translucent fill (neutral's) still hides what is behind.
        color: Qt.tint(root.scheme.surface, root.toneBgOf(root.tone))
        border.color: root.toneBorderOf(root.tone)
    }

    // Rich text exposes no accessible node for an anchor, so this one-pixel proxy carries the link: a button named
    // by the action's label, reachable by Tab, fired by Enter and Space. The label's own text stays the banner's
    // accessible description.
    Item {
        id: proxy
        objectName: "actionProxy"
        visible: root.link
        width: 1
        height: 1
        activeFocusOnTab: root.link
        Accessible.role: Accessible.Button
        Accessible.name: root.actionLabel
        Accessible.focusable: true
        Accessible.onPressAction: root.fire("action", undefined)
        Keys.onReturnPressed: root.fire("action", undefined)
        Keys.onEnterPressed: root.fire("action", undefined)
        Keys.onSpacePressed: root.fire("action", undefined)
        Binding {
            target: root
            property: "linkFocused"
            value: proxy.activeFocus
        }
    }

    GridLayout {
        id: grid
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceOf(3)
        anchors.rightMargin: Theme.spaceOf(3)
        anchors.topMargin: Theme.spaceOf(2)
        anchors.bottomMargin: Theme.spaceOf(2)
        columns: 4
        rowSpacing: Theme.spaceOf(2)
        columnSpacing: 5

        // The tone's icon, when the theme's icon set names it.
        Image {
            objectName: "toneIcon"
            readonly property string name: ["", "tone.info", "tone.ok", "tone.warn", "tone.err"][root.tone] ?? ""
            visible: source.toString() !== ""
            source: Theme.iconSource(name)
            sourceSize.height: Theme.unitHeight
            Layout.row: 0
            Layout.column: 0
            Layout.preferredHeight: Theme.unitHeight
            Layout.preferredWidth: visible ? Theme.unitHeight : 0
        }

        Label {
            objectName: "message"
            textFormat: root.messageRuns.length > 0 ? Text.RichText : Text.PlainText
            font: Theme.bodyFont
            color: root.tone === 0 ? root.scheme.text : root.toneOf(root.tone)
            linkColor: root.scheme.accent
            Layout.row: 0
            Layout.column: 1
            Layout.fillWidth: true
            text: root.messageRuns.length > 0 ? root.runsHtml(root.messageRuns) : root.text
            wrapMode: Text.Wrap
            onLinkActivated: link => {
                if (link === "action")
                    root.fire("action", undefined)
            }
        }

        Button {
            id: action
            font: Theme.bodyFont
            objectName: "action"
            visible: root.actionLabel !== "" && !root.link
            text: root.actionLabel
            leftPadding: Theme.spaceOf(root.actionStyle === 1 ? 3 : 1)
            rightPadding: Theme.spaceOf(root.actionStyle === 1 ? 3 : 1)
            palette.buttonText: root.actionStyle === 1 ? root.scheme.text : root.scheme.accent
            Layout.row: root.stretched ? 1 : 0
            Layout.column: root.stretched ? 0 : 2
            Layout.columnSpan: root.stretched ? 4 : 1
            Layout.fillWidth: root.stretched
            contentItem: MorphPlainContent {
                control: action
                color: action.palette.buttonText
            }
            background: MorphButtonBackground {
                scheme: root.scheme
                ghost: root.actionStyle !== 1
                hovered: action.hovered
                pressed: action.down
                focused: action.visualFocus
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
            Layout.row: 0
            Layout.column: 3
            onClicked: root.fire("dismiss", undefined)
        }
    }
}
