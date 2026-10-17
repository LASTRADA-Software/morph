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
                w = Math.max(Math.max(headerMetrics.advanceWidth(column.label), root.widestCell(i)) + 2 * Theme.unitWidth,
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

    // The widest shown cell in column `index`. Reading each cell's implicitWidth here makes the widths binding
    // follow it, so a cell that grows widens its column.
    function widestCell(index: int): real {
        let widest = 0
        for (let r = 0; r < body.children.length; ++r) {
            const row = body.children[r]
            if (!row.visible || !row.content || index >= row.content.children.length)
                continue
            const cell = row.content.children[index]
            if (cell.visible)
                widest = Math.max(widest, cell.implicitWidth)
        }
        return widest
    }

    implicitWidth: Math.max(header.implicitWidth, body.implicitWidth)
    implicitHeight: header.implicitHeight + body.implicitHeight

    FontMetrics {
        id: headerMetrics
        font.bold: true
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Row {
            id: header

            Repeater {
                model: root.columns

                Label {
                    required property int index
                    required property var modelData
                    width: root.widths[index] !== undefined ? root.widths[index] : 0
                    text: modelData.label
                    font.bold: true
                    color: root.scheme.muted
                    topPadding: Theme.spaceOf(2)
                    bottomPadding: Theme.spaceOf(2)
                    elide: Text.ElideRight
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
