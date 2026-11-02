// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick

// A table's header row background, under its column labels. It is the `tableHeader` part a restyle replaces; the
// table writes its scheme here and keeps the header's placement. Its implicit size follows its content.
Item {
    id: root

    // The colour roles of the table the header is in.
    property MorphScheme scheme: Theme.themeScheme

    Rectangle {
        anchors.fill: parent
        color: root.scheme.tableHeader
    }
    // The rule under the header, in the theme's `color.table.headerRule`, over the band's last pixel row.
    Rectangle {
        objectName: "headerRule"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: root.scheme.tableHeaderRule
    }
}
