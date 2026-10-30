// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Rows of cells under column headers. Each column's width comes from its sizing: fixed units, the widest of its
// header and its shown cells for content, or a share of what is left for stretch; every row places its cells at
// those widths.
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
    // A content column's widest-cell width, cached per column index; [] until seeded. A cell growing only raises
    // its entry (no rescan needed); a cell shrinking, a row leaving, or a cell's/row's visibility changing can only
    // be resolved by rescanning that column, since the cache does not know whether the row that set its current
    // value is still there or still contributing. Reassigned (not mutated in place) whenever it changes, since a
    // QML array notifies dependents only on reassignment.
    property var columnCache: []
    // Test-only: how many rows a full column rescan has visited, summed across every rescan. Not a public API.
    property int rowScans: 0
    readonly property var widths: root.columnWidths(width - root.lead)

    // A column's declared width describes its content, with `table.cellPaddingX` added on both sides, so the padding
    // never takes room from the content: a fixed column is its units plus the padding (a unit is a body character,
    // or the mono face's advance in a column whose role is mono, whatever its cells draw), a content column its widest
    // cell plus the padding. No column is narrower than its header: a content column is at least its header and its
    // widest cell, a stretch column at least its header (its cells are meant to take what is left, so they do not
    // raise it), each with the padding a side and never under 8 units. A stretch column takes its share of what the others leave, and
    // a share under its floor is the floor, the rest shared among the others. Columns wider in sum than the table
    // scroll sideways (`hflick`).
    function columnWidths(total: real): var {
        const pad = 2 * Theme.tableCellPaddingX
        const out = []
        const floors = []
        let used = 0
        let pending = []
        for (let i = 0; i < root.columns.length; ++i) {
            const column = root.columns[i]
            const mark = column.sortable ? headerMetrics.advanceWidth(root.ascendingMark) : 0
            const header = Math.max(headerMetrics.advanceWidth(column.label) + mark + pad,
                                    8 * Theme.unitWidth)
            let w = 0
            if (column.kind === 1)
                // A mono column's n characters are whole pixels wide, so the text, which a pixel of rounding can make
                // wider than n advances, is never cut short for want of it.
                w = (column.role === 6 ? Math.ceil(column.amount * Theme.monoAdvance) : column.amount * Theme.unitWidth) + pad
            else if (column.kind === 0)
                w = Math.max(root.cachedWidth(i) + pad, header)
            else
                pending.push(i)
            out.push(w)
            floors.push(header)
            used += w
        }
        let left = Math.max(0, total - used)
        let floored = true
        while (floored && pending.length > 0) {
            floored = false
            let weights = 0
            for (const i of pending)
                weights += Math.max(1, root.columns[i].amount)
            const next = []
            for (const i of pending) {
                const share = left * Math.max(1, root.columns[i].amount) / weights
                if (share < floors[i]) {
                    out[i] = floors[i]
                    floored = true
                } else {
                    out[i] = share
                    next.push(i)
                }
            }
            if (floored) {
                for (const i of pending) {
                    if (!next.includes(i))
                        left = Math.max(0, left - floors[i])
                }
            }
            pending = next
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

    // Grows `columnCache` to cover every column, scanning any not-yet-cached content column once. Called only from
    // outside a `widths` evaluation (onColumnsChanged, and C++ on each row adoption) -- never from `cachedWidth`.
    // columnCache's own per-entry default is 0, set by rescanColumn's own padding, which is indistinguishable from
    // a column whose widest cell really is 0 (an empty table) -- harmless here, since seeding an already-correct
    // zero with the same scan just repeats the (cheap, currently-zero-row) work rather than skipping it wrongly.
    function seedMissingColumns() {
        for (let i = 0; i < root.columns.length; ++i) {
            if (root.columns[i].kind === 0 && i >= root.columnCache.length)
                root.rescanColumn(i)
        }
    }

    // The widest shown cell in column `index`, scanning every row. Used only to seed or rescan the cache -- not
    // read from a property binding, so it does not make `widths` a dependency of every cell's `implicitWidth`.
    function widestCell(index: int): real {
        let widest = 0
        for (let r = 0; r < body.children.length; ++r) {
            ++root.rowScans
            const row = body.children[r]
            // A heading's one cell spans every column, so it widens none.
            if (!row.visible || row.heading || !row.content || index >= row.content.children.length)
                continue
            const cell = row.content.children[index]
            if (cell.visible)
                widest = Math.max(widest, cell.implicitWidth)
        }
        return widest
    }

    // Rescans column `index` from scratch and stores the result, reassigning the whole array so bindings on
    // `columnCache` (through `widths`) are notified. Virtualized, the result only ever grows the cache: this is
    // the one true chokepoint every shrink-capable caller (invalidateAllColumns, a cell's own visibleChanged
    // handler in table.cpp, reportCellWidth's own shrink branch below) ultimately goes through, so the monotonic
    // rule from invalidateAllColumns' own comment is enforced here, not duplicated at each call site.
    function rescanColumn(index: int) {
        const next = root.columnCache.slice()
        while (next.length <= index)
            next.push(0)
        const scanned = root.widestCell(index)
        next[index] = root.virtualized ? Math.max(next[index], scanned) : scanned
        root.columnCache = next
    }

    // Called by C++ (TableRowImpl::adopted, via the row's item, the same safe idiom `place` uses) when a cell's
    // implicitWidth changes: grows the column's cached width on the spot when the new width is larger (no rescan
    // needed -- a cache can only grow correctly that way), otherwise rescans the column fully, since a shrink might
    // have been the row that set today's maximum.
    function reportCellWidth(index: int, width: real) {
        if (index >= root.columnCache.length || width > root.columnCache[index]) {
            const next = root.columnCache.slice()
            while (next.length <= index)
                next.push(0)
            next[index] = Math.max(next[index], width)
            root.columnCache = next
        } else if (width < root.columnCache[index]) {
            root.rescanColumn(index)
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
            if (root.columns[i].kind === 0)
                root.rescanColumn(i)
        }
    }

    onColumnsChanged: {
        root.columnCache = []
        root.seedMissingColumns()
    }

    // A Tab stop: the focus goes on to the current row, or the first one.
    activeFocusOnTab: true
    onActiveFocusChanged: {
        if (activeFocus)
            root.focusRow(root.shownRows().includes(root.currentRow) ? root.currentRow : root.shownRows()[0])
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
        interactive: root.columnsWidth > width
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
