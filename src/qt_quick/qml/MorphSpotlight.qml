// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A guided tour's spotlight: four scrim rectangles dim the window around the target's box, a ring in the focus colour
// surrounds it, and the card (`content`) sits on the side of the target with the most room, inside the window. The
// layer is an item of the window's content item above everything else, not a popup: a popup's item takes every press
// inside it, and a press inside the cut-out must reach the target. The scrim and the card take the presses on them.
MorphWidget {
    id: root

    property bool open: false
    // The target's box in window coordinates, as C++ tracks it; `ringed` false centres the card with no cut-out.
    property rect cutout: Qt.rect(0, 0, 0, 0)
    property bool ringed: false
    readonly property real ringPad: Theme.spaceOf(1)
    readonly property Item content: column
    readonly property Item card: card
    readonly property Item ring: ring

    focusTarget: column
    implicitWidth: 0
    implicitHeight: 0
    // Only a zero-sized placeholder: the layer below takes no room in the container's layout.
    Layout.preferredWidth: -1
    Layout.fillWidth: false
    Layout.preferredHeight: -1
    Layout.fillHeight: false

    Item {
        id: layer
        parent: root.Window.window ? root.Window.window.contentItem : root
        anchors.fill: parent
        z: 10000
        visible: root.open

        readonly property rect hole: root.ringed
            ? Qt.rect(root.cutout.x - root.ringPad, root.cutout.y - root.ringPad,
                      root.cutout.width + 2 * root.ringPad, root.cutout.height + 2 * root.ringPad)
            : Qt.rect(width / 2, height / 2, 0, 0)

        component Scrim: Rectangle {
            color: Theme.scrim
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                hoverEnabled: true
                onWheel: wheel => wheel.accepted = true
            }
        }

        Scrim {
            objectName: "spotlight_scrim_top"
            x: 0
            y: 0
            width: layer.width
            height: Math.max(0, layer.hole.y)
        }
        Scrim {
            objectName: "spotlight_scrim_bottom"
            x: 0
            y: layer.hole.y + layer.hole.height
            width: layer.width
            height: Math.max(0, layer.height - y)
        }
        Scrim {
            objectName: "spotlight_scrim_left"
            x: 0
            y: layer.hole.y
            width: Math.max(0, layer.hole.x)
            height: layer.hole.height
        }
        Scrim {
            objectName: "spotlight_scrim_right"
            x: layer.hole.x + layer.hole.width
            y: layer.hole.y
            width: Math.max(0, layer.width - x)
            height: layer.hole.height
        }

        Rectangle {
            id: ring
            objectName: "spotlight_ring"
            visible: root.ringed
            x: layer.hole.x
            y: layer.hole.y
            width: layer.hole.width
            height: layer.hole.height
            color: "transparent"
            border.color: Theme.focus
            border.width: 2
            radius: Theme.radiusControl
        }

        Rectangle {
            id: card
            objectName: "spotlight_card"
            readonly property real gap: Theme.spaceOf(2)
            readonly property real pad: Theme.spaceOf(4)
            // The room on each side of the hole, less the card: the side with the most is where the card goes.
            readonly property var rooms: [
                layer.height - (layer.hole.y + layer.hole.height) - gap - height,
                layer.hole.y - gap - height,
                layer.width - (layer.hole.x + layer.hole.width) - gap - width,
                layer.hole.x - gap - width
            ]
            readonly property int side: {
                if (!root.ringed)
                    return -1
                let best = 0
                for (let index = 1; index < 4; ++index) {
                    if (rooms[index] > rooms[best])
                        best = index
                }
                return best
            }

            function clamp(value: real, low: real, high: real): real {
                return Math.max(low, Math.min(value, high))
            }

            width: Math.min(column.implicitWidth + 2 * pad, Theme.unitWidth * 40, layer.width - 2 * gap)
            height: column.implicitHeight + 2 * pad
            x: clamp(side === -1 ? (layer.width - width) / 2
                     : side === 2 ? layer.hole.x + layer.hole.width + gap
                     : side === 3 ? layer.hole.x - gap - width
                     : layer.hole.x, gap, layer.width - width - gap)
            y: clamp(side === -1 ? (layer.height - height) / 2
                     : side === 0 ? layer.hole.y + layer.hole.height + gap
                     : side === 1 ? layer.hole.y - gap - height
                     : layer.hole.y, gap, layer.height - height - gap)
            color: Theme.cardScheme.surface
            border.color: Theme.border
            radius: Theme.radiusCard

            // Under the content, so a press on the card's padding reaches nothing behind it.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
            }

            ColumnLayout {
                id: column
                x: card.pad
                y: card.pad
                width: card.width - 2 * card.pad
                spacing: Theme.spaceOf(2)
            }
        }
    }
}
