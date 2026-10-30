// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A grid: children row-major in `columns` columns, `gap` (a spacing step) apart; a child's span widens it. With
// `tracks`, each column is sized and aligned by its track, and `place` lays the cells out instead of a GridLayout.
MorphWidget {
    id: root

    property int columns: 1
    property int gap: 0
    // Where each cell's content sits in its row, a `ui::Align` index, which C++ also writes on each child as its
    // `crossAlign`.
    property int align: 0
    // The column tracks, each {kind, amount, align}: a `ui::Sizing` kind (0 content, 1 fixed, 2 stretch) and amount,
    // and a `ui::Align` index across the column. Empty lays the cells out in a GridLayout of `columns` columns.
    property var tracks: []
    readonly property bool tracked: tracks.length > 0
    // C++ sets the tracks before it adopts any child, so every child lands in the content the grid lays out with.
    readonly property Item content: tracked ? free : layout

    implicitWidth: tracked ? free.implicitWidth : layout.implicitWidth
    implicitHeight: tracked ? free.implicitHeight : layout.implicitHeight

    onWidthChanged: place()
    onTracksChanged: place()

    // A cell's own extent along one dimension: its fixed amount, else its implicit extent, within its bounds.
    function extentOf(kind: int, amount: int, implicit: real, minimum: int, maximum: int, unit: real): real {
        let extent = kind === 1 ? amount * unit : implicit
        if (minimum >= 0)
            extent = Math.max(extent, minimum * unit)
        if (maximum >= 0)
            extent = Math.min(extent, maximum * unit)
        return extent
    }

    function naturalWidth(cell: Item): real {
        const widget = cell as MorphWidget
        if (!widget)
            return cell.implicitWidth
        return root.extentOf(widget.widthKind, widget.widthAmount, cell.implicitWidth, widget.minWidthUnits,
                             widget.maxWidthUnits, Theme.unitWidth)
    }

    function naturalHeight(cell: Item): real {
        const widget = cell as MorphWidget
        if (!widget)
            return cell.implicitHeight
        return root.extentOf(widget.heightKind, widget.heightAmount, cell.implicitHeight, widget.minHeightUnits,
                             widget.maxHeightUnits, Theme.unitHeight)
    }

    // Lays the shown cells out by the tracks, as the terminal does: row-major, a cell whose span does not fit the
    // rest of its row starting the next one. Fixed tracks take their amount, content tracks their widest one-track
    // cell, and stretch tracks share what is left by weight. Across its track a label-width cell keeps its natural
    // width at the track's start, centre or end, a text draws its text there, and any other cell fills the track. Down
    // its row a cell sits by its own `alignSelf`, else the grid's `align`. C++ calls it again whenever a cell's
    // implicit size, visibility or span changes.
    function place() {
        if (!root.tracked)
            return
        const count = root.tracks.length
        const space = Theme.spaceOf(root.gap)
        const cells = []
        let column = 0
        let row = 0
        for (let i = 0; i < free.children.length; ++i) {
            const cell = free.children[i]
            if (!cell.visible)
                continue
            const widget = cell as MorphWidget
            const span = Math.min(Math.max(widget ? widget.columnSpan : 1, 1), count)
            if (span > count - column) {
                ++row
                column = 0
            }
            cells.push({"item": cell, "column": column, "row": row, "span": span})
            column += span
        }

        const naturals = []
        for (let t = 0; t < count; ++t)
            naturals.push(0)
        for (const placed of cells) {
            if (placed.span === 1)
                naturals[placed.column] = Math.max(naturals[placed.column], root.naturalWidth(placed.item))
        }
        const widths = []
        let used = space * (count - 1)
        let natural = used
        let weights = 0
        for (let t = 0; t < count; ++t) {
            const track = root.tracks[t]
            const w = track.kind === 1 ? track.amount * Theme.unitWidth : track.kind === 0 ? naturals[t] : 0
            widths.push(w)
            used += w
            natural += track.kind === 2 ? naturals[t] : w
            if (track.kind === 2)
                weights += Math.max(1, track.amount)
        }
        const left = Math.max(0, root.width - used)
        for (let t = 0; t < count; ++t) {
            const track = root.tracks[t]
            if (track.kind === 2)
                widths[t] = left * Math.max(1, track.amount) / weights
        }
        const xs = []
        let x = 0
        for (let t = 0; t < count; ++t) {
            xs.push(x)
            x += widths[t] + space
        }

        const heights = []
        for (const placed of cells) {
            while (heights.length <= placed.row)
                heights.push(0)
            heights[placed.row] = Math.max(heights[placed.row], root.naturalHeight(placed.item))
        }
        const ys = []
        let y = 0
        for (let r = 0; r < heights.length; ++r) {
            ys.push(y)
            y += heights[r] + space
        }

        for (const placed of cells) {
            const cell = placed.item
            const widget = cell as MorphWidget
            let room = space * (placed.span - 1)
            for (let t = placed.column; t < placed.column + placed.span; ++t)
                room += widths[t]
            const across = root.tracks[placed.column].align
            let width = room
            if (widget && widget.fillsAcross === false)
                width = Math.min(root.naturalWidth(cell), room)
            else if (widget && widget.widthKind === 1)
                width = Math.min(root.naturalWidth(cell), room)
            const spare = room - width
            cell.x = xs[placed.column] + (across === 3 ? spare : across === 2 ? Math.floor(spare / 2) : 0)
            cell.width = width
            if (cell.textAlign !== undefined)
                cell.textAlign = across === 3 ? Qt.AlignRight : across === 2 ? Qt.AlignHCenter : Qt.AlignLeft

            const rowHeight = heights[placed.row]
            const down = widget && widget.alignSelf >= 0 ? widget.alignSelf : root.align
            const height = down === 0 && !(widget && widget.heightKind === 1) ? rowHeight
                                                                               : Math.min(root.naturalHeight(cell),
                                                                                          rowHeight)
            const below = rowHeight - height
            cell.y = ys[placed.row] + (down === 3 ? below : down === 2 ? Math.floor(below / 2) : 0)
            cell.height = height
        }
        free.implicitWidth = Math.max(0, natural)
        free.implicitHeight = heights.length > 0 ? y - space : 0
    }

    GridLayout {
        id: layout
        anchors.fill: parent
        visible: !root.tracked
        columns: Math.max(1, root.columns)
        rowSpacing: Theme.spaceOf(root.gap)
        columnSpacing: Theme.spaceOf(root.gap)
    }

    Item {
        id: free
        anchors.fill: parent
        visible: root.tracked
        onChildrenChanged: root.place()
    }
}
