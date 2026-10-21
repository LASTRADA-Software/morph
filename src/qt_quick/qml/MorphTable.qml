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
    // `columnCache` (through `widths`) are notified.
    function rescanColumn(index: int) {
        const next = root.columnCache.slice()
        while (next.length <= index)
            next.push(0)
        next[index] = root.widestCell(index)
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

    implicitWidth: Math.max(header.implicitWidth, body.implicitWidth)
    implicitHeight: header.implicitHeight + body.implicitHeight

    FontMetrics {
        id: headerMetrics
        font: Theme.strongFont
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            id: headerArea
            implicitWidth: header.implicitWidth
            implicitHeight: header.implicitHeight

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
                        elide: Text.ElideRight
                    }
                }
            }
        }

        ColumnLayout {
            id: body
            Layout.fillWidth: true
            spacing: 0
        }
    }
}
