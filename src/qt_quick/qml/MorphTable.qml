// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Rows of cells under column headers. Each column's width comes from its sizing: fixed units, the widest of its
// header and its shown cells for content, or for stretch its content plus a share of what is left; every row places
// its cells at those widths.
//
// A table over a `TableSource` (`sourceRows`) draws what the mount realizes. Each row sits at the top of its view row
// (`viewRow`), whatever its place among the children, and each cell in the column C++ gave it (`measuredIndex`); a
// cell with none is pooled and hidden. Once the mount lays the columns out (`arranged`), the header and the rows show
// the window's columns only (`windowColumns`, pixels), the first `pinnedShown` of them held at the leading edge over
// the rest, which scroll; the table reports its sideways scroll and viewport, and a cell's width to grow its column.
// A row is as tall as the tallest of its placed cells, and its height only grows while it is realized; a row that
// leaves forgets it, and a row not realized counts as the mean of those measured. A table whose columns all keep one
// line has one row height and measures no cell. When rows above the first visible one change height, the view moves
// by as much, so the rows in view stay where they were. Keyboard focus stays on the table, never on a row, which a
// recycled slot would take away: the keys go to the mount (`focusMove`), and the focused cell (`focusRow`,
// `focusColumn`) is kept in view and drawn as the current row.
//
// The status line (`statusShown`) is one line under the rows: the counts, the state and the message, and "Retry"
// while the state offers it. It takes no space while it is not shown.
MorphWidget {
    id: root

    // The columns: [{label, kind (0 content, 1 fixed, 2 stretch), amount, align, valign, sortable}], the places an
    // `Align`: 0 stretch, 1 start, 2 center, 3 end.
    property var columns: []
    property bool showHeader: true
    // The column whose header marks the sort, -1 for none, and whether it is descending; C++ writes both.
    property int sortColumn: -1
    property bool sortDescending: false
    // What follows a sortable header's label while it sorts, and the room every sortable header keeps for it, so the
    // columns do not move when the sort does.
    readonly property string ascendingMark: " ▲"
    readonly property string descendingMark: " ▼"
    // Whether the user can select several rows; C++ writes it with the selection mode.
    property bool multiple: false
    // The row keyboard focus is on, or was on last; null before any.
    property Item currentRow: null
    // Whether the selection is drawn as a leading column of check boxes; C++ sets it once, before any row.
    property bool selectColumn: false
    // The select-all box's accessible name.
    property string selectAllLabel: "Select all"
    // The width the select column takes before the first column: a touch target.
    readonly property real lead: root.selectColumn ? Theme.targetHeight : 0
    // How many of the shown rows are selected, for the select-all box.
    readonly property int selectedShown: {
        let count = 0
        for (const row of root.shownRows()) {
            if (row.selected)
                ++count
        }
        return count
    }
    // Set by C++ (TableImpl::setRealizedWindow) only above a virtualized table's threshold; totalRows stays -1
    // (its default, meaning "not virtualizing") otherwise, which is what keeps every binding below referencing
    // these three properties a no-op for a table that never crosses its threshold -- body has no spacers, and the
    // Flickable's own reporting never fires, since nothing ever asks "what rows exist beyond what's realized."
    property int realizedFirst: 0
    property int realizedCount: 0
    property int totalRows: -1
    readonly property bool virtualized: root.totalRows >= 0
    // A fixed estimate for scroll-position math -- how tall a row the Flickable hasn't measured yet is assumed to
    // be -- corrected once real, realized rows give an actual average (see averageRealizedRowHeight below). This
    // is the row-height-estimation spike the design left open: a uniform-row-height assumption would be simpler
    // and exact, but this codebase's own tables mix row heights (a multi-line cell, a sieve row); an estimate that
    // self-corrects is the more broadly applicable of the two options the design named, at the cost of an
    // imprecise scrollbar thumb position until enough real rows have been measured to average.
    property real estimatedRowHeight: Theme.unitHeight + 2 * Theme.tableCellPaddingY
    // The height each row had when it was last realized, by view index (a sparse array), the view indices that have
    // one, and the sum of their heights. A row not realized now keeps its measured height in the spacers, so the
    // content above and below the viewport changes only when a row is measured for the first time. The estimate is
    // the average of every measured row, which moves far less than the average of the rows realized right now.
    // `measuredRevision` changes whenever a height does, for the bindings that read the spacers.
    property var measured: []
    property var measuredIndices: []
    property real measuredSum: 0
    property int measuredRevision: 0
    // The row at the viewport's top and how far into it the viewport is, kept while the user scrolls and restored
    // after a measurement changes the content above the viewport, so the rows the user looks at stay where they are.
    property int anchorRow: 0
    property real anchorOffset: 0
    property bool restoring: false
    // Whether the rows come from a `TableSource`; C++ sets it before the first row.
    property bool sourceRows: false
    readonly property Item content: root.sourceRows ? rowsPlane : body
    readonly property Item rowsHolder: root.sourceRows ? rowsPlane : body
    // The column window, once the mount lays the columns out: [{column, offset, width}] in pixels, the pinned ones
    // first; the width of every visible column together; how many of the window are pinned. C++ writes them.
    property bool arranged: false
    property var windowColumns: []
    property real windowTotalWidth: 0
    property int pinnedShown: 0
    // For tests and assistive technology: the window's size, its first scrolled column, and the header cells alive.
    readonly property int realizedColumnCount: root.windowColumns.length
    readonly property int realizedColumnFirst: root.windowColumns.length > root.pinnedShown
                                               ? root.windowColumns[root.pinnedShown].column : 0
    readonly property int headerCellCount: root.arranged ? windowHeader.count : headerLabels.count
    // Each window column's place in `windowColumns`, by column.
    readonly property var windowPlaces: {
        const places = {}
        for (let i = 0; i < root.windowColumns.length; ++i)
            places[root.windowColumns[i].column] = i
        return places
    }
    // Where the scrolled columns pass under the pinned ones, from the first column's start.
    readonly property real pinnedEdge: {
        let edge = 0
        for (let i = 0; i < root.pinnedShown && i < root.windowColumns.length; ++i)
            edge = Math.max(edge, root.windowColumns[i].offset + root.windowColumns[i].width)
        return edge
    }
    readonly property real scrollX: hflick.contentX
    // The view's width across the columns, the select column left out.
    readonly property real columnViewport: Math.max(0, hflick.width - root.lead)
    // Whether every column keeps one line: the rows then share one height and no cell is measured.
    readonly property bool allOneLine: {
        for (const column of root.columns) {
            if (column.wrap)
                return false
        }
        return true
    }
    readonly property real oneLineRowHeight: root.estimatedRowHeight
    // Each realized row's height by view row, and the mean of them, which a row not realized counts as.
    property var rowHeights: ({})
    property real meanRowHeight: root.estimatedRowHeight
    // Test-only: how many cell heights the rows have measured for their own heights.
    property int rowMeasures: 0
    readonly property real sourceContentHeight: {
        let height = Math.max(0, root.totalRows) * root.meanRowHeight
        for (const key in root.rowHeights)
            height += root.rowHeights[key] - root.meanRowHeight
        return Math.max(0, height)
    }
    // The focused cell, which C++ writes; -1 before any.
    property int focusRow: -1
    property int focusColumn: -1
    // The counts assistive technology is told: the rows, -1 while they are a lower bound or not known, and the visible
    // columns.
    property int accessibleRowCount: -1
    property int accessibleColumnCount: 0
    // The status line.
    property string statusText: ""
    property bool statusShown: false
    property bool retryShown: false
    readonly property real statusHeight: root.statusShown ? statusLine.implicitHeight : 0
    // The sideways scroll's bar, made from the `scrollBar` part (`MorphScrollBar` or its restyle), which C++ passes at
    // creation; null without one.
    property Component bar: null
    property ScrollBar barItem: null
    // The bar takes the height it needs along the table's foot while it shows, so the rows end above it.
    readonly property real barHeight: hflick.interactive && root.barItem ? root.barItem.implicitHeight : 0
    // What draws under the header's column labels: MorphTableHeaderBackground, or the `tableHeader` restyle. C++
    // sets it when it makes the table.
    property Component headerBackground: null
    readonly property Item headerBackgroundItem: headerBackgroundLoader.item as Item
    // A content or stretch column's widest-cell natural width (the cell's `widthFreeNatural` when it has one, else its
    // `implicitWidth`), cached per column index; [] until seeded. A cell growing only raises its entry (no rescan
    // needed); a cell shrinking, a row leaving, or a cell's/row's visibility changing can only be resolved by rescanning
    // that column, since the cache does not know whether the row that set its current value is still there or still
    // contributing. Reassigned (not mutated in place) whenever it changes, since a QML array notifies dependents only
    // on reassignment.
    property var columnCache: []
    // A stretch column's widest-cell least width, the narrowest its cells can be laid out without losing content (a
    // cell's `shrinkWidth` within its natural width, else its natural width), cached and kept like `columnCache`. Both
    // caches are written outside the `widths` binding, so no binding loop forms.
    property var leastCache: []
    // Test-only: how many rows a full column rescan has visited, summed across every rescan. Not a public API.
    property int rowScans: 0
    // Once the mount lays the columns out, no width is worked out here: nothing reads every column per frame.
    readonly property var widths: root.arranged ? [] : root.columnWidths(width - root.lead)

    // A column's declared width describes its content, with `table.cellPaddingX` added on both sides, so the padding
    // never takes room from the content: a fixed column is its units plus the padding (a unit is a body character,
    // or the mono face's advance in a column whose role is mono, whatever its cells draw), a content column its widest
    // cell plus the padding. No column is narrower than its header: a content column is at least its header and its
    // widest cell, with the padding a side and never under 8 units.
    //
    // A stretch column starts from its content. It has a natural width `n` (the larger of its header and its widest
    // cell), a least width `l` (`n` in a column that does not wrap; in one that does, the widest word of its cells,
    // never under the header) and a floor `f` (the header, or the 8 units alone with `shrink`, so the column can be
    // narrower than its header, which then elides). With `L` the width left after the fixed and content columns:
    //  1. `L >= sum(n)`: each is `n` plus its weighted share of `L - sum(n)`.
    //  2. `sum(l) <= L < sum(n)`: each is `l` plus the share `(n - l) * (L - sum(l)) / (sum(n) - sum(l))` of the room
    //     between its widths.
    //  3. `sum(f) <= L < sum(l)`: each starts at `l`, and the deficit is taken from the columns with `shrink`, last
    //     first, each down to `f`, then from the others in the same order.
    //  4. `L < sum(f)`: each is `f`, and the columns scroll sideways.
    // In steps 1 to 3 the stretch columns add up to `L` exactly, and no column narrows as the table widens. Columns
    // wider in sum than the table by a pixel or more scroll sideways (`hflick`).
    //
    // Each column's widths, read from the caches and the headers, never from the table's own width.
    function measure(): var {
        // `advanceWidth` is a plain call, so the font is read here for the widths to follow a header font that changes.
        void headerMetrics.font
        const pad = 2 * Theme.tableCellPaddingX
        const out = []
        for (let i = 0; i < root.columns.length; ++i) {
            const column = root.columns[i]
            const mark = column.sortable ? headerMetrics.advanceWidth(root.ascendingMark) : 0
            const header = Math.max(headerMetrics.advanceWidth(column.label) + mark + pad,
                                    8 * Theme.unitWidth)
            let natural = header
            let least = header
            let floor = header
            if (column.kind === 1) {
                // A mono column's n characters are whole pixels wide, so the text, which a pixel of rounding can make
                // wider than n advances, is never cut short for want of it.
                natural = (column.role === 6 ? Math.ceil(column.amount * Theme.monoAdvance)
                                             : column.amount * Theme.unitWidth) + pad
                least = natural
                floor = natural
            } else if (column.kind === 0) {
                natural = Math.max(root.cachedWidth(i) + pad, header)
                least = natural
                floor = natural
            } else {
                natural = Math.max(root.cachedWidth(i) + pad, header)
                least = column.wrap ? Math.min(natural, Math.max(root.cachedLeast(i) + pad, header)) : natural
                floor = column.shrink === true ? 8 * Theme.unitWidth : header
            }
            out.push({"kind": column.kind, "natural": natural, "least": least, "floor": floor,
                      "weight": Math.max(1, column.amount)})
        }
        return out
    }

    function columnWidths(total: real): var {
        const metrics = root.measure()
        const out = []
        const stretch = []
        let used = 0
        for (let i = 0; i < metrics.length; ++i) {
            if (metrics[i].kind === 2) {
                out.push(0)
                stretch.push(i)
            } else {
                out.push(metrics[i].natural)
                used += metrics[i].natural
            }
        }
        if (stretch.length === 0)
            return out
        const left = Math.max(0, total - used)
        let natural = 0
        let least = 0
        let floor = 0
        let weights = 0
        for (const i of stretch) {
            natural += metrics[i].natural
            least += metrics[i].least
            floor += metrics[i].floor
            weights += metrics[i].weight
        }
        // The last column a step changes takes what the others leave, so the stretch columns add up to `left` exactly
        // rather than to within the rounding of the quotients.
        let last = -1
        if (left >= natural) {
            for (const i of stretch) {
                out[i] = metrics[i].natural + (left - natural) * metrics[i].weight / weights
                last = i
            }
        } else if (left >= least) {
            for (const i of stretch) {
                const m = metrics[i]
                out[i] = m.least + (m.natural - m.least) * (left - least) / (natural - least)
                if (m.natural > m.least)
                    last = i
            }
        } else if (left >= floor) {
            for (const i of stretch)
                out[i] = metrics[i].least
            let deficit = least - left
            for (const gives of [true, false]) {
                for (let k = stretch.length - 1; k >= 0 && deficit > 0; --k) {
                    const i = stretch[k]
                    if ((root.columns[i].shrink === true) !== gives)
                        continue
                    const take = Math.min(out[i] - metrics[i].floor, deficit)
                    if (take > 0) {
                        out[i] -= take
                        deficit -= take
                        last = Math.max(last, i)
                    }
                }
            }
        } else {
            for (const i of stretch)
                out[i] = metrics[i].floor
        }
        if (last >= 0) {
            let others = 0
            for (const i of stretch) {
                if (i !== last)
                    others += out[i]
            }
            out[last] = left - others
        }
        return out
    }

    // The columns' summed width: the rows' and the header's width, and what `hflick` scrolls across.
    readonly property real columnsWidth: {
        if (root.arranged)
            return root.lead + root.windowTotalWidth
        let sum = root.lead
        for (const w of root.widths)
            sum += w
        return sum
    }

    // The cached widest-cell width for column `index`; 0 until seeded. This must never write to `columnCache` --
    // it is read from inside the `widths` binding's own evaluation, and a write there while that evaluation is in
    // progress re-marks `widths` dirty and re-enters it, which previously looped (each newly-adopted row seeding
    // the cache mid-read, over and over, as `body` filled up). Seeding and rescanning both happen eagerly, outside
    // any binding evaluation: see `seedMissingColumns`, called from `onColumnsChanged` and from `TableImpl::adopted`
    // (table.cpp) whenever a row arrives, so the cache is always already the right length before `widths` reads it.
    function cachedWidth(index: int): real {
        return index < root.columnCache.length ? root.columnCache[index] : 0
    }

    // The cached widest-cell least width for stretch column `index`; 0 until seeded. Read-only, for the same reason.
    function cachedLeast(index: int): real {
        return index < root.leastCache.length ? root.leastCache[index] : 0
    }

    // What a cell asks for however wide it is laid out: its `widthFreeNatural` where it gives one, else its
    // `implicitWidth`.
    function naturalOf(cell: var): real {
        const free = cell.widthFreeNatural
        return free !== undefined && free >= 0 ? free : cell.implicitWidth
    }

    // The narrowest a cell can be laid out without losing content: its `shrinkWidth` within its natural width `natural`,
    // else the natural width.
    function leastOf(cell: var, natural: real): real {
        const shrink = cell.shrinkWidth
        return shrink !== undefined && shrink >= 0 ? Math.min(shrink, natural) : natural
    }

    // Grows `columnCache` to cover every column, scanning any not-yet-cached content or stretch column once. Called only
    // from outside a `widths` evaluation (onColumnsChanged, and C++ on each row adoption) -- never from `cachedWidth`.
    // columnCache's own per-entry default is 0, set by rescanColumn's own padding, which is indistinguishable from
    // a column whose widest cell really is 0 (an empty table) -- harmless here, since seeding an already-correct
    // zero with the same scan just repeats the (cheap, currently-zero-row) work rather than skipping it wrongly.
    function seedMissingColumns() {
        if (root.sourceRows)
            return
        for (let i = 0; i < root.columns.length; ++i) {
            if (root.columns[i].kind !== 1 && i >= root.columnCache.length)
                root.rescanColumn(i)
        }
    }

    // The widest natural and least widths of the shown cells in column `index`, scanning every row. Used only to seed
    // or rescan the cache -- not read from a property binding, so it does not make `widths` a dependency of every
    // cell's `implicitWidth`.
    function widestCell(index: int): var {
        let natural = 0
        let least = 0
        for (let r = 0; r < root.rowsHolder.children.length; ++r) {
            ++root.rowScans
            const row = root.rowsHolder.children[r]
            // A heading's one cell spans every column, so it widens none.
            if (!row.visible || row.heading || !row.content || index >= row.content.children.length)
                continue
            const cell = row.content.children[index]
            if (cell.visible) {
                const cellNatural = root.naturalOf(cell)
                natural = Math.max(natural, cellNatural)
                least = Math.max(least, root.leastOf(cell, cellNatural))
            }
        }
        return {"natural": natural, "least": least}
    }

    // Rescans column `index` from scratch and stores the result, reassigning the whole arrays so bindings on
    // `columnCache` (through `widths`) are notified. Virtualized, the result only ever grows the caches: this is
    // the one true chokepoint every shrink-capable caller (invalidateAllColumns, a cell's own visibleChanged
    // handler in table.cpp, reportCell's own shrink branch below) ultimately goes through, so the monotonic
    // rule from invalidateAllColumns' own comment is enforced here, not duplicated at each call site.
    function rescanColumn(index: int) {
        const column = root.columns[index]
        if (!column || column.kind === 1 || root.sourceRows)
            return
        const scanned = root.widestCell(index)
        if (column.kind === 2) {
            const nextLeast = root.leastCache.slice()
            while (nextLeast.length <= index)
                nextLeast.push(0)
            nextLeast[index] = root.virtualized ? Math.max(nextLeast[index], scanned.least) : scanned.least
            root.leastCache = nextLeast
        }
        const next = root.columnCache.slice()
        while (next.length <= index)
            next.push(0)
        next[index] = root.virtualized ? Math.max(next[index], scanned.natural) : scanned.natural
        root.columnCache = next
    }

    // Called by C++ (TableRowImpl::adopted, via the row's item, the same safe idiom `place` uses) and by a cell itself
    // (`MorphWidget.reportMeasure`) when a cell's widths change: grows the column's cached widths on the spot when the
    // new ones are larger (no rescan needed -- a cache can only grow correctly that way), otherwise rescans the column
    // fully, since a shrink might have been the row that set today's maximum.
    //
    // A report that arrives while a row is placing its cells is held until the placing is done: placing runs from the
    // change notification of `widths`, and writing a cache that `widths` reads from inside that notification would
    // make `widths` a binding loop.
    property int placing: 0
    property var deferredReports: []
    function flushReports() {
        const reports = root.deferredReports
        root.deferredReports = []
        for (const report of reports)
            root.reportCell(report[0], report[1])
    }
    function reportCell(index: int, cell: var) {
        if (root.placing > 0) {
            root.deferredReports.push([index, cell])
            Qt.callLater(root.flushReports)
            return
        }
        const column = root.columns[index]
        if (!column || column.kind === 1 || !cell || !cell.visible || cell.parent?.parent?.heading)
            return
        // Over a source the mount holds the widths: a cell wider than its column asks it to grow.
        if (root.sourceRows) {
            const place = root.windowPlaces[index]
            const width = Math.ceil(root.naturalOf(cell) + 2 * Theme.tableCellPaddingX)
            if (place === undefined || width > root.windowColumns[place].width)
                root.fire("measured", {"column": index, "width": width})
            return
        }
        const natural = root.naturalOf(cell)
        const stretch = column.kind === 2
        const least = stretch ? root.leastOf(cell, natural) : 0
        const cachedNatural = root.cachedWidth(index)
        const cachedLeast = root.cachedLeast(index)
        if (natural < cachedNatural || (stretch && least < cachedLeast)) {
            root.rescanColumn(index)
            return
        }
        if (stretch && (index >= root.leastCache.length || least > cachedLeast)) {
            const nextLeast = root.leastCache.slice()
            while (nextLeast.length <= index)
                nextLeast.push(0)
            nextLeast[index] = least
            root.leastCache = nextLeast
        }
        if (index >= root.columnCache.length || natural > cachedNatural) {
            const next = root.columnCache.slice()
            while (next.length <= index)
                next.push(0)
            next[index] = natural
            root.columnCache = next
        }
    }

    // Called by C++ (TableImpl::releasing, via the table's own item) when a row is leaving the table: a leaving
    // row's cell may have held a column's current cached maximum, which only a rescan can confirm or correct.
    // A single cell's own visibility flipping is handled more narrowly, by table.cpp rescanning only that cell's
    // column (not every column) -- see TableRowImpl::adopted's own visibleChanged connection.
    //
    // A row "leaving" here fires just as much for an ordinary dewindow (scrolled out of view, not gone from the
    // data) as for a real removal, and nothing tells the two apart at this call site -- which is exactly why
    // rescanColumn itself, not this function, is where virtualized tables stop shrinking: without that, a content
    // column's width would shrink back down whenever the row that set its current maximum dewindows, then grow
    // again if a wider row scrolls back into view -- visible jitter on every scroll, not a one-time correction.
    // The accepted cost of the monotonic rule is a column that stays wider than currently necessary after the row
    // that justified it has scrolled away, preferred over that jitter.
    function invalidateAllColumns() {
        if (root.sourceRows)
            return
        for (let i = 0; i < root.columns.length; ++i) {
            if (root.columns[i].kind !== 1)
                root.rescanColumn(i)
        }
    }

    onColumnsChanged: {
        root.columnCache = []
        root.leastCache = []
        root.seedMissingColumns()
    }

    // A Tab stop: the focus goes on to the current row, or the first one.
    activeFocusOnTab: true
    onActiveFocusChanged: {
        if (activeFocus && !root.sourceRows)
            root.focusRow(root.shownRows().includes(root.currentRow) ? root.currentRow : root.shownRows()[0])
    }

    // Spanning cells: C++ resolves which row spans which columns (`spanPlan` on the origin row), and here the
    // heights follow from the rows' own heights. A block, the rows a spanning cell covers, is as tall as those rows
    // together (a row's `implicitHeight`); when the cell asks for more, the last row it covers is given the
    // difference (`spanExtra`), so the rows stay rows and the cell never feeds a row's own height back to itself.
    // Called from the rows' height changes and by C++, once per event loop turn.
    property bool spansQueued: false
    function scheduleSpans() {
        if (!root.spansQueued) {
            root.spansQueued = true
            Qt.callLater(root.recomputeSpans)
        }
    }
    function recomputeSpans() {
        root.spansQueued = false
        const rows = []
        for (let i = 0; i < root.rowsHolder.children.length; ++i) {
            const row = root.rowsHolder.children[i]
            if (row.visible && row.table === root)
                rows.push(row)
        }
        const extras = new Map()
        const inset = 2 * Theme.tableCellPaddingY
        for (let at = 0; at < rows.length; ++at) {
            for (const span of rows[at].spanPlan) {
                const last = Math.min(at + span.rows, rows.length) - 1
                let own = 0
                for (let r = at; r <= last; ++r)
                    own += rows[r].ownHeight
                const cell = rows[at].content.children[span.column]
                const need = cell ? cell.implicitHeight + inset : 0
                extras.set(rows[last], Math.max(extras.get(rows[last]) ?? 0, need - own))
            }
        }
        for (const row of rows)
            row.spanExtra = Math.max(0, extras.get(row) ?? 0)
        for (let at = 0; at < rows.length; ++at) {
            const blocks = []
            for (const span of rows[at].spanPlan) {
                const last = Math.min(at + span.rows, rows.length) - 1
                let height = 0
                for (let r = at; r <= last; ++r)
                    height += rows[r].implicitHeight
                blocks.push({"column": span.column, "height": height})
            }
            if (JSON.stringify(blocks) !== JSON.stringify(rows[at].spanBlocks))
                rows[at].spanBlocks = blocks
        }
    }

    // The data rows the user can see, in order: a heading row is never current, selected or counted.
    function shownRows(): var {
        const shown = []
        for (let i = 0; i < root.rowsHolder.children.length; ++i) {
            const row = root.rowsHolder.children[i]
            if (row.visible && row.table === root && !row.heading)
                shown.push(row)
        }
        return shown
    }

    function focusRow(row: Item) {
        if (row) {
            root.currentRow = row
            row.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    // The table's own keys, while the table or one of its rows holds the focus itself; a key a control in a cell
    // leaves goes on outwards. The arrows, Home, End, PageUp and PageDown move the current row; Shift with Up or Down
    // moves it and selects from the anchor; Space toggles it (selects it in single mode); Ctrl+A selects every row,
    // or clears them, in multiple mode only. Enter is the row's own key.
    function ownKey(event: var): bool {
        const focused = root.Window.activeFocusItem
        if (focused !== root && (focused as MorphTableRow)?.table !== root)
            return false
        if (root.sourceRows)
            return root.sourceKey(event)
        const rows = root.shownRows()
        const at = rows.indexOf(root.currentRow)
        const mods = event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
        if (event.key === Qt.Key_A && mods === Qt.ControlModifier) {
            if (!root.multiple)
                return false
            root.fire("selectAll", undefined)
            return true
        }
        if (event.key === Qt.Key_Space && mods === Qt.NoModifier) {
            if (at >= 0)
                rows[at].fire("tap", {"ctrl": true})
            return true
        }
        const page = Math.max(1, Math.floor((root.Window.height > 0 ? root.Window.height : 0) /
                                            Math.max(1, root.estimatedRowHeight)) - 1)
        let next = -1
        if (event.key === Qt.Key_Up)
            next = Math.max(0, at - 1)
        else if (event.key === Qt.Key_Down)
            next = at < 0 ? 0 : Math.min(rows.length - 1, at + 1)
        else if (event.key === Qt.Key_Home && mods === Qt.NoModifier)
            next = 0
        else if (event.key === Qt.Key_End && mods === Qt.NoModifier)
            next = rows.length - 1
        else if (event.key === Qt.Key_PageUp && mods === Qt.NoModifier)
            next = Math.max(0, at - page)
        else if (event.key === Qt.Key_PageDown && mods === Qt.NoModifier)
            next = Math.min(rows.length - 1, at + page)
        if (next < 0 || (mods !== Qt.NoModifier && mods !== Qt.ShiftModifier))
            return false
        const extend = mods === Qt.ShiftModifier
        if (extend && event.key !== Qt.Key_Up && event.key !== Qt.Key_Down)
            return false
        if (extend && at >= 0)
            rows[at].fire("anchor", undefined)
        root.focusRow(rows[next])
        if (extend)
            rows[next].fire("tap", {"shift": true})
        return true
    }

    implicitWidth: Math.max(root.showHeader ? header.implicitWidth : 0, body.implicitWidth)
    // The width the columns ask for, whatever the table's own width: the lead and each column's natural width, read
    // from the caches and the headers. `implicitWidth` follows `width` through the header, so a table that is a cell
    // of another table reports this instead.
    widthFreeNatural: {
        if (root.arranged)
            return Math.ceil(root.lead + root.windowTotalWidth)
        let sum = root.lead
        for (const m of root.measure())
            sum += m.natural
        // In whole pixels, so a sum that wobbles in its last digits does not read as a change.
        return Math.ceil(sum)
    }
    // Unvirtualized: unchanged, the realized rows' own full height. Virtualized: the realized rows' real height
    // (whatever that happens to measure) plus the estimated height of every row this table is not realizing right
    // now -- otherwise the table's own size, and so its container's scrollbar, would shrink every time a row
    // dewindows, which is the "table visually collapses" symptom this design exists to avoid, not just the
    // underlying widget cost.
    implicitHeight: headerArea.height + root.barHeight + root.statusHeight +
                    (root.sourceRows ? root.sourceContentHeight
                                     : body.implicitHeight +
                                           (root.virtualized ? root.heightBefore(root.realizedFirst) +
                                                                   root.heightBefore(root.totalRows) -
                                                                   root.heightBefore(root.realizedFirst +
                                                                                     root.realizedCount)
                                                             : 0))
    // A table whose columns are wider than it scrolls sideways, and says so once.
    needWidth: root.columnsWidth
    haveWidth: root.width
    overflowKind: "table"

    FontMetrics {
        id: headerMetrics
        font: Theme.tableHeaderFont
    }

    // The table's area, which scrolls sideways only while its columns are wider than the table: a table that fits
    // takes no drag and no wheel event, so an ancestor's scroll keeps them, and a vertical wheel always passes on.
    // It holds the header over the rows' own vertical area, `flick`.
    Flickable {
        id: hflick
        objectName: "hflick"
        anchors.fill: parent
        anchors.bottomMargin: root.statusHeight
        onContentXChanged: root.reportColumns()
        onWidthChanged: root.reportColumns()
        contentWidth: Math.max(root.columnsWidth, width)
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        // Columns wider than the table by less than a pixel are rounding, not content to scroll to.
        interactive: root.columnsWidth - width >= 1
        clip: interactive
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.horizontal: root.barItem

        Item {
            id: headerArea
            visible: root.showHeader
            width: hflick.contentWidth
            implicitWidth: header.implicitWidth
            height: !root.showHeader ? 0 : root.arranged ? headerMetrics.height + 2 * Theme.tableCellPaddingY
                                                         : header.implicitHeight

            Loader {
                id: headerBackgroundLoader
                anchors.fill: parent
                sourceComponent: root.headerBackground
            }
            Binding {
                target: headerBackgroundLoader.item
                property: "scheme"
                value: root.scheme
            }

            Row {
                id: header

                // The select-all box: checked while every shown row is selected, partly while some are. It shows the
                // selection and never changes its own state, so a selection the document refuses leaves it as it was.
                Item {
                    visible: root.selectColumn
                    width: root.lead
                    height: parent.height

                    CheckBox {
                        id: selectAllBox
                        objectName: "selectAll"
                        anchors.centerIn: parent
                        padding: 0
                        indicator: MorphCheckIndicator {
                            objectName: "boxIndicator"
                            x: selectAllBox.leftPadding
                            y: (selectAllBox.height - height) / 2
                            scheme: root.scheme
                            checked: selectAllBox.checkState === Qt.Checked
                            partial: selectAllBox.checkState === Qt.PartiallyChecked
                            focused: selectAllBox.visualFocus
                        }
                        implicitWidth: indicator.implicitWidth + leftPadding + rightPadding
                        implicitHeight: indicator.implicitHeight + topPadding + bottomPadding
                        tristate: true
                        enabled: root.shownRows().length > 0
                        checkState: root.selectedShown === 0 ? Qt.Unchecked
                                                             : root.selectedShown === root.shownRows().length
                                                               ? Qt.Checked : Qt.PartiallyChecked
                        nextCheckState: function() {
                            return checkState
                        }
                        onClicked: root.fire("selectAll", undefined)
                        Accessible.name: root.selectAllLabel
                    }
                }

                Repeater {
                    id: headerLabels
                    model: root.arranged ? [] : root.columns

                    // A sortable column's header is a Tab stop, and a click, Enter or Space on it asks to sort by
                    // the column; the table shows the sort it is given, never one of its own.
                    Label {
                        id: headerLabel
                        objectName: "header" + index
                        textFormat: Text.PlainText
                        font: Theme.tableHeaderFont
                        required property int index
                        required property var modelData
                        readonly property bool sorted: root.sortColumn === index
                        width: root.widths[index] !== undefined ? root.widths[index] : 0
                        text: modelData.label + (sorted ? (root.sortDescending ? root.descendingMark
                                                                               : root.ascendingMark) : "")
                        activeFocusOnTab: modelData.sortable === true
                        Accessible.role: Accessible.ColumnHeader
                        Accessible.name: modelData.label
                        Accessible.description: !sorted ? "" : root.sortDescending ? "Sorted descending"
                                                                                   : "Sorted ascending"
                        Keys.onPressed: event => {
                            if (modelData.sortable === true && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                                                || event.key === Qt.Key_Space)) {
                                root.fire("sort", index)
                                event.accepted = true
                            }
                        }
                        TapHandler {
                            enabled: headerLabel.modelData.sortable === true
                            onTapped: root.fire("sort", headerLabel.index)
                        }
                        color: root.scheme.muted
                        topPadding: Theme.tableCellPaddingY
                        bottomPadding: Theme.tableCellPaddingY
                        leftPadding: Theme.tableCellPaddingX
                        rightPadding: Theme.tableCellPaddingX
                        // Over its cells: a number column's header sits over its digits.
                        horizontalAlignment: modelData.align === 3 ? Text.AlignRight
                                                                   : modelData.align === 2 ? Text.AlignHCenter
                                                                                           : Text.AlignLeft
                        elide: Text.ElideRight
                    }
                }
            }

            // The header cells of the column window, each at its column's offset; the pinned ones held at the leading
            // edge, over the rest.
            Repeater {
                id: windowHeader
                model: root.arranged ? root.windowColumns : []

                Label {
                    id: windowLabel
                    required property int index
                    required property var modelData
                    readonly property var column: root.columns[modelData.column] ?? ({"label": "", "align": 1,
                                                                                       "sortable": false})
                    readonly property bool pinned: index < root.pinnedShown
                    readonly property bool sorted: root.sortColumn === modelData.column
                    objectName: "header" + modelData.column
                    x: root.lead + modelData.offset + (pinned ? root.scrollX : 0)
                    z: pinned ? 1 : 0
                    width: modelData.width
                    height: headerArea.height
                    textFormat: Text.PlainText
                    font: Theme.tableHeaderFont
                    text: column.label + (sorted ? (root.sortDescending ? root.descendingMark : root.ascendingMark) : "")
                    activeFocusOnTab: column.sortable === true
                    Accessible.role: Accessible.ColumnHeader
                    Accessible.name: column.label
                    Accessible.description: !sorted ? "" : root.sortDescending ? "Sorted descending" : "Sorted ascending"
                    Keys.onPressed: event => {
                        if (windowLabel.column.sortable === true && (event.key === Qt.Key_Return ||
                                                                     event.key === Qt.Key_Enter ||
                                                                     event.key === Qt.Key_Space)) {
                            root.fire("sort", windowLabel.modelData.column)
                            event.accepted = true
                        }
                    }
                    TapHandler {
                        enabled: windowLabel.column.sortable === true
                        onTapped: root.fire("sort", windowLabel.modelData.column)
                    }
                    color: root.scheme.muted
                    background: Rectangle {
                        visible: windowLabel.pinned
                        color: root.scheme.surface
                    }
                    topPadding: Theme.tableCellPaddingY
                    bottomPadding: Theme.tableCellPaddingY
                    leftPadding: Theme.tableCellPaddingX
                    rightPadding: Theme.tableCellPaddingX
                    horizontalAlignment: column.align === 3 ? Text.AlignRight
                                                            : column.align === 2 ? Text.AlignHCenter : Text.AlignLeft
                    elide: Text.ElideRight
                }
            }
        }

        // Unvirtualized, flick is a transparent pass-through: its height is body's own, nothing clips, and it never
        // scrolls on its own, so an ancestor (a panel, a dialog's ScrollView, the page) does. Virtualized, it is the
        // table's own bounded viewport: it takes the height the document gave the table, less the header, and clips
        // to it, with topSpace and bottomSpace standing in for the rows not realized right now.
        Flickable {
            id: flick
            objectName: "flick"
            y: headerArea.height
            width: hflick.contentWidth
            height: root.virtualized ? Math.max(0, hflick.height - headerArea.height - root.barHeight)
                                     : body.implicitHeight
            clip: root.virtualized
            interactive: root.virtualized
            flickableDirection: Flickable.VerticalFlick
            contentWidth: width
            contentHeight: root.sourceRows ? root.sourceContentHeight
                                           : topSpace.height + body.implicitHeight + bottomSpace.height
            boundsBehavior: Flickable.StopAtBounds

            onContentYChanged: {
                if (!root.restoring && !root.sourceRows)
                    root.anchorAt(flick.contentY)
                root.reportVisibleRange()
            }
            // The viewport's own size decides how many rows it shows, so the first layout and a resize report
            // the range exactly as a scroll does.
            onHeightChanged: root.reportVisibleRange()

            Item {
                id: topSpace
                width: 1
                height: root.virtualized && !root.sourceRows ? root.heightBefore(root.realizedFirst) : 0
            }

            ColumnLayout {
                id: body
                anchors.top: topSpace.bottom
                width: flick.width
                spacing: 0
                onImplicitHeightChanged: root.remeasureRowHeight()
            }

            Item {
                id: bottomSpace
                width: 1
                anchors.top: body.bottom
                height: root.virtualized && !root.sourceRows
                            ? root.heightBefore(root.totalRows) -
                              root.heightBefore(root.realizedFirst + root.realizedCount)
                            : 0
            }

            // The rows of a table over a source, each placed at the top of its view row by `layoutRows`.
            Item {
                id: rowsPlane
                width: flick.width
                height: root.sourceContentHeight
            }
        }
    }

    // The status line, under the rows and outside the sideways scroll. A polite live region: its text is its
    // accessible name, so a change is read out as the name changes.
    RowLayout {
        id: statusLine
        objectName: "statusLine"
        visible: root.statusShown
        x: Theme.tableCellPaddingX
        y: root.height - root.statusHeight
        width: root.width - 2 * Theme.tableCellPaddingX
        spacing: Theme.tableCellPaddingX

        Text {
            objectName: "statusText"
            Layout.fillWidth: true
            text: root.statusText
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font: Theme.smallFont
            color: root.scheme.muted
            topPadding: Theme.tableCellPaddingY
            bottomPadding: Theme.tableCellPaddingY
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
        Button {
            objectName: "retry"
            visible: root.retryShown
            text: "Retry"
            onClicked: root.fire("retry", undefined)
        }
    }

    // Records the height of every realized row against its view index, takes the estimate from every row measured so
    // far, and then puts the viewport back on its anchor row: a row that was only estimated and is now measured moves
    // the content above the viewport, and the anchor keeps what the user looks at where it was. Only runs
    // virtualized: an unvirtualized table never needs an estimate, since every row is always realized.
    function remeasureRowHeight() {
        if (!root.virtualized || root.sourceRows || root.rowsHolder.children.length === 0)
            return
        let changed = false
        let at = root.realizedFirst
        for (let i = 0; i < body.children.length; ++i) {
            const row = body.children[i]
            if (!row.visible || row.height <= 0)
                continue
            const known = root.measured[at]
            if (known === undefined) {
                root.measuredIndices.push(at)
                root.measuredSum += row.height
                root.measured[at] = row.height
                changed = true
            } else if (Math.abs(known - row.height) > 0.01) {
                root.measuredSum += row.height - known
                root.measured[at] = row.height
                changed = true
            }
            ++at
        }
        if (!changed)
            return
        root.estimatedRowHeight = root.measuredSum / root.measuredIndices.length
        ++root.measuredRevision
        root.restoreAnchor()
    }

    // The height of the first `count` rows in view order: the measured height of each that has one, the estimate for
    // the rest.
    function heightBefore(count: int): real {
        void root.measuredRevision
        let sum = 0
        let known = 0
        for (const index of root.measuredIndices) {
            if (index < count) {
                sum += root.measured[index]
                ++known
            }
        }
        return sum + Math.max(0, count - known) * root.estimatedRowHeight
    }

    // The view index of the row that covers vertical position `y` of the content, and the position of its top.
    function rowAt(y: real): var {
        if (!root.virtualized || y <= 0)
            return {"index": 0, "top": 0}
        let low = 0
        let high = Math.max(0, root.totalRows - 1)
        while (low < high) {
            const middle = Math.ceil((low + high) / 2)
            if (root.heightBefore(middle) <= y)
                low = middle
            else
                high = middle - 1
        }
        return {"index": low, "top": root.heightBefore(low)}
    }

    function anchorAt(y: real) {
        const found = root.rowAt(y)
        root.anchorRow = found.index
        root.anchorOffset = y - found.top
    }

    function restoreAnchor() {
        const bar = flick.ScrollBar.vertical
        if (!root.virtualized || (bar && bar.pressed))
            return
        const target = root.heightBefore(root.anchorRow) + root.anchorOffset
        if (Math.abs(flick.contentY - target) < 0.5)
            return
        root.restoring = true
        flick.contentY = target
        root.restoring = false
    }

    // Converts the Flickable's contentY (how far scrolled) and height (how much is visible) into the view-index
    // range C++ should realize, from the same row heights the spacers use. Reported via fire(), the same channel
    // every other user-driven event (a tap, an activation) already uses -- a scroll is a user-driven request for
    // different rows to be realized, in the same sense a tap is a request to select a row.
    function reportVisibleRange() {
        if (root.sourceRows) {
            const first = root.rowAtY(flick.contentY)
            const last = root.rowAtY(flick.contentY + Math.max(0, flick.height - 1))
            root.fire("scroll", {"first": first, "count": Math.max(0, Math.min(last - first + 1,
                                                                               root.totalRows - first))})
            return
        }
        if (!root.virtualized || root.estimatedRowHeight <= 0)
            return
        root.remeasureRowHeight()
        const first = root.rowAt(flick.contentY).index
        // The window bounds what can show: before the first layout a table's height is its whole content's, and
        // realizing every row for that one pass would cost far more than the screen can ever display.
        const shown = root.Window.height > 0 ? Math.min(flick.height, root.Window.height) : flick.height
        const bottom = flick.contentY + shown
        let last = first
        while (last < root.totalRows && root.heightBefore(last) < bottom)
            ++last
        root.fire("scroll", {
            "first": first,
            "count": Math.min(Math.max(1, last - first) + 1, Math.max(0, root.totalRows - first))
        })
    }

    onRealizedFirstChanged: root.remeasureRowHeight()
    onRealizedCountChanged: root.remeasureRowHeight()

    // ── A table over a source ──

    // Where view row `viewRow` starts: the mean height for each row before it, corrected by the rows measured.
    function rowTop(viewRow: int): real {
        let top = viewRow * root.meanRowHeight
        for (const key in root.rowHeights) {
            if (Number(key) < viewRow)
                top += root.rowHeights[key] - root.meanRowHeight
        }
        return top
    }

    // The view row at `y`, from the top of the rows.
    function rowAtY(y: real): int {
        const mean = Math.max(1, root.meanRowHeight)
        const known = Object.keys(root.rowHeights).map(Number).sort((a, b) => a - b)
        let row = 0
        let base = 0
        for (const measured of known) {
            if (y < base + (measured - row) * mean)
                break
            base += (measured - row) * mean
            row = measured
            if (y < base + root.rowHeights[measured])
                return measured
            base += root.rowHeights[measured]
            row = measured + 1
        }
        return Math.max(0, Math.min(Math.max(0, root.totalRows - 1), row + Math.floor((y - base) / mean)))
    }

    // Lays the realized rows out once per event loop turn: called as rows get their view rows and change height.
    property bool rowsQueued: false
    function scheduleRows() {
        if (root.sourceRows && !root.rowsQueued) {
            root.rowsQueued = true
            Qt.callLater(root.layoutRows)
        }
    }

    // Keeps each realized row's height (the larger of what it had and what it measures now; a row that left is
    // forgotten), places every row at its view row, and moves the view by what the rows above the first visible one
    // changed, so the rows in view stay where they are.
    function layoutRows() {
        root.rowsQueued = false
        const anchor = root.rowAtY(flick.contentY)
        const into = flick.contentY - root.rowTop(anchor)
        const rows = []
        for (let i = 0; i < rowsPlane.children.length; ++i) {
            const row = rowsPlane.children[i]
            if (row.table === root && row.viewRow >= 0)
                rows.push(row)
        }
        const heights = {}
        let sum = 0
        for (const row of rows) {
            const height = Math.max(root.rowHeights[row.viewRow] ?? 0, row.measuredHeight)
            heights[row.viewRow] = height
            sum += height
        }
        root.rowHeights = heights
        root.meanRowHeight = rows.length > 0 ? sum / rows.length : root.estimatedRowHeight
        for (const row of rows) {
            row.y = root.rowTop(row.viewRow)
            row.height = heights[row.viewRow]
        }
        const target = root.rowTop(anchor) + into
        if (Math.abs(target - flick.contentY) >= 0.5)
            flick.contentY = target
    }

    // Reports the sideways scroll and the viewport, in pixels from the first column's start.
    property bool settingOffset: false
    function reportColumns() {
        if (root.sourceRows && !root.settingOffset)
            root.fire("hscroll", {"scrollX": Math.round(hflick.contentX), "viewport": Math.round(root.columnViewport)})
    }
    onSourceRowsChanged: Qt.callLater(root.reportColumns)

    // Called by C++: the mount moved the view so the columns in view stay where they were; it is not told back.
    function setHorizontalOffset(scrollX: int) {
        root.settingOffset = true
        hflick.contentX = Math.max(0, scrollX)
        root.settingOffset = false
    }

    // Called by C++: brings a view row into view, as little as it takes.
    function scrollToRow(viewRow: var) {
        const top = root.rowTop(viewRow)
        const bottom = top + (root.rowHeights[viewRow] ?? root.meanRowHeight)
        if (top < flick.contentY)
            flick.contentY = top
        else if (bottom > flick.contentY + flick.height)
            flick.contentY = Math.max(0, bottom - flick.height)
    }

    // Puts view row `viewRow` at the top of the view.
    function scrollRowToTop(viewRow: int) {
        flick.contentY = root.rowTop(viewRow)
    }

    // Called by C++ when focus moves: the focused row and its column come into view.
    function showFocusCell() {
        if (root.focusRow < 0)
            return
        root.scrollToRow(root.focusRow)
        const place = root.windowPlaces[root.focusColumn]
        if (place === undefined || place < root.pinnedShown)
            return
        const entry = root.windowColumns[place]
        if (entry.offset - hflick.contentX < root.pinnedEdge)
            hflick.contentX = Math.max(0, entry.offset - root.pinnedEdge)
        else if (entry.offset + entry.width - hflick.contentX > root.columnViewport)
            hflick.contentX = entry.offset + entry.width - root.columnViewport
    }

    // The keys over a source: the moves go to the mount, which resolves them over rows and columns not realized;
    // Space and Enter act on the focused row when it is realized. The order of `FocusMove`.
    function sourceKey(event: var): bool {
        const mods = event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)
        let move = -1
        if (mods === Qt.ControlModifier && event.key === Qt.Key_Home)
            move = 6
        else if (mods === Qt.ControlModifier && event.key === Qt.Key_End)
            move = 7
        else if (mods === Qt.NoModifier) {
            const keys = [Qt.Key_Up, Qt.Key_Down, Qt.Key_Left, Qt.Key_Right, Qt.Key_Home, Qt.Key_End, -1, -1,
                          Qt.Key_PageUp, Qt.Key_PageDown]
            move = keys.indexOf(event.key)
        }
        if (move >= 0) {
            root.fire("focusMove", move)
            return true
        }
        const row = root.rowAt(root.focusRow)
        if (!row || mods !== Qt.NoModifier)
            return false
        if (event.key === Qt.Key_Space) {
            row.fire("tap", {"ctrl": true})
            return true
        }
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            row.fire("activate", undefined)
            return true
        }
        return false
    }

    // Where a column's cells go, from the first column's start: its window entry once the mount lays the columns out,
    // else its place at the table's own widths; null for a column not drawn.
    function columnSlot(column: int): var {
        if (root.arranged) {
            const place = root.windowPlaces[column]
            if (place === undefined)
                return null
            const entry = root.windowColumns[place]
            return {"offset": entry.offset, "width": entry.width, "pinned": place < root.pinnedShown}
        }
        if (column < 0 || column >= root.widths.length)
            return null
        let offset = 0
        for (let i = 0; i < column; ++i)
            offset += root.widths[i]
        return {"offset": offset, "width": root.widths[column], "pinned": false}
    }

    // The realized row at view row `viewRow`, or null.
    function rowAt(viewRow: int): var {
        for (let i = 0; i < rowsPlane.children.length; ++i) {
            const row = rowsPlane.children[i]
            if (row.table === root && row.viewRow === viewRow && row.visible)
                return row
        }
        return null
    }

    // For tests: the focused cell; whether a cell is realized and shown; a column's width; a row's height; a
    // cell's load state as drawn (0 loaded, 1 unloaded, 2 failed).
    function focusedRow(): int {
        return root.focusRow
    }
    function focusedColumn(): int {
        return root.focusColumn
    }
    function cellRealized(viewRow: int, column: int): bool {
        const row = root.rowAt(viewRow)
        if (!row)
            return false
        for (let i = 0; i < row.content.children.length; ++i) {
            const cell = row.content.children[i]
            if (cell.measuredIndex === column && cell.visible)
                return true
        }
        return false
    }
    function columnWidth(column: int): real {
        const place = root.windowPlaces[column]
        if (place !== undefined)
            return root.windowColumns[place].width
        return column < root.widths.length ? root.widths[column] : 0
    }
    function rowHeight(viewRow: int): real {
        return root.rowHeights[viewRow] ?? root.meanRowHeight
    }
    function cellLoadAt(viewRow: int, column: int): int {
        const row = root.rowAt(viewRow)
        return row ? row.loadOf(column) : 0
    }

    // The bar is a child of the flickable itself, not of its content: the attached ScrollBar lays out only a bar whose
    // parent is the flickable. It is always on while the table scrolls, so a table at rest does not look cut off.
    Component.onCompleted: {
        if (root.bar) {
            root.barItem = root.bar.createObject(hflick, {
                orientation: Qt.Horizontal,
                visible: Qt.binding(() => hflick.interactive),
                scheme: Qt.binding(() => root.scheme)
            }) as ScrollBar
            root.barItem.policy = Qt.binding(() => hflick.interactive ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded)
            // The rows' own vertical bar: a virtualized table scrolls inside `flick`, which has no other way to be
            // dragged. The minimum size keeps the handle grabbable however many rows there are.
            const vertical = root.bar.createObject(flick, {
                orientation: Qt.Vertical,
                minimumSize: 0.08,
                visible: Qt.binding(() => root.virtualized && flick.contentHeight > flick.height),
                scheme: Qt.binding(() => root.scheme)
            }) as ScrollBar
            vertical.policy = ScrollBar.AlwaysOn
            flick.ScrollBar.vertical = vertical
        }
    }
}
