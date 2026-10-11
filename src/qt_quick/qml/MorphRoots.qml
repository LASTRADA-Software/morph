// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Where a backend's roots are shown: one column, top to bottom, filling the host item.
Item {
    id: root
    readonly property Item content: column

    anchors.fill: parent

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 0
    }
}
