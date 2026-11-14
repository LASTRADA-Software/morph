// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick

// The marker of a menu that opens: a chevron pointing down (`down`), or an up and a down chevron one above the other
// (`updown`), each drawn as two antialiased bars that meet at its tip, in a colour. Qt Shapes is not used, so no
// further Qt module reaches the WebAssembly build.
Item {
    id: root

    // "down" or "updown".
    property string kind: "down"
    property color color: "black"
    property real thickness: 1.5

    readonly property bool both: root.kind === "updown"
    // How far one chevron's tip stands from the line its arms start on, and the length of each arm.
    readonly property real depth: Math.max(2, Math.round(0.28 * Theme.bodyPixels))
    readonly property real arm: root.depth * Math.SQRT2
    readonly property real chevronWidth: 2 * root.depth + root.thickness
    readonly property real gap: Math.max(1, Math.round(0.15 * Theme.bodyPixels))

    implicitWidth: Math.ceil(root.chevronWidth)
    implicitHeight: Math.ceil(root.both ? 2 * root.depth + root.gap + root.thickness : root.depth + root.thickness)
    width: implicitWidth
    height: implicitHeight
    Accessible.ignored: true

    // A chevron whose tip points down stands at the bottom of the box; one that points up stands at the top.
    Repeater {
        model: root.both ? [{"up": true}, {"up": false}] : [{"up": false}]

        Item {
            id: chevron
            required property var modelData
            readonly property real tipY: modelData.up ? root.thickness / 2 : root.height - root.thickness / 2
            readonly property real armY: modelData.up ? root.thickness / 2 + root.depth
                                                      : root.height - root.thickness / 2 - root.depth

            Repeater {
                model: [-1, 1]

                Rectangle {
                    required property int modelData
                    width: root.arm
                    height: root.thickness
                    radius: height / 2
                    antialiasing: true
                    color: root.color
                    // Each bar is centred half way between the tip and the arm's far end, and leans toward the tip.
                    x: root.width / 2 + modelData * root.depth / 2 - width / 2
                    y: (chevron.tipY + chevron.armY) / 2 - height / 2
                    rotation: chevron.modelData.up ? modelData * 45 : -modelData * 45
                }
            }
        }
    }
}
