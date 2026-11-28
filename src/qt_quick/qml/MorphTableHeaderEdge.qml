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

    // The last translation of the drag under way: the handler resets its own before it reports the drag ended.
    property real dragged: 0

    visible: edge.table !== null && edge.table.resizable
    objectName: "edge" + edge.column
    // Above the header's filter button, which sits at the same edge and would otherwise take the strip's input.
    z: 1
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
        onActiveTranslationChanged: {
            if (drag.active)
                edge.dragged = drag.activeTranslation.x
        }
        onActiveChanged: {
            if (drag.active) {
                edge.dragged = 0
            } else if (edge.parent) {
                edge.table.fire("columnResize", {"column": edge.column,
                                                 "width": Math.max(1, edge.parent.width + edge.dragged)})
            }
        }
    }
    TapHandler {
        onDoubleTapped: edge.table.fire("columnResize", {"column": edge.column, "width": null})
    }
}
