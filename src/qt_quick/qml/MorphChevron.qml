// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick

// A chevron pointing left (`back`) or right (`forward`), drawn as two antialiased bars that meet at its tip, in a
// colour. Qt Shapes is not used, so no further Qt module reaches the WebAssembly build. Its height is what the caller
// gives; its width follows from the arms' angle.
Item {
    id: root

    // "back" points left, "forward" right.
    property string direction: "back"
    property color color: "black"
    // How thick each bar is.
    property real thickness: 1.5

    readonly property bool forward: root.direction === "forward"
    // The length of each bar: two of them at 45 degrees span the height less one bar's thickness.
    readonly property real arm: Math.max(0, (root.height - root.thickness) * Math.SQRT1_2)
    readonly property real tipY: root.height / 2

    implicitWidth: root.arm * Math.SQRT1_2 + root.thickness
    implicitHeight: Math.round(0.6 * Theme.bodyPixels)
    width: implicitWidth
    height: implicitHeight
    Accessible.ignored: true

    Repeater {
        model: [-1, 1]

        Rectangle {
            required property int modelData
            width: root.arm
            height: root.thickness
            radius: height / 2
            antialiasing: true
            color: root.color
            // The bars meet at the tip, on the side the chevron points to, and fan away from it.
            x: root.forward ? root.width - root.thickness / 2 - width : root.thickness / 2
            y: root.tipY - height / 2
            transformOrigin: root.forward ? Item.Right : Item.Left
            rotation: root.forward ? -modelData * 45 : modelData * 45
        }
    }
}
