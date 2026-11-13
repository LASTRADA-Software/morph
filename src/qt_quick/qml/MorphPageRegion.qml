// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A rectangle of a sheet, in the sheet's unscaled pixels, holding its children as a column. C++ writes the rectangle
// (`ui::layoutPage`) and hears the height its content asks for when the region states none. The content is clipped to
// the rectangle; a region whose content does not fit draws a fade with an ellipsis over its last line.
MorphWidget {
    id: root

    readonly property Item content: column
    // The rectangle, written by C++; `x` and `y` are the item's own.
    property real rectWidth: 0
    property real rectHeight: 0
    // Whether the content does not fit.
    property bool overflow: false

    width: root.rectWidth
    height: root.rectHeight
    clip: true
    // An unnamed region is not an accessible group: a sheet would add empty groups to the tree.
    Accessible.ignored: root.accessibleName === ""

    // What the content asks for at this width, in pixels.
    readonly property real contentHeight: column.implicitHeight
    onContentHeightChanged: root.fire("content", root.contentHeight)

    ColumnLayout {
        id: column
        anchors.fill: parent
        spacing: 0
    }

    Rectangle {
        id: mark
        objectName: "overflowMark"
        visible: root.overflow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Theme.unitHeight * 2
        z: 50
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.alpha(root.scheme.surface, 0.0) }
            GradientStop { position: 1.0; color: root.scheme.surface }
        }
        Text {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: Theme.unitWidth
            text: "…"
            color: root.scheme.muted
            font.pixelSize: Theme.unitHeight
        }
    }
}
