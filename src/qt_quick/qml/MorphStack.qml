// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A column or a row: children in order along the axis, `gap` (a spacing step) apart. With `wrap` (a row only), the
// children fill lines in order and `place` lays them out instead of a GridLayout.
MorphWidget {
    id: root

    // 0 vertical, 1 horizontal; fixed at creation.
    property int axis: 0
    property int gap: 0
    // Across the axis (a `ui::Align` index, which C++ also writes on each child as its `crossAlign`) and along it
    // (a `ui::Justify` index: 0 start, 1 center, 2 end, 3 between).
    property int align: 0
    property int justify: 0
    // Whether the row's children continue on the next line when they do not fit; C++ sets it before it adopts any
    // child, so every child lands in the content the stack lays out with.
    property bool wrap: false
    readonly property Item content: wrap ? free : layout

    // Under `between` the layout's spacing comes from the stack's length, so its implicit length along the axis would
    // feed that length back to a parent that sizes the stack from it, growing on every pass; the stack's own natural
    // length is the children's and the gaps' instead.
    implicitWidth: root.wrap ? free.implicitWidth
                             : root.axis === 1 && root.between ? root.naturalLength : layout.implicitWidth
    implicitHeight: root.wrap ? free.implicitHeight
                              : root.axis === 0 && root.between ? root.naturalLength : layout.implicitHeight

    onWidthChanged: place()
    onWrapChanged: place()
    onGapChanged: place()
    onAlignChanged: place()
    onJustifyChanged: place()

    // Whether a shown child stretches along the axis. Without one, the layout keeps its natural length on the axis
    // and packs the children at the start: given more, a layout would hand the spare length to the gaps between
    // them.
    readonly property bool stretchesAlong: {
        for (const child of layout.children) {
            const along = root.axis === 0 ? child.heightKind : child.widthKind
            if (child.visible && along === 2)
                return true
        }
        return false
    }

    // Packed along the axis at its natural length, where `justify` puts it; for `between`, at full length with the
    // spare length in the spacing: a layout given more length would hand it to the cells, not the gaps.
    readonly property bool packed: !root.stretchesAlong && root.justify !== 3
    readonly property bool between: !root.stretchesAlong && root.justify === 3
    // The shown children's lengths along the axis, as the layout sizes a child that does not stretch.
    readonly property var shownLengths: {
        const lengths = []
        for (const object of layout.children) {
            // `children` is a list of QtObject; the Layout attached type needs an Item.
            const child = object as Item
            if (!child || !child.visible)
                continue
            const preferred = root.axis === 0 ? child.Layout.preferredHeight : child.Layout.preferredWidth
            const implicit = root.axis === 0 ? child.implicitHeight : child.implicitWidth
            const least = root.axis === 0 ? child.Layout.minimumHeight : child.Layout.minimumWidth
            const most = root.axis === 0 ? child.Layout.maximumHeight : child.Layout.maximumWidth
            lengths.push(Math.min(Math.max(preferred >= 0 ? preferred : implicit, least), most))
        }
        return lengths
    }
    readonly property real gapLength: Theme.spaceOf(root.gap)
    readonly property real naturalLength: root.shownLengths.reduce((sum, length) => sum + length, 0)
                                          + root.gapLength * Math.max(0, root.shownLengths.length - 1)
    readonly property real betweenSpacing: {
        const count = root.shownLengths.length
        if (count < 2)
            return root.gapLength
        const used = root.shownLengths.reduce((sum, length) => sum + length, 0)
        const length = root.axis === 0 ? root.height : root.width
        return Math.max(root.gapLength, (length - used) / (count - 1))
    }

    function leadOf(spare: real): real {
        return root.justify === 1 ? Math.max(0, spare) / 2 : root.justify === 2 ? Math.max(0, spare) : 0
    }

    // A child's own extent along one dimension: its fixed amount, else its implicit extent, within its bounds.
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

    // A child's basis along a wrapping row: a stretch child's minimum width, else its natural width, within its
    // bounds and at most the row's width.
    function basisOf(cell: Item): real {
        const widget = cell as MorphWidget
        let basis = root.naturalWidth(cell)
        if (widget && widget.widthKind === 2) {
            basis = widget.minWidthUnits >= 0 ? widget.minWidthUnits * Theme.unitWidth : cell.implicitWidth
            if (widget.maxWidthUnits >= 0)
                basis = Math.min(basis, widget.maxWidthUnits * Theme.unitWidth)
        }
        return Math.min(basis, root.width)
    }

    // Lays a wrapping row out: children fill lines in order, a child starting a new line when its basis, and a gap,
    // does not fit after those already on the line (a line holds one child at least, and a child with no basis takes
    // no gap and never starts a line). On a line the stretch children share the spare width by weight, each stopping
    // at its maximum; a line without one places it by `justify`. A line is as tall as its tallest child, which the
    // row's `align`, or the child's own, places down it; lines are a gap apart. C++ calls it again whenever a
    // child's implicit size or visibility changes.
    function place() {
        if (!root.wrap)
            return
        const space = Theme.spaceOf(root.gap)
        const cells = []
        let natural = 0
        for (let i = 0; i < free.children.length; ++i) {
            const cell = free.children[i]
            if (!cell.visible)
                continue
            const basis = root.basisOf(cell)
            if (natural > 0 && basis > 0)
                natural += space
            natural += root.naturalWidth(cell)
            cells.push({"item": cell, "basis": basis, "width": basis})
        }
        free.implicitWidth = natural
        if (root.width <= 0) {
            free.implicitHeight = 0
            return
        }

        const lines = []
        let line = null
        for (const placed of cells) {
            const gap = line !== null && line.used > 0 && placed.basis > 0 ? space : 0
            if (line === null || (placed.basis > 0 && line.used > 0 && line.used + gap + placed.basis > root.width)) {
                line = {"cells": [], "used": 0}
                lines.push(line)
            }
            line.used += (line.used > 0 && placed.basis > 0 ? space : 0) + placed.basis
            line.cells.push(placed)
        }

        let y = 0
        for (let l = 0; l < lines.length; ++l) {
            const members = lines[l].cells
            let spare = Math.max(0, root.width - lines[l].used)
            // The stretch children share the spare width; one that reaches its maximum leaves the rest to the others.
            let open = members.filter(m => (m.item as MorphWidget) && (m.item as MorphWidget).widthKind === 2
                                           && m.basis > 0)
            while (spare > 0 && open.length > 0) {
                let weights = 0
                for (const m of open)
                    weights += Math.max(1, (m.item as MorphWidget).widthAmount)
                let given = 0
                const stillOpen = []
                for (const m of open) {
                    const widget = m.item as MorphWidget
                    const room = widget.maxWidthUnits >= 0 ? Math.max(0, widget.maxWidthUnits * Theme.unitWidth - m.width)
                                                           : Infinity
                    const share = Math.min(spare * Math.max(1, widget.widthAmount) / weights, room)
                    m.width += share
                    given += share
                    if (share < room)
                        stillOpen.push(m)
                }
                spare -= given
                open = given > 0.01 ? stillOpen : []
            }
            let lead = 0
            let extra = 0
            if (spare > 0) {
                if (root.justify === 1)
                    lead = spare / 2
                else if (root.justify === 2)
                    lead = spare
                else if (root.justify === 3 && members.length > 1)
                    extra = spare / (members.length - 1)
            }

            let x = lead
            let height = 0
            let counted = 0
            for (const m of members) {
                m.item.width = m.width
                m.height = root.naturalHeight(m.item)
                height = Math.max(height, m.height)
            }
            for (const m of members) {
                const cell = m.item
                const widget = cell as MorphWidget
                if (counted > 0 && m.basis > 0)
                    x += space + extra
                const down = widget && widget.alignSelf >= 0 ? widget.alignSelf : root.align
                const fixed = widget && widget.heightKind === 1
                const size = down === 0 && !fixed ? height : Math.min(m.height, height)
                const below = height - size
                cell.x = x
                cell.y = y + (down === 3 ? below : down === 2 ? Math.floor(below / 2) : 0)
                cell.height = size
                x += m.width
                if (m.basis > 0)
                    ++counted
            }
            y += height + (l < lines.length - 1 ? space : 0)
        }
        free.implicitHeight = y
    }

    Item {
        id: free
        anchors.fill: parent
        visible: root.wrap
        onChildrenChanged: root.place()
    }

    GridLayout {
        id: layout
        visible: !root.wrap
        x: root.axis === 1 && root.packed ? root.leadOf(root.width - width) : 0
        y: root.axis === 0 && root.packed ? root.leadOf(root.height - height) : 0
        width: root.axis === 1 && root.packed ? implicitWidth : root.width
        height: root.axis === 0 && root.packed ? implicitHeight : root.height
        flow: root.axis === 0 ? GridLayout.TopToBottom : GridLayout.LeftToRight
        rowSpacing: root.axis === 0 && root.between ? root.betweenSpacing : root.gapLength
        columnSpacing: root.axis === 1 && root.between ? root.betweenSpacing : root.gapLength
    }
}
