// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick

// A grid: children row-major in its tracks, `gap` (a spacing step) apart; a child's span widens it. Without `tracks`
// it has `columns` equal stretch tracks, as wide by nature as `columns` times the widest cell. `place` lays every
// grid out itself, so both frontends share one rule.
MorphWidget {
    id: root

    property int columns: 1
    property int gap: 0
    // Where each cell's content sits in its row, a `ui::Align` index, which C++ also writes on each child as its
    // `crossAlign`.
    property int align: 0
    // The column tracks, each {kind, amount, align}: a `ui::Sizing` kind (0 content, 1 fixed, 2 stretch) and amount,
    // and a `ui::Align` index across the column. Empty gives `columns` equal stretch tracks of weight 1.
    property var tracks: []
    readonly property bool tracked: tracks.length > 0
    readonly property var placedTracks: {
        if (tracked)
            return tracks
        const equal = []
        for (let i = 0; i < Math.max(1, columns); ++i)
            equal.push({"kind": 2, "amount": 1, "align": 0})
        return equal
    }
    readonly property Item content: free

    implicitWidth: free.implicitWidth
    implicitHeight: free.implicitHeight

    onWidthChanged: place()
    onPlacedTracksChanged: place()
    onGapChanged: place()

    // The spacing scale and the unit sizes feed every extent `place` computes.
    Connections {
        target: Theme
        function onUnitWidthChanged() {
            root.place()
        }
        function onUnitHeightChanged() {
            root.place()
        }
        function onSpaceChanged() {
            root.place()
        }
    }

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
    // width at the track's start, centre or end, a text draws its text there, and any other cell fills the track, up
    // to its own maximum width. Without tracks the natural width is the terminal's: the widest cell, a span divided
    // out, times the columns, so a content-sized grid never squeezes its own cells. Down its row a cell sits by its own `alignSelf`, else the grid's `align`. C++ calls it again whenever a cell's
    // implicit size, visibility or span changes.
    function place() {
        const tracks = root.placedTracks
        const count = tracks.length
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
            const track = tracks[t]
            const w = track.kind === 1 ? track.amount * Theme.unitWidth : track.kind === 0 ? naturals[t] : 0
            widths.push(w)
            used += w
            natural += track.kind === 2 ? naturals[t] : w
            if (track.kind === 2)
                weights += Math.max(1, track.amount)
        }
        const left = Math.max(0, root.width - used)
        for (let t = 0; t < count; ++t) {
            const track = tracks[t]
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
            const across = tracks[placed.column].align
            let width = room
            if (widget && widget.fillsAcross === false)
                width = Math.min(root.naturalWidth(cell), room)
            else if (widget && widget.widthKind === 1)
                width = Math.min(root.naturalWidth(cell), room)
            else if (widget && widget.maxWidthUnits >= 0)
                width = Math.min(room, widget.maxWidthUnits * Theme.unitWidth)
            const spare = room - width
            cell.x = xs[placed.column] + (across === 3 ? spare : across === 2 ? Math.floor(spare / 2) : 0)
            cell.width = width
            if (root.tracked && cell.textAlign !== undefined && !cell.ownAlign)
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
        if (!root.tracked) {
            let unit = 0
            for (const placed of cells) {
                const covered = space * (placed.span - 1)
                unit = Math.max(unit, Math.ceil(Math.max(0, root.naturalWidth(placed.item) - covered) / placed.span))
            }
            natural = unit * count + space * (count - 1)
        }
        free.implicitWidth = Math.max(0, natural)
        free.implicitHeight = heights.length > 0 ? y - space : 0
    }

    Item {
        id: free
        anchors.fill: parent
        onChildrenChanged: root.place()
    }
}
