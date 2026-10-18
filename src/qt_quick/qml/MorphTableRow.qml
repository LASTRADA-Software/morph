// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// One row of a table: its cells placed by column, so a hidden cell keeps its column and the cells after it stay
// under their own headers. A click selects it, Ctrl+click toggles it, a double click or Enter activates it. C++
// calls `place` again whenever a cell's implicit size changes.
MorphWidget {
    id: root

    // The MorphTable it belongs to.
    property var table: null
    property bool selected: false
    // The row's key as text (`i:<int64>` or `s:<text>`).
    property string rowKey: ""
    readonly property var widths: table ? table.widths : []
    readonly property Item content: cells

    function place() {
        let x = 0
        let tallest = 0
        for (let i = 0; i < cells.children.length; ++i) {
            const cell = cells.children[i]
            const w = i < root.widths.length ? root.widths[i] : cell.implicitWidth
            cell.x = x
            cell.width = w
            x += w
            tallest = Math.max(tallest, cell.implicitHeight)
        }
        cells.implicitWidth = x
        cells.implicitHeight = tallest
        for (let i = 0; i < cells.children.length; ++i)
            cells.children[i].height = tallest
    }

    onWidthsChanged: place()
    Layout.fillWidth: true
    implicitWidth: cells.implicitWidth
    // A dense row: the control height less 8 px, or the cells' height when they need more.
    implicitHeight: Math.max(cells.implicitHeight, Theme.controlHeight - 8)
    Accessible.role: Accessible.Row
    Accessible.selected: selected

    Keys.onReturnPressed: root.fire("activate", undefined)
    Keys.onEnterPressed: root.fire("activate", undefined)

    Rectangle {
        anchors.fill: parent
        color: root.selected ? root.scheme.selection : "transparent"
    }
    // The hairline between rows.
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: root.scheme.border
        opacity: 0.6
    }

    Item {
        id: cells
        anchors.fill: parent
        onChildrenChanged: root.place()
    }

    TapHandler {
        id: tap
        onTapped: {
            if (tap.tapCount >= 2)
                root.fire("activate", undefined)
            else
                root.fire("tap", (tap.point.modifiers & Qt.ControlModifier) !== 0)
        }
    }
}
