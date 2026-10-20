// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// An overlay's footer: what C++ puts in it (the overlay's actions) in one row against the right edge. Empty, it
// takes no room.
MorphWidget {
    id: root

    readonly property Item content: row
    // Whether anything is in the footer.
    readonly property bool holding: row.children.length > 0

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    RowLayout {
        id: row
        anchors.right: parent.right
        height: root.height
        spacing: Theme.spaceOf(2)
    }
}
