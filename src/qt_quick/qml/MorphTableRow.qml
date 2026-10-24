// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// One row of a table: its cells placed by column, so a hidden cell keeps its column and the cells after it stay
// under their own headers, over its background part. A click selects it, Ctrl+click toggles it, a double click or
// Enter activates it. C++ calls `place` again whenever a cell's implicit size changes.
MorphWidget {
    id: root

    // The MorphTable it belongs to.
    property var table: null
    property bool selected: false
    // The row's key as text (`i:<int64>` or `s:<text>`).
    property string rowKey: ""
    readonly property var widths: table ? table.widths : []
    readonly property Item content: cells
    // What draws under the cells: MorphTableRowBackground, or the `tableRow` restyle. C++ sets it when it makes the
    // row, and the row hands its state to the instance.
    property Component background: null
    readonly property bool hovered: rowHover.hovered
    readonly property bool current: root.holdsFocus(Window.activeFocusItem)
    // Whether the row is at an odd position in its table; C++ writes it as rows come, go and move.
    property bool alternate: false
    // The row's tone: -1 for none, else a tone index (Neutral, Info, Ok, Warn, Err).
    property int tone: -1
    readonly property Item backgroundItem: backgroundLoader.item as Item

    function holdsFocus(focused: Item): bool {
        for (let at = focused; at; at = at.parent) {
            if (at === root)
                return true
        }
        return false
    }

    function place() {
        // Each cell sits inset by the same step as the header label's own leftPadding/rightPadding
        // (MorphTable.qml), so cell text lines up under its header instead of touching the column's edge.
        const inset = Theme.spaceOf(2)
        let x = 0
        let tallest = 0
        for (let i = 0; i < cells.children.length; ++i) {
            const cell = cells.children[i]
            const w = i < root.widths.length ? root.widths[i] : cell.implicitWidth
            cell.x = x + inset
            cell.width = Math.max(0, w - 2 * inset)
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
    // The cells' height, or the background's when it asks for more: the stock one is a dense row.
    implicitHeight: Math.max(cells.implicitHeight, root.backgroundItem ? root.backgroundItem.implicitHeight : 0)
    Accessible.role: Accessible.Row
    Accessible.selected: selected

    // A chord the table declares comes before the row's activation.
    Keys.onReturnPressed: event => {
        if (!(root.table && root.table.takeOwnChord(event)))
            root.fire("activate", undefined)
    }
    Keys.onEnterPressed: event => {
        if (!(root.table && root.table.takeOwnChord(event)))
            root.fire("activate", undefined)
    }

    Loader {
        id: backgroundLoader
        anchors.fill: parent
        sourceComponent: root.background
    }
    Binding {
        target: backgroundLoader.item
        property: "selected"
        value: root.selected
    }
    Binding {
        target: backgroundLoader.item
        property: "hovered"
        value: root.hovered
    }
    Binding {
        target: backgroundLoader.item
        property: "current"
        value: root.current
    }
    Binding {
        target: backgroundLoader.item
        property: "alternate"
        value: root.alternate
    }
    Binding {
        target: backgroundLoader.item
        property: "tone"
        value: root.tone
    }
    Binding {
        target: backgroundLoader.item
        property: "scheme"
        value: root.scheme
    }

    HoverHandler {
        id: rowHover
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
