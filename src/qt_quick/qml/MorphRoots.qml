// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Where a backend's roots are shown: one column, top to bottom, filling the host item. A root whose height stretches
// takes the height the others leave; without one, the roots keep their own heights at the top, since a column given
// more height than it needs hands the spare to the gaps between its children.
Item {
    id: root
    readonly property Item content: column

    readonly property bool stretches: {
        for (const child of column.children) {
            if (child.visible && child.heightKind === 2)
                return true
        }
        return false
    }

    anchors.fill: parent

    // The window behind every surface.
    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.stretches ? root.height : implicitHeight
        spacing: 0
    }
}
