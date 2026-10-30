// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A passive, transient message near a corner of the window: never modal, never dims, never takes focus, and
// dismisses only on its own timer. `show` always restarts the timer and updates the message, whether or not the
// toast is already on-screen.
MorphWidget {
    id: root

    implicitWidth: 0
    implicitHeight: 0

    function show(text: string, tone: int): void {
        popup.text = text
        popup.tone = tone
        popup.open()
        dismissTimer.restart()
    }

    Timer {
        id: dismissTimer
        interval: 4000
        onTriggered: popup.close()
    }

    Popup {
        id: popup
        property string text: ""
        property int tone: 0

        x: (popup.Overlay.overlay?.width ?? 0) - width - Theme.unitWidth
        y: (popup.Overlay.overlay?.height ?? 0) - height - Theme.unitHeight
        modal: false
        dim: false
        focus: false
        closePolicy: Popup.NoAutoClose

        background: Rectangle {
            color: Qt.tint(root.scheme.surface, root.toneBgOf(popup.tone))
            border.color: root.toneBorderOf(popup.tone)
            radius: Theme.radiusCard
        }

        contentItem: RowLayout {
            spacing: Theme.spaceOf(2)

            Image {
                objectName: "toneIcon"
                readonly property string name: ["", "tone.info", "tone.ok", "tone.warn", "tone.err"][popup.tone] ?? ""
                visible: source.toString() !== ""
                source: Theme.iconSource(name)
                sourceSize.height: Theme.bodyPixels
                Layout.preferredHeight: Theme.bodyPixels
                Layout.preferredWidth: visible ? Theme.bodyPixels : 0
            }

            Label {
                textFormat: Text.PlainText
                font: Theme.bodyFont
                text: popup.text
                wrapMode: Text.Wrap
            }
        }
    }
}
