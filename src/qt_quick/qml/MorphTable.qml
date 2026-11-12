// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Rows of cells under column headers. Each column's width comes from its sizing: fixed units, the widest of its
// header and its shown cells for content, or for stretch its content plus a share of what is left; every row places
// its cells at those widths.
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
    readonly property Item content: body
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
    readonly property var widths: root.columnWidths(width - root.lead)

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
        for (let r = 0; r < body.children.length; ++r) {
            ++root.rowScans
            const row = body.children[r]
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
        if (!column || column.kind === 1)
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
        if (activeFocus)
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
        for (let i = 0; i < body.children.length; ++i) {
            const row = body.children[i]
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
        for (let i = 0; i < body.children.length; ++i) {
            const row = body.children[i]
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
    implicitHeight: (root.showHeader ? header.implicitHeight : 0) + body.implicitHeight +
                    (root.virtualized ? (root.totalRows - root.realizedCount) * root.estimatedRowHeight : 0) +
                    root.barHeight
    // A table whose columns are wider than it scrolls sideways, and says so once.
    needWidth: root.columnsWidth
    haveWidth: root.width
    overflowKind: "table"

    FontMetrics {
        id: headerMetrics
        font: Theme.smallStrongFont
    }

    // The table's area, which scrolls sideways only while its columns are wider than the table: a table that fits
    // takes no drag and no wheel event, so an ancestor's scroll keeps them, and a vertical wheel always passes on.
    // It holds the header over the rows' own vertical area, `flick`.
    Flickable {
        id: hflick
        objectName: "hflick"
        anchors.fill: parent
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
            height: root.showHeader ? header.implicitHeight : 0

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
                    model: root.columns

                    // A sortable column's header is a Tab stop, and a click, Enter or Space on it asks to sort by
                    // the column; the table shows the sort it is given, never one of its own.
                    Label {
                        id: headerLabel
                        objectName: "header" + index
                        textFormat: Text.PlainText
                        font: Theme.smallStrongFont
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
            contentHeight: topSpace.height + body.implicitHeight + bottomSpace.height
            boundsBehavior: Flickable.StopAtBounds

            onContentYChanged: root.reportVisibleRange()

            Item {
                id: topSpace
                width: 1
                height: root.virtualized ? root.realizedFirst * root.estimatedRowHeight : 0
            }

            ColumnLayout {
                id: body
                anchors.top: topSpace.bottom
                width: flick.width
                spacing: 0
            }

            Item {
                id: bottomSpace
                width: 1
                anchors.top: body.bottom
                height: root.virtualized
                            ? Math.max(0, root.totalRows - root.realizedFirst - root.realizedCount) *
                              root.estimatedRowHeight
                            : 0
            }
        }
    }

    // Measures the realized rows actually mounted right now and folds their average into estimatedRowHeight, so
    // the spacers above correct themselves toward the real average the longer the table is used -- the "corrected
    // once real rows are measured" half of the row-height-estimation spike (§4.3). Only runs virtualized: an
    // unvirtualized table never needs an estimate, since every row is always realized.
    function remeasureRowHeight() {
        if (!root.virtualized || body.children.length === 0)
            return
        let total = 0
        let counted = 0
        for (let i = 0; i < body.children.length; ++i) {
            const row = body.children[i]
            if (row.visible && row.height > 0) {
                total += row.height
                ++counted
            }
        }
        if (counted > 0)
            root.estimatedRowHeight = total / counted
    }

    // Converts the Flickable's contentY (how far scrolled) and height (how much is visible) into the view-index
    // range C++ should realize, folding in the estimate's own correction first so a remeasure and a scroll report
    // always agree on the same estimatedRowHeight. Reported via fire(), the same channel every other user-driven
    // event (a tap, an activation) already uses -- a scroll is a user-driven request for different rows to be
    // realized, in the same sense a tap is a request to select a row.
    function reportVisibleRange() {
        if (!root.virtualized || root.estimatedRowHeight <= 0)
            return
        root.remeasureRowHeight()
        const first = Math.max(0, Math.floor(flick.contentY / root.estimatedRowHeight))
        const visibleRows = Math.ceil(flick.height / root.estimatedRowHeight) + 1
        root.fire("scroll", {
            "first": first,
            "count": Math.min(visibleRows, Math.max(0, root.totalRows - first))
        })
    }

    onRealizedFirstChanged: root.remeasureRowHeight()
    onRealizedCountChanged: root.remeasureRowHeight()

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
        }
    }
}
