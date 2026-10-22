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

    // The columns: [{label, kind (0 content, 1 fixed, 2 stretch), amount}].
    property var columns: []
    property bool showHeader: true
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
    property real estimatedRowHeight: Theme.unitHeight + Theme.spaceOf(2)
    readonly property Item content: body
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
    readonly property var widths: root.columnWidths(width)

    function columnWidths(total: real): var {
        const out = []
        let used = 0
        let weights = 0
        for (let i = 0; i < root.columns.length; ++i) {
            const column = root.columns[i]
            let w = 0
            if (column.kind === 1)
                w = column.amount * Theme.unitWidth
            else if (column.kind === 0)
                w = Math.max(Math.max(headerMetrics.advanceWidth(column.label), root.cachedWidth(i)) + 2 * Theme.unitWidth,
                             8 * Theme.unitWidth)
            else
                weights += Math.max(1, column.amount)
            out.push(w)
            used += w
        }
        const left = Math.max(0, total - used)
        for (let i = 0; i < root.columns.length; ++i) {
            if (root.columns[i].kind === 2)
                out[i] = weights > 0 ? left * Math.max(1, root.columns[i].amount) / weights : 0
        }
        return out
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
            if (!row.visible || !row.content || index >= row.content.children.length)
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

    implicitWidth: Math.max(root.showHeader ? header.implicitWidth : 0, body.implicitWidth)
    // Unvirtualized: unchanged, the realized rows' own full height. Virtualized: the realized rows' real height
    // (whatever that happens to measure) plus the estimated height of every row this table is not realizing right
    // now -- otherwise the table's own size, and so its container's scrollbar, would shrink every time a row
    // dewindows, which is the "table visually collapses" symptom this design exists to avoid, not just the
    // underlying widget cost.
    implicitHeight: (root.showHeader ? header.implicitHeight : 0) + body.implicitHeight +
                    (root.virtualized ? (root.totalRows - root.realizedCount) * root.estimatedRowHeight : 0)

    FontMetrics {
        id: headerMetrics
        font: Theme.strongFont
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            id: headerArea
            visible: root.showHeader
            implicitWidth: header.implicitWidth
            implicitHeight: root.showHeader ? header.implicitHeight : 0

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

                Repeater {
                    model: root.columns

                    Label {
                        font: Theme.strongFont
                        required property int index
                        required property var modelData
                        width: root.widths[index] !== undefined ? root.widths[index] : 0
                        text: modelData.label
                        color: root.scheme.muted
                        topPadding: Theme.spaceOf(2)
                        bottomPadding: Theme.spaceOf(2)
                        leftPadding: Theme.spaceOf(2)
                        rightPadding: Theme.spaceOf(2)
                        elide: Text.ElideRight
                    }
                }
            }
        }

        // Unvirtualized, flick is a transparent pass-through: its content height tracks body's own (grow to
        // content, exactly as before this existed), nothing clips, and it never scrolls on its own -- an ancestor
        // (a panel, a dialog's own ScrollView, the page) still does, exactly as today. Virtualized, flick is the
        // table's own bounded viewport (see the design's "a virtualized table needs its own bounded viewport"):
        // Layout.fillHeight claims whatever height the document gave the table (the new requirement, §4.2a), and
        // flick clips to it, with topSpace/bottomSpace standing in for the rows not realized right now.
        Flickable {
            id: flick
            objectName: "flick"
            Layout.fillWidth: true
            Layout.fillHeight: root.virtualized
            Layout.preferredHeight: root.virtualized ? -1 : body.implicitHeight
            implicitHeight: root.virtualized ? -1 : body.implicitHeight
            clip: root.virtualized
            interactive: root.virtualized
            contentWidth: body.implicitWidth
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
}
