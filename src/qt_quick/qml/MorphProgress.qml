// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A progress bar with a label; a negative value is indeterminate. The fill is the scheme's accent on its neutral
// track, drawn here rather than by the style, which paints its own colours.
MorphWidget {
    id: root

    property real value: -1
    property string label: ""

    implicitWidth: Math.max(bar.implicitWidth, caption.implicitWidth)
    implicitHeight: bar.implicitHeight + (caption.visible ? caption.implicitHeight : 0)

    Column {
        anchors.fill: parent

        ProgressBar {
            id: bar
            width: parent.width
            indeterminate: root.value < 0
            value: root.value < 0 ? 0 : root.value

            background: Rectangle {
                objectName: "track"
                implicitWidth: 20 * Theme.unitWidth
                implicitHeight: Theme.spaceOf(1)
                radius: height / 2
                color: root.scheme.neutral
            }

            contentItem: Item {
                implicitWidth: 20 * Theme.unitWidth
                implicitHeight: Theme.spaceOf(1)
                clip: true

                // Determinate: as wide as the value's share of the track. Indeterminate: a third of the track that
                // slides across it on a loop, which replaces the style's own animation this item takes the place of.
                Rectangle {
                    id: fill
                    objectName: "fill"
                    height: parent.height
                    width: bar.indeterminate ? parent.width / 3 : bar.visualPosition * parent.width
                    x: 0
                    radius: height / 2
                    color: root.scheme.accent

                    NumberAnimation on x {
                        running: bar.indeterminate && root.visible
                        loops: Animation.Infinite
                        from: -fill.width
                        to: bar.availableWidth
                        duration: 1200
                        onRunningChanged: {
                            if (!running)
                                fill.x = 0
                        }
                    }
                }
            }
        }

        Label {
            font: Theme.bodyFont
            id: caption
            visible: root.label !== ""
            text: root.label
        }
    }
}
