// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick

// A header's trailing edge, while its table's columns can be resized: dragging it reports the column's new width in
// pixels (`columnResize`), and a double click on it reports none, which gives the column its declared width again.
Item {
    id: edge

    // The column whose header this edge ends, an index into the table's columns.
    property int column: -1
    // The MorphTable the header belongs to; read through `var`, as its own members (`resizable`, `fire`) are not an
    // Item's.
    property var table: null

    visible: edge.table !== null && edge.table.resizable
    objectName: "edge" + edge.column
    anchors.right: parent ? parent.right : undefined
    anchors.top: parent ? parent.top : undefined
    anchors.bottom: parent ? parent.bottom : undefined
    width: 6

    HoverHandler {
        cursorShape: Qt.SplitHCursor
    }
    DragHandler {
        id: drag
        yAxis.enabled: false
        target: null
        onActiveChanged: {
            if (!active && edge.parent)
                edge.table.fire("columnResize", {"column": edge.column,
                                                 "width": Math.max(1, edge.parent.width + drag.activeTranslation.x)})
        }
    }
    TapHandler {
        onDoubleTapped: edge.table.fire("columnResize", {"column": edge.column, "width": null})
    }
}
