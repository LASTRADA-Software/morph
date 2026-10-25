// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One row of a table: its cells placed by column, so a hidden cell keeps its column and the cells after it stay
// under their own headers, over its background part. A click selects it alone, Ctrl+click toggles it, Shift+click
// selects the range from the anchor, and each makes it the table's current row; a double click or Enter activates
// it. C++ calls `place` again whenever a cell's implicit size changes.
MorphWidget {
    id: root

    // The MorphTable it belongs to.
    property var table: null
    property bool selected: false
    // The row's key as text (`i:<int64>` or `s:<text>`).
    property string rowKey: ""
    // The key as a person reads it, which names the row until the table gives it a label.
    property string keyText: ""
    // The row's accessible name, and its select box's; empty names it by its key.
    property string rowLabel: ""
    defaultName: rowLabel !== "" ? rowLabel : keyText
    readonly property var widths: table ? table.widths : []
    readonly property var columns: table ? table.columns : []
    readonly property Item content: cells
    // What draws under the cells: MorphTableRowBackground, or the `tableRow` restyle. C++ sets it when it makes the
    // row, and the row hands its state to the instance.
    property Component background: null
    readonly property bool hovered: rowHover.hovered
    readonly property bool current: root.holdsFocus(Window.activeFocusItem)
    onCurrentChanged: {
        if (current && root.table)
            root.table.currentRow = root
    }
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

    // Each cell sits inset by the same step as the header label's own padding (MorphTable.qml), so its text lines
    // up under its header. A cell fills its column's width, except a kind whose width is its label (`fillsAcross`
    // false: a badge, a check box, a button), which keeps its natural width at the column's start, centre or end.
    // A cell that draws text (`textAlign`) draws it at the column's start, centre or end, and an end column's
    // digits are tabular so they line up across rows. Every cell keeps its own height, up to the row's, and sits at
    // the row's top, centre or bottom by its column's `valign`; `stretch` fills the row's height.
    function place() {
        const inset = Theme.spaceOf(2)
        let x = root.table ? root.table.lead : 0
        let tallest = 0
        for (let i = 0; i < cells.children.length; ++i) {
            const cell = cells.children[i]
            const w = i < root.widths.length ? root.widths[i] : cell.implicitWidth
            const room = Math.max(0, w - 2 * inset)
            const across = i < root.columns.length ? root.columns[i].align : 1
            if ((cell as MorphWidget)?.fillsAcross === false) {
                cell.width = Math.min(cell.implicitWidth, room)
                const spare = room - cell.width
                cell.x = x + inset + (across === 3 ? spare : across === 2 ? Math.floor(spare / 2) : 0)
            } else {
                cell.width = room
                cell.x = x + inset
            }
            if (cell.textAlign !== undefined)
                cell.textAlign = across === 3 ? Qt.AlignRight : across === 2 ? Qt.AlignHCenter : Qt.AlignLeft
            if (cell.tabularFigures !== undefined)
                cell.tabularFigures = across === 3
            x += w
            tallest = Math.max(tallest, cell.implicitHeight)
        }
        cells.implicitWidth = x
        cells.implicitHeight = tallest
        const rowHeight = Math.max(tallest, root.backgroundItem ? root.backgroundItem.implicitHeight : 0)
        for (let i = 0; i < cells.children.length; ++i) {
            const cell = cells.children[i]
            const down = i < root.columns.length ? root.columns[i].valign : 2
            cell.height = down === 0 ? rowHeight : Math.min(cell.implicitHeight, tallest)
            const spare = rowHeight - cell.height
            cell.y = down === 3 ? spare : down === 2 ? Math.floor(spare / 2) : 0
        }
    }

    onWidthsChanged: place()
    onColumnsChanged: place()
    onBackgroundItemChanged: place()
    // The row is as tall as its background asks when that is more than the cells, and they centre in that height.
    Connections {
        target: root.backgroundItem
        function onImplicitHeightChanged() {
            root.place()
        }
    }
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

    // The row's select box: checked while the row is selected, and a click on it is a Ctrl-click on the row. It shows
    // the selection and never changes its own state, as the select-all box does.
    CheckBox {
        id: selectBox
        objectName: "selectBox"
        visible: root.table ? root.table.selectColumn : false
        x: Math.floor(((root.table ? root.table.lead : 0) - width) / 2)
        anchors.verticalCenter: parent.verticalCenter
        checked: root.selected
        nextCheckState: function() {
            return checkState
        }
        onClicked: root.fire("tap", {"ctrl": true})
        Accessible.name: root.accessibleName
    }

    TapHandler {
        id: tap
        onTapped: {
            const held = tap.point.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)
            // Only plain clicks make a double click, so two quick Ctrl-clicks toggle a row twice.
            if (tap.tapCount >= 2 && held === Qt.NoModifier) {
                root.fire("activate", undefined)
                return
            }
            // A click on the select box is the box's own.
            if (selectBox.visible && selectBox.contains(selectBox.mapFromItem(root, tap.point.position)))
                return
            // The row takes the focus unless a control in one of its cells took it with the press.
            if (!root.holdsFocus(root.Window.activeFocusItem))
                root.forceActiveFocus(Qt.MouseFocusReason)
            root.fire("tap", {
                "ctrl": (tap.point.modifiers & Qt.ControlModifier) !== 0,
                "shift": (tap.point.modifiers & Qt.ShiftModifier) !== 0
            })
        }
    }
}
