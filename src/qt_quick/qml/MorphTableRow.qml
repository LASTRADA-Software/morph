// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One row of a table: its cells placed by column, so a hidden cell keeps its column and the cells after it stay
// under their own headers, over its background part. A click selects it alone, Ctrl+click toggles it, Shift+click
// selects the range from the anchor, and each makes it the table's current row; a double click or Enter activates
// it. C++ calls `place` again whenever a cell's implicit size changes. A heading row has one cell across every column
// and the select column, takes no click or key, and is never the table's current row.
//
// A cell that spans (`spanPlan`, written by C++) covers the rows below its own. It stays a cell of this row, left out
// of the row's height, and is placed down the `spanBlocks` height the table gives it; the row is drawn above the rows
// it covers (C++ sets `z`), and under the cell sits a `spanBackdrop` in the column: an opaque surface with this row's
// background, the look of the row over the whole block, and a tap handler of its own, since the block lies outside
// the row's bounds, which the row's handlers would not reach. Hover is the row's own: its `containmentMask` takes in
// the blocks. A row ignores a tap in a spanning or covered column: the backdrop took it.
//
// In a table whose columns the mount lays out (`table.arranged`), each cell is placed in the column C++ gave it
// (`measuredIndex`), at the window's offset and width; a pinned column's cell is held at the leading edge, over the
// rest. A cell whose column has not loaded (`cellLoads`) keeps its content, bound but invisible, under a mark: a
// rounded bar 3 em wide for one not loaded, an error mark of the same width for one that failed. A cell's accent
// (`cellAccents`) tints its slot with the tone's fill under the cell, so the row's selection and focus marks still
// show; a tooltip draws the "more here" mark, shows on hover after the style's delay and at once while the cell has
// the keyboard focus, and is the accessible description of the cell's slot; a value that could not be read draws the error mark
// in place of the cell, with the accent's tooltip. The row's
// `measuredHeight` is its tallest placed cell's, unless every column keeps one line; the table sets its height.
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
    // The row's view row, in a table over a source; C++ writes it. A recycled slot measures its cells again for the
    // row it now shows, and is striped by the view row's parity, not by its place among the children.
    property int viewRow: -1
    onViewRowChanged: {
        if (root.viewRow >= 0) {
            root.alternate = root.viewRow % 2 === 1
            root.schedulePlace()
        }
    }
    // Places the cells once, from the event loop: C++ asks for it as each cell of a pass is placed or pooled.
    function schedulePlace() {
        Qt.callLater(root.place)
    }
    // The height the row's cells ask for, in a table over a source; the table keeps the largest while it is realized.
    property real measuredHeight: 0
    onMeasuredHeightChanged: {
        if (root.table)
            root.table.scheduleRows()
    }
    // The columns whose cells have not loaded, by column as text: 1 not loaded, 2 failed. C++ writes it.
    property var cellLoads: ({})
    // The marks drawn over those cells: [{x, width, load, align}].
    property var loadMarks: []
    function loadOf(column: int): int {
        return root.cellLoads[String(column)] ?? 0
    }
    onCellLoadsChanged: root.place()
    // The columns whose cells have an accent, by column as text: {tone, tooltip, invalid}. C++ writes it.
    property var cellAccents: ({})
    // The accents drawn in those cells' slots: [{x, width, column, tone, tooltip, pinned}].
    property var accentMarks: []
    function accentOf(column: int): var {
        return root.cellAccents[String(column)] ?? null
    }
    onCellAccentsChanged: root.place()
    // A tone's fill in the scheme, as the row background draws a toned row.
    function toneFill(index: int): color {
        switch (index) {
        case 1:
            return root.scheme.infoBg
        case 2:
            return root.scheme.successBg
        case 3:
            return root.scheme.warnBg
        case 4:
            return root.scheme.errorBg
        default:
            return Qt.alpha(root.scheme.muted, 0.15)
        }
    }
    // Over a source the table holds the focus and names the focused row; otherwise the row holds it.
    readonly property bool current: root.table && root.table.sourceRows
                                    ? root.table.activeFocus && root.viewRow === root.table.focusedRowIndex
                                    : root.holdsFocus(Window.activeFocusItem)
    onCurrentChanged: {
        if (current && root.table)
            root.table.currentRow = root
    }
    // Whether the row is at an odd position in its table; C++ writes it as rows come, go and move.
    property bool alternate: false
    // The row's tone: -1 for none, else a tone index (Neutral, Info, Ok, Warn, Err).
    property int tone: -1
    // Whether the row is a group heading; C++ sets it once, right after the row's key.
    property bool heading: false
    readonly property Item backgroundItem: backgroundLoader.item as Item
    // The cells that span, C++'s resolve of the row's spans: [{column, rows}], and the columns of this row that a
    // span above covers: [column]. The table gives each spanning column the block it covers: [{column, height}], and
    // this row the lines a spanning cell adds to it as the last row it covers.
    property var spanPlan: []
    property var coveredColumns: []
    property var spanBlocks: []
    property real spanExtra: 0
    // The height of the row's own cells and background, without what a spanning cell adds.
    readonly property real ownHeight: Math.max(cells.implicitHeight,
                                               root.backgroundItem ? root.backgroundItem.implicitHeight : 0)

    // Whether column `index` is one this row's spanning cells or a span above it cover.
    function spanned(index: int): bool {
        if (root.coveredColumns.indexOf(index) >= 0)
            return true
        for (const span of root.spanPlan) {
            if (span.column === index)
                return true
        }
        return false
    }

    // The left edge of column `index`, from the row's start.
    function columnLeft(index: int): real {
        let left = root.table ? root.table.lead : 0
        for (let i = 0; i < index && i < root.widths.length; ++i)
            left += root.widths[i]
        return left
    }

    // The height of the block the spanning cell of column `index` covers: the table's, else the row's own.
    function blockHeight(index: int): real {
        for (const block of root.spanBlocks) {
            if (block.column === index)
                return block.height
        }
        return root.height
    }

    // Whether a point in the row's coordinates lies in a spanning or covered column.
    function inSpannedColumn(x: real): bool {
        for (let i = 0; i < root.widths.length; ++i) {
            if (root.spanned(i) && x >= root.columnLeft(i) && x < root.columnLeft(i) + root.widths[i])
                return true
        }
        return false
    }

    function holdsFocus(focused: Item): bool {
        for (let at = focused; at; at = at.parent) {
            if (at === root)
                return true
        }
        return false
    }

    function isSpanning(index: int): bool {
        for (const span of root.spanPlan) {
            if (span.column === index)
                return true
        }
        return false
    }

    // Each cell sits inset by `table.cellPaddingX` and `table.cellPaddingY`, as the header label is (MorphTable.qml),
    // so its text lines up under its header, and a row is as tall as its tallest cell plus twice the vertical inset.
    // A cell fills its column's width, except a kind whose width is its label (`fillsAcross`
    // false: a badge, a check box, a button), which keeps its natural width at the column's start, centre or end.
    // A cell that draws text (`textAlign`) draws it at the column's start, centre or end unless it has an alignment of
    // its own (`ownAlign`), and an end column's digits are tabular so they line up across rows. In a column whose `wrap` is false it draws on one line, elided. Every cell keeps its own height, up to the row's, and sits at
    // the row's top, centre or bottom by its column's `valign`; `stretch` fills the row's height.
    //
    // Placing a cell can change the cell's implicit width (a nested table's follows its width), which reports to the
    // table and changes `widths`, which asks to place again while this call is still running. A call made meanwhile
    // only asks for another pass; the outer call makes it with `widths` read afresh, at most `kMaxPasses` in one call
    // stack. A row still unsettled after that places once more from the event loop, so a follower that never settles
    // cannot deepen the stack.
    readonly property int kMaxPasses: 3
    // Whether `place` is running, and whether it was called while it was. Private.
    property bool placing: false
    property bool placeAgain: false
    // Test-only: how many passes `place` has made.
    property int placePasses: 0
    // Test-only: how many calls arrived while `place` was running and were held for another pass.
    property int placeReentries: 0
    function place() {
        if (root.placing) {
            ++root.placeReentries
            root.placeAgain = true
            return
        }
        root.placing = true
        if (root.table)
            ++root.table.placing
        let passes = 0
        try {
            do {
                root.placeAgain = false
                ++root.placePasses
                root.placeOnce()
                ++passes
            } while (root.placeAgain && passes < root.kMaxPasses)
        } finally {
            root.placing = false
            if (root.table)
                --root.table.placing
        }
        if (root.placeAgain) {
            root.placeAgain = false
            Qt.callLater(root.place)
        }
    }
    function placeOnce() {
        const inset = Theme.tableCellPaddingX
        const insetY = Theme.tableCellPaddingY
        if (root.table && root.table.sourceRows && !root.heading) {
            root.placeWindowed()
            return
        }
        if (root.heading) {
            let across = root.table ? root.table.lead : 0
            for (const w of root.widths)
                across += w
            let tall = 0
            for (let i = 0; i < cells.children.length; ++i) {
                const cell = cells.children[i]
                cell.x = inset
                cell.width = Math.max(0, across - 2 * inset)
                tall = Math.max(tall, cell.implicitHeight)
            }
            cells.implicitWidth = across
            cells.implicitHeight = tall + 2 * insetY
            const headingHeight = Math.max(tall + 2 * insetY, root.backgroundItem ? root.backgroundItem.implicitHeight : 0)
            for (let i = 0; i < cells.children.length; ++i) {
                const cell = cells.children[i]
                cell.height = Math.min(cell.implicitHeight, tall)
                cell.y = Math.floor((headingHeight - cell.height) / 2)
            }
            return
        }
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
            if (cell.textAlign !== undefined && !cell.ownAlign)
                cell.textAlign = across === 3 ? Qt.AlignRight : across === 2 ? Qt.AlignHCenter : Qt.AlignLeft
            if (cell.singleLine !== undefined)
                cell.singleLine = i < root.columns.length && !root.columns[i].wrap
            if (cell.elideToWidth !== undefined)
                cell.elideToWidth = true
            if (cell.tabularFigures !== undefined)
                cell.tabularFigures = across === 3
            x += w
            if (!root.isSpanning(i))
                tallest = Math.max(tallest, cell.implicitHeight)
        }
        cells.implicitWidth = x
        cells.implicitHeight = tallest + 2 * insetY
        const rowHeight = Math.max(root.ownHeight, tallest + 2 * insetY) + root.spanExtra
        const room = rowHeight - 2 * insetY
        for (let i = 0; i < cells.children.length; ++i) {
            const cell = cells.children[i]
            const down = i < root.columns.length ? root.columns[i].valign : 2
            // A spanning cell is placed down the block it covers, and sits at the row's top edge.
            const spanning = root.isSpanning(i)
            const space = spanning ? root.blockHeight(i) - 2 * insetY : room
            cell.height = down === 0 ? space : Math.min(cell.implicitHeight, spanning ? space : tallest)
            const spare = space - cell.height
            cell.y = insetY + (down === 3 ? spare : down === 2 ? Math.floor(spare / 2) : 0)
        }
    }

    // Each placed cell in its column (`table.columnSlot`); a pooled cell (no column, hidden by C++) is left where it is.
    function placeWindowed() {
        const table = root.table
        const inset = Theme.tableCellPaddingX
        const insetY = Theme.tableCellPaddingY
        const measure = !table.allOneLine
        const placed = []
        const marks = []
        const accents = []
        let tallest = 0
        for (let i = 0; i < cells.children.length; ++i) {
            const cell = cells.children[i]
            const column = cell.measuredIndex
            const slot = cell === root.pinnedBackdrop || column === undefined ? null : table.columnSlot(column)
            if (!slot)
                continue
            const pinned = slot.pinned
            const left = table.lead + slot.offset + (pinned ? table.scrollX : 0)
            const room = Math.max(0, slot.width - 2 * inset)
            const spec = column < root.columns.length ? root.columns[column] : null
            const across = spec ? spec.align : 1
            if ((cell as MorphWidget)?.fillsAcross === false) {
                cell.width = Math.min(cell.implicitWidth, room)
                const spare = room - cell.width
                cell.x = left + inset + (across === 3 ? spare : across === 2 ? Math.floor(spare / 2) : 0)
            } else {
                cell.width = room
                cell.x = left + inset
            }
            cell.z = pinned ? 1 : 0
            const accent = root.accentOf(column)
            const loaded = root.loadOf(column)
            // A value that could not be read draws the error mark in place of its text, as a failed load does.
            const load = loaded !== 0 ? loaded : accent && accent.invalid ? 2 : 0
            cell.opacity = load === 0 ? 1 : 0
            if (load !== 0)
                marks.push({"x": left + inset, "width": room, "load": load, "align": across, "pinned": pinned,
                            "tip": loaded === 0 && accent ? accent.tooltip : ""})
            if (accent && (accent.tone >= 0 || accent.tooltip !== ""))
                accents.push({"x": left, "width": slot.width, "column": column, "tone": accent.tone,
                              "tooltip": accent.tooltip, "pinned": pinned})
            if (cell.textAlign !== undefined && !cell.ownAlign)
                cell.textAlign = across === 3 ? Qt.AlignRight : across === 2 ? Qt.AlignHCenter : Qt.AlignLeft
            if (cell.singleLine !== undefined)
                cell.singleLine = !(spec && spec.wrap)
            if (cell.elideToWidth !== undefined)
                cell.elideToWidth = true
            if (cell.tabularFigures !== undefined)
                cell.tabularFigures = across === 3
            if (measure && cell.visible) {
                ++table.rowMeasures
                tallest = Math.max(tallest, cell.implicitHeight)
            }
            placed.push(cell)
        }
        const own = measure ? tallest + 2 * insetY : table.oneLineRowHeight
        root.measuredHeight = Math.max(own, root.backgroundItem ? root.backgroundItem.implicitHeight : 0)
        cells.implicitWidth = table.columnsWidth
        cells.implicitHeight = root.measuredHeight
        const space = Math.max(0, (root.height > 0 ? root.height : root.measuredHeight) - 2 * insetY)
        for (const cell of placed) {
            const down = cell.measuredIndex < root.columns.length ? root.columns[cell.measuredIndex].valign : 2
            cell.height = down === 0 ? space : Math.min(cell.implicitHeight, space)
            const spare = space - cell.height
            cell.y = insetY + (down === 3 ? spare : down === 2 ? Math.floor(spare / 2) : 0)
        }
        if (JSON.stringify(marks) !== JSON.stringify(root.loadMarks))
            root.loadMarks = marks
        if (JSON.stringify(accents) !== JSON.stringify(root.accentMarks))
            root.accentMarks = accents
        root.placeBackdrop()
    }

    // The opaque surface under the pinned cells, which the scrolled cells pass beneath: the surface, then the row's
    // own background cut to the pinned columns, so the row's state shows there as on the rest of the row. It is made
    // in `cells` (the cells' sibling, so it can lie between the scrolled cells and the pinned ones) only for a table
    // over a source with pinned columns, so a row of any other table holds nothing in `cells` but its cells.
    property Item pinnedBackdrop: null
    Component {
        id: pinnedBackdropComponent
        Item {
            id: backdrop
            objectName: "pinnedBackdrop"
            z: 0.5
            clip: true
            Rectangle {
                anchors.fill: parent
                color: root.scheme.surface
            }
            Loader {
                id: backdropLoader
                x: -backdrop.x
                width: root.width
                height: backdrop.height
                sourceComponent: root.background
            }
            Binding {
                target: backdropLoader.item
                property: "selected"
                value: root.selected
            }
            Binding {
                target: backdropLoader.item
                property: "hovered"
                value: root.hovered
            }
            Binding {
                target: backdropLoader.item
                property: "current"
                value: root.current
            }
            Binding {
                target: backdropLoader.item
                property: "alternate"
                value: root.alternate
            }
            Binding {
                target: backdropLoader.item
                property: "tone"
                value: root.tone
            }
            Binding {
                target: backdropLoader.item
                property: "scheme"
                value: root.scheme
            }
            Binding {
                target: backdropLoader.item
                property: "selectColumn"
                value: root.table ? root.table.selectColumn : false
                when: backdropLoader.item !== null && ("selectColumn" in backdropLoader.item)
            }
        }
    }
    function placeBackdrop() {
        const table = root.table
        const shown = table && table.arranged && table.pinnedShown > 0 && table.pinnedEdge > 0
        if (shown && !root.pinnedBackdrop)
            root.pinnedBackdrop = pinnedBackdropComponent.createObject(cells)
        if (!root.pinnedBackdrop)
            return
        root.pinnedBackdrop.visible = shown
        if (shown) {
            root.pinnedBackdrop.x = table.lead + table.scrollX
            root.pinnedBackdrop.width = table.pinnedEdge
            root.pinnedBackdrop.height = root.height
        }
    }

    // The pinned cells follow the sideways scroll, so they stay at the leading edge; nothing else moves.
    function placePinned() {
        const table = root.table
        if (!table || !table.arranged || table.pinnedShown === 0)
            return
        for (let i = 0; i < cells.children.length; ++i) {
            const cell = cells.children[i]
            const place = cell.measuredIndex >= 0 ? table.windowPlaces[cell.measuredIndex] : undefined
            if (place === undefined || place >= table.pinnedShown)
                continue
            const entry = table.windowColumns[place]
            const room = Math.max(0, entry.width - 2 * Theme.tableCellPaddingX)
            cell.x = table.lead + entry.offset + table.scrollX + Theme.tableCellPaddingX + (room - cell.width) *
                     (cell.fillsAcross === false ? (root.columns[cell.measuredIndex].align === 3 ? 1
                                                    : root.columns[cell.measuredIndex].align === 2 ? 0.5 : 0) : 0)
        }
        root.placeBackdrop()
        if (root.loadMarks.some(mark => mark.pinned))
            root.place()
    }
    Connections {
        target: root.table
        enabled: root.table !== null && root.table.sourceRows
        function onScrollXChanged() {
            root.placePinned()
        }
        function onWindowColumnsChanged() {
            root.place()
        }
    }
    onHeightChanged: {
        if (root.table && root.table.sourceRows)
            root.place()
    }

    Connections {
        target: Theme
        function onTableCellPaddingXChanged() {
            root.place()
        }
        function onTableCellPaddingYChanged() {
            root.place()
        }
    }
    onWidthsChanged: place()
    onHeadingChanged: place()
    onSpanPlanChanged: place()
    onSpanBlocksChanged: place()
    onSpanExtraChanged: place()
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
    implicitHeight: root.ownHeight + root.spanExtra
    // The table works out the blocks and the lines a spanning cell adds from the rows' heights.
    onOwnHeightChanged: {
        if (root.table)
            root.table.scheduleSpans()
    }
    Accessible.role: Accessible.Row
    Accessible.selected: selected

    // A chord the table declares comes before the row's activation.
    Keys.onReturnPressed: event => {
        if (!root.heading && !(root.table && root.table.takeOwnChord(event)))
            root.fire("activate", undefined)
    }
    Keys.onEnterPressed: event => {
        if (!root.heading && !(root.table && root.table.takeOwnChord(event)))
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
    // Only a background that declares `heading`: a `tableRow` restyle that draws no headings still loads.
    Binding {
        target: backgroundLoader.item
        property: "heading"
        value: root.heading
        when: backgroundLoader.item !== null && ("heading" in backgroundLoader.item)
    }
    Binding {
        target: backgroundLoader.item
        property: "scheme"
        value: root.scheme
    }
    // Whether the table's select column already shows the selection, so the row takes no fill for it.
    Binding {
        target: backgroundLoader.item
        property: "selectColumn"
        value: root.table ? root.table.selectColumn : false
        when: backgroundLoader.item !== null && ("selectColumn" in backgroundLoader.item)
    }

    HoverHandler {
        id: rowHover
    }

    // Hover is delivered only to an item whose bounds hold the pointer, so the part of a spanning cell below the row
    // is part of the row for it: the row's own rectangle and the blocks of its spanning cells.
    containmentMask: QtObject {
        function contains(point: point): bool {
            if (point.x >= 0 && point.x < root.width && point.y >= 0 && point.y < root.height)
                return true
            for (const span of root.spanPlan) {
                const left = root.columnLeft(span.column)
                if (span.column < root.widths.length && point.x >= left && point.x < left + root.widths[span.column] &&
                        point.y >= 0 && point.y < root.blockHeight(span.column))
                    return true
            }
            return false
        }
    }

    Repeater {
        model: root.spanPlan
        delegate: Item {
            id: backdrop
            required property var modelData
            objectName: "spanBackdrop"
            x: root.columnLeft(modelData.column)
            y: 0
            width: modelData.column < root.widths.length ? root.widths[modelData.column] : 0
            height: root.blockHeight(modelData.column)
            clip: true
            Rectangle {
                anchors.fill: parent
                color: root.scheme.surface
            }
            // This row's background laid out as wide as the row and cut to the column, so the row's own marks (the
            // selection's bar at the leading edge) fall where they do on the row.
            Loader {
                id: spanLoader
                x: -backdrop.x
                width: root.width
                height: backdrop.height
                sourceComponent: root.background
            }
            Binding {
                target: spanLoader.item
                property: "selected"
                value: root.selected
            }
            Binding {
                target: spanLoader.item
                property: "hovered"
                value: root.hovered
            }
            Binding {
                target: spanLoader.item
                property: "current"
                value: root.current
            }
            Binding {
                target: spanLoader.item
                property: "alternate"
                value: root.alternate
            }
            Binding {
                target: spanLoader.item
                property: "tone"
                value: root.tone
            }
            Binding {
                target: spanLoader.item
                property: "scheme"
                value: root.scheme
            }
            Binding {
                target: spanLoader.item
                property: "selectColumn"
                value: root.table ? root.table.selectColumn : false
                when: spanLoader.item !== null && ("selectColumn" in spanLoader.item)
            }
            TapHandler {
                onTapped: root.tapped(point.modifiers, tapCount, point.position.x + backdrop.x,
                                      point.position.y, false)
            }
        }
    }

    // Each accented cell's slot, under the cells: the tone's tint, the "more here" mark and the tooltip.
    Repeater {
        model: root.accentMarks
        delegate: Item {
            id: accentMark
            required property var modelData
            readonly property bool focusedCell: root.current && root.table !== null
                                                && root.table.focusedColumnIndex === modelData.column
            objectName: "cellAccent" + modelData.column
            x: modelData.x
            z: modelData.pinned ? 0.75 : 0
            width: modelData.width
            height: root.height
            Rectangle {
                objectName: "cellTint"
                anchors.fill: parent
                visible: accentMark.modelData.tone >= 0
                color: Qt.alpha(root.toneFill(accentMark.modelData.tone), 0.5)
            }
            Rectangle {
                objectName: "moreMark"
                visible: accentMark.modelData.tooltip !== ""
                anchors.top: parent.top
                anchors.right: parent.right
                width: Math.round(Theme.bodyFont.pixelSize * 0.4)
                height: width
                color: root.scheme.accent
            }
            HoverHandler {
                id: accentHover
            }
            ToolTip.visible: accentMark.modelData.tooltip !== "" && (accentHover.hovered || accentMark.focusedCell)
            ToolTip.delay: accentMark.focusedCell ? 0 : Qt.styleHints.mousePressAndHoldInterval
            ToolTip.text: accentMark.modelData.tooltip
            Accessible.role: Accessible.Cell
            Accessible.description: accentMark.modelData.tooltip
        }
    }

    Item {
        id: cells
        anchors.fill: parent
        onChildrenChanged: root.place()
    }

    // A cell not loaded draws a rounded bar; one that failed, an error mark; each 3 em wide in its column's alignment.
    Repeater {
        model: root.loadMarks
        delegate: Item {
            id: mark
            required property var modelData
            readonly property real markWidth: Math.min(3 * Theme.bodyFont.pixelSize, modelData.width)
            objectName: modelData.load === 2 ? "failedMark" : "unloadedMark"
            x: modelData.x + (modelData.align === 3 ? modelData.width - markWidth
                                                    : modelData.align === 2 ? (modelData.width - markWidth) / 2 : 0)
            z: 2
            width: markWidth
            height: root.height
            Rectangle {
                visible: mark.modelData.load === 1
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: Math.round(Theme.bodyFont.pixelSize * 0.6)
                radius: height / 2
                color: root.scheme.muted
                opacity: 0.35
            }
            Text {
                id: failedText
                visible: mark.modelData.load === 2
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "!"
                font: Theme.strongFont
                color: root.scheme.error
                Accessible.role: Accessible.StaticText
                Accessible.name: mark.modelData.tip ? mark.modelData.tip : "Could not be loaded"
                Accessible.description: mark.modelData.tip ?? ""
                HoverHandler {
                    id: failedHover
                }
                ToolTip.visible: failedHover.hovered
                ToolTip.text: mark.modelData.tip ? mark.modelData.tip : "Could not be loaded"
            }
        }
    }

    // The row's select box: checked while the row is selected, and a click on it is a Ctrl-click on the row. It shows
    // the selection and never changes its own state, as the select-all box does.
    CheckBox {
        id: selectBox
        objectName: "selectBox"
        visible: root.table && !root.heading ? root.table.selectColumn : false
        x: Math.floor(((root.table ? root.table.lead : 0) - width) / 2)
        anchors.verticalCenter: parent.verticalCenter
        padding: 0
        indicator: MorphCheckIndicator {
            objectName: "boxIndicator"
            x: selectBox.leftPadding
            y: (selectBox.height - height) / 2
            scheme: root.scheme
            checked: selectBox.checked
            focused: selectBox.visualFocus
        }
        implicitWidth: indicator.implicitWidth + leftPadding + rightPadding
        implicitHeight: indicator.implicitHeight + topPadding + bottomPadding
        checked: root.selected
        nextCheckState: function() {
            return checkState
        }
        onClicked: root.fire("tap", {"ctrl": true})
        Accessible.name: root.accessibleName
    }

    // A click, as the row takes it: by the row's own handler, or by a spanning cell's backdrop, which passes its point
    // in the row's coordinates (a point past the row's bottom).
    function tapped(modifiers: int, tapCount: int, x: real, y: real, fromRow: bool) {
        if (root.heading)
            return
        // The backdrop takes a tap in a spanning or covered column; the row's own handler leaves it.
        if (fromRow && root.inSpannedColumn(x))
            return
        const held = modifiers & (Qt.ControlModifier | Qt.ShiftModifier)
        // Only plain clicks make a double click, so two quick Ctrl-clicks toggle a row twice.
        if (tapCount >= 2 && held === Qt.NoModifier) {
            root.fire("activate", undefined)
            return
        }
        // A click on the select box is the box's own.
        if (fromRow && selectBox.visible && selectBox.contains(selectBox.mapFromItem(root, Qt.point(x, y))))
            return
        // Over a source the table takes the focus, never the row, which a recycled slot would take away; a
        // Shift-click selects through the mount, from the table's anchor, and any other click makes this row the anchor.
        // Otherwise the row takes the focus unless a control in one of its cells took it with the press.
        if (root.table && root.table.sourceRows) {
            if (!root.holdsFocus(root.Window.activeFocusItem))
                root.table.forceActiveFocus(Qt.MouseFocusReason)
            if ((modifiers & Qt.ShiftModifier) !== 0 && (modifiers & Qt.ControlModifier) === 0 && root.table.multiple) {
                root.table.selectRangeTo(root.viewRow, false)
                return
            }
            root.table.anchorRowIndex = root.viewRow
        } else if (!root.holdsFocus(root.Window.activeFocusItem)) {
            root.forceActiveFocus(Qt.MouseFocusReason)
        }
        root.fire("tap", {
            "ctrl": (modifiers & Qt.ControlModifier) !== 0,
            "shift": (modifiers & Qt.ShiftModifier) !== 0
        })
    }

    TapHandler {
        id: tap
        enabled: !root.heading
        onTapped: root.tapped(tap.point.modifiers, tap.tapCount, tap.point.position.x, tap.point.position.y, true)
    }
}
