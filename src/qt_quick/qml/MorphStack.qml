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
    // The spacing steps the children are inset by above and below, and at the sides.
    property int paddingV: 0
    property int paddingH: 0
    readonly property real insetY: Theme.spaceOf(root.paddingV)
    readonly property real insetX: Theme.spaceOf(root.paddingH)
    // Whether the stack can be the chosen entry of a list (C++ sets it with the first `selected`), and whether it is:
    // a selected one draws the accent's bar at its leading edge and is announced as a selected group.
    property bool selectable: false
    property bool selected: false
    // Across the axis (a `ui::Align` index, which C++ also writes on each child as its `crossAlign`) and along it
    // (a `ui::Justify` index: 0 start, 1 center, 2 end, 3 between).
    property int align: 0
    property int justify: 0
    // Whether the row's children continue on the next line when they do not fit; C++ sets it before it adopts any
    // child, so every child lands in the content the stack lays out with.
    property bool wrap: false
    // Whether the stack is the column the interpreter inserts around a list of nodes: a child that bleeds then reaches
    // past the padding of the stack's own container, so the stack passes that padding on besides its own.
    property bool passesBleed: false
    // Whether a 1 px rule in the scheme's divider colour is drawn between each two shown children of a column; the
    // rule takes 1 px of its own, so the children are `gap` plus that apart.
    property bool divided: false
    // Whether the stack is a row of actions (an overlay's footer, an action bar): its children are then the theme's
    // action gap apart, which is spacing step 2 while the theme sets none.
    property bool actionRow: false
    // The space between two children before the divider: the action gap of a row of actions, else the `gap` step.
    readonly property real stepLength: root.actionRow ? Theme.actionSpacing : Theme.spaceOf(root.gap)
    readonly property Item content: wrap ? free : layout

    // Under `between` the layout's spacing comes from the stack's length, so its implicit length along the axis would
    // feed that length back to a parent that sizes the stack from it, growing on every pass; the stack's own natural
    // length is the children's and the gaps' instead. The same holds while a child's width is a share of the stack's,
    // and while a child stretches along the axis: the layout's implicit length sums the shares, which follow the
    // length a parent derives from this one.
    implicitWidth: 2 * root.insetX + (root.wrap ? root.wrappedNaturalWidth
                                                : root.axis === 1 && (root.between || root.hasShare
                                                                      || root.stretchesAlong)
                                                  ? root.naturalLength : layout.implicitWidth)
    implicitHeight: 2 * root.insetY + (root.wrap ? free.implicitHeight
                                                : root.axis === 0 && (root.between || root.stretchesAlong)
                                                  ? root.naturalLength : layout.implicitHeight)

    // The widest a wrapping row's child ever needs: a wrapping row can be as narrow as that. A child that can be
    // narrower than its basis (a text that wraps, a stack of them) counts as that.
    readonly property real widestMinimum: {
        let widest = 0
        for (let i = 0; i < free.children.length; ++i) {
            const cell = free.children[i]
            if (cell.visible)
                widest = Math.max(widest, root.minimumBasisOf(cell))
        }
        return widest
    }
    // How narrow the stack can be laid out: a wrapping row, as narrow as its widest child; a column, the same; a row
    // that does not wrap, its children side by side.
    shrinkWidth: 2 * root.insetX + (root.wrap ? root.widestMinimum : root.shrunkContent)
    readonly property real shrunkContent: {
        let along = 0
        let across = 0
        let shown = 0
        for (const object of layout.children) {
            const child = object as Item
            if (!child || !child.visible)
                continue
            const least = root.minimumBasisOf(child)
            along += least
            across = Math.max(across, least)
            ++shown
        }
        return root.axis === 1 ? along + root.gapLength * Math.max(0, shown - 1) : across
    }
    // The width the children ask for however wide the stack is laid out, or -1 while none of them has such a width
    // (their own implicit widths are then the stack's): a row's children together, as `naturalLength` counts them,
    // a column takes the widest, with the gaps and the padding.
    widthFreeNatural: {
        if (root.wrap)
            return -1
        let given = false
        let across = 0
        for (const object of root.content.children) {
            const child = object as MorphWidget
            if (!child || !child.visible)
                continue
            const free = child.widthFreeNatural
            given = given || free >= 0
            across = Math.max(across, free >= 0 ? free : child.implicitWidth)
        }
        if (!given)
            return -1
        return 2 * root.insetX + (root.axis === 1 ? root.naturalAlong(child => {
                                                        const free = (child as MorphWidget)?.widthFreeNatural ?? -1
                                                        return free >= 0 ? free : child.implicitWidth
                                                    }) : across)
    }
    // The content's minimum across the width, and the width there is inside the padding.
    needWidth: root.wrap ? root.widestMinimum : root.axis === 1 && root.between ? root.naturalLength
                                                                       : layout.Layout.minimumWidth
    haveWidth: root.width - 2 * root.insetX
    contentMinimumWidth: 2 * root.insetX + root.needWidth
    overflowKind: root.axis === 1 ? "row" : "column"

    // The widest child whose minimum alone exceeds the width inside the padding, or empty.
    function overflowChild(): string {
        let found = ""
        let widest = root.haveWidth
        for (const object of root.content.children) {
            const child = object as MorphWidget
            if (!child || !child.visible)
                continue
            const least = root.wrap ? root.minimumBasisOf(child) : child.Layout.minimumWidth
            if (least > widest) {
                widest = least
                found = child.objectName
            }
        }
        return found
    }

    defaultRole: root.selectable ? Accessible.Grouping : Accessible.Client
    Accessible.selectable: root.selectable
    Accessible.selected: root.selected

    onWidthChanged: place()
    onWrapChanged: place()
    onGapChanged: place()
    onPaddingHChanged: place()
    onAlignChanged: place()
    onJustifyChanged: place()

    // Whether a shown child stretches along the axis: its sizing there is `stretch`, or it is a content-sized spacer,
    // which counts as a stretch of weight 1. @p child is untyped because only a spacer declares `expandsByDefault`.
    function stretchesIn(child: var): bool {
        if (!child || !child.visible)
            return false
        const kind = root.axis === 0 ? child.heightKind : child.widthKind
        return kind === 2 || (kind === 0 && child["expandsByDefault"] === true)
    }
    // A stretching child's weight along the axis: its amount, at least 1; a spacer's is 1.
    function weightIn(child: var): real {
        const kind = root.axis === 0 ? child.heightKind : child.widthKind
        return kind === 2 ? Math.max(1, root.axis === 0 ? child.heightAmount : child.widthAmount) : 1
    }
    // Whether a shown child stretches along the axis, or a row's child takes a share of its spare width. Without
    // one, the layout keeps its natural length on the axis and packs the children at the start: given more, a layout
    // would hand the spare length to the gaps between them.
    readonly property bool stretchesAlong: {
        for (const child of layout.children) {
            if (root.stretchesIn(child) || (root.axis === 1 && child.visible && child.takesShare))
                return true
        }
        return false
    }

    // Packed along the axis at its natural length, where `justify` puts it; for `between`, at full length with the
    // spare length in the spacing: a layout given more length would hand it to the cells, not the gaps.
    readonly property bool packed: !root.stretchesAlong && root.justify !== 3
    readonly property bool between: !root.stretchesAlong && root.justify === 3
    // The shown children's lengths along the axis, as the layout sizes a child that does not stretch. A child that is
    // a share of the stack's width counts at that share, where the spare length is divided.
    readonly property var shownLengths: root.lengthsOf(true)
    // Whether a shown child along a row is a share of the row's width.
    readonly property bool hasShare: {
        if (root.axis !== 1)
            return false
        for (const object of layout.children) {
            const child = object as MorphWidget
            if (child && child.visible && child.widthKind === 3)
                return true
        }
        return false
    }
    function lengthsOf(useShare: bool): var {
        const lengths = []
        for (const object of layout.children) {
            // `children` is a list of QtObject; the Layout attached type needs an Item.
            const child = object as Item
            if (!child || !child.visible)
                continue
            lengths.push(root.lengthOf(child, useShare))
        }
        return lengths
    }
    // A shown child's length along the axis as the layout sizes it, within its bounds. A stretching child's
    // preferred length is its share, which follows this stack's length, so it is never read here.
    function lengthOf(child: Item, useShare: bool): real {
        const widget = child as MorphWidget
        const shared = root.axis === 1 && widget && widget.widthKind === 3
        // A share's preferred width is not read for the natural length: it follows this stack's width, which a
        // parent may derive from the natural length.
        const preferred = (shared && !useShare) || root.stretchesIn(child) ? -1
                          : root.axis === 0 ? child.Layout.preferredHeight : child.Layout.preferredWidth
        const implicit = root.axis === 0 ? child.implicitHeight : child.implicitWidth
        // A share's floor follows this stack's width, so the natural length reads the minimum without it.
        const least = shared && !useShare ? widget.baseMinimumWidth
                      : root.axis === 0 ? child.Layout.minimumHeight : child.Layout.minimumWidth
        return Math.min(Math.max(preferred >= 0 ? preferred : implicit, Math.max(0, least)), root.mostOf(child))
    }
    // A child's maximum length along the axis; unbounded while it has none.
    function mostOf(child: Item): real {
        const most = root.axis === 0 ? child.Layout.maximumHeight : child.Layout.maximumWidth
        return most >= 0 ? most : Infinity
    }
    // A child's minimum length along the axis.
    function leastOf(child: Item): real {
        return Math.max(0, root.axis === 0 ? child.Layout.minimumHeight : child.Layout.minimumWidth)
    }
    // Whether the stack is the shell's expanded head bar, whose gap the theme's `head.paddingY` sets while it is
    // above 0: the bar has no other gap, so the token is the space above and below its line.
    property bool headBar: false
    readonly property real gapLength: (root.headBar && Theme.headPaddingY > 0 ? Theme.headPaddingY
                                                                               : root.stepLength) +
                                      (root.divided && !root.wrap ? 1 : 0)
    // What the stack asks of its parent along the axis: the children's natural lengths and the gaps, where the
    // stretching children together ask for what lets each hold its natural.
    readonly property real naturalLength: root.naturalAlong(child => root.axis === 0 ? child.implicitHeight
                                                                                        : child.implicitWidth)

    // The shown children's natural lengths along the axis, and the gaps: a child that does not stretch counts as
    // the layout sizes it, and the stretching children, which share what is left by weight, ask for
    // `W * max(n / w)` together, where `n` is a child's natural within its bounds and `w` its weight, and `W` the
    // sum of the weights, so that each share is at least its child's natural. With one stretching child that is its
    // natural. @p natural gives a stretching child's natural along the axis.
    function naturalAlong(natural: var): real {
        let others = 0
        let weights = 0
        let ratio = 0
        let shown = 0
        for (const object of layout.children) {
            const child = object as Item
            if (!child || !child.visible)
                continue
            ++shown
            if (root.stretchesIn(child)) {
                const weight = root.weightIn(child)
                const basis = Math.min(Math.max(natural(child), root.leastOf(child)), root.mostOf(child))
                weights += weight
                ratio = Math.max(ratio, basis / weight)
            } else {
                others += root.lengthOf(child, false)
            }
        }
        return others + weights * ratio + root.gapLength * Math.max(0, shown - 1)
    }

    // The length each shown child along the axis has, aligned with the layout's children: -1 for a child that does not
    // stretch, else its share. The stretching children share what the fixed, percent and content children and the
    // gaps leave of the length inside the padding, by weight, from zero. A wrapping row has none.
    readonly property var stretchShares: {
        const shares = []
        if (root.wrap)
            return shares
        const members = []
        const owners = []
        let used = 0
        let shown = 0
        let index = 0
        for (const object of layout.children) {
            const child = object as Item
            shares.push(-1)
            if (child && child.visible) {
                ++shown
                if (root.stretchesIn(child)) {
                    members.push({"weight": root.weightIn(child), "minimum": root.leastOf(child),
                                     "maximum": root.mostOf(child)})
                    owners.push(index)
                } else {
                    used += root.lengthOf(child, true)
                }
            }
            ++index
        }
        const length = root.axis === 0 ? root.height - 2 * root.insetY : root.width - 2 * root.insetX
        const given = root.shareByWeight(members, length - used - root.gapLength * Math.max(0, shown - 1))
        for (let i = 0; i < owners.length; ++i)
            shares[owners[i]] = given[i]
        return shares
    }
    // The share of @p child along @p along (0 down a column, 1 along a row), or -1 when this stack does not size the
    // child that way.
    function shareOf(child: Item, along: int): real {
        if (along !== root.axis)
            return -1
        let index = 0
        for (const object of layout.children) {
            if (object === child)
                return index < root.stretchShares.length ? root.stretchShares[index] : -1
            ++index
        }
        return -1
    }
    // Shares @p room among @p members, which hold a `weight`, a `minimum` and a `maximum`, by weight from zero. A
    // share outside its bounds is held at the bound and the rest is shared again among the others (the bounds of a
    // flex layout): each pass shares the room among the open members and clamps; when the clamps added length in
    // total the members raised to their minimum are held, when they removed length the members cut to their maximum
    // are, and when they changed nothing every member is. A pass holds at least one member, so there are at most as
    // many passes as members.
    function shareByWeight(members: var, room: real): var {
        const lengths = members.map(() => 0)
        let open = members.map((member, i) => i)
        let spare = room
        while (open.length > 0) {
            let weights = 0
            for (const i of open)
                weights += members[i].weight
            const clamped = {}
            let added = 0
            for (const i of open) {
                const raw = Math.max(0, spare) * members[i].weight / weights
                clamped[i] = Math.min(Math.max(raw, members[i].minimum), members[i].maximum)
                added += clamped[i] - raw
            }
            const frozen = []
            for (const i of open) {
                const raw = Math.max(0, spare) * members[i].weight / weights
                if (Math.abs(added) < 1e-6 || (added > 0 ? clamped[i] > raw : clamped[i] < raw))
                    frozen.push(i)
            }
            for (const i of frozen) {
                lengths[i] = clamped[i]
                spare -= clamped[i]
            }
            open = open.filter(i => !frozen.includes(i))
        }
        return lengths
    }
    readonly property real betweenSpacing: {
        const count = root.shownLengths.length
        if (count < 2)
            return root.gapLength
        const used = root.shownLengths.reduce((sum, length) => sum + length, 0)
        const length = root.axis === 0 ? root.height - 2 * root.insetY : root.width - 2 * root.insetX
        return Math.max(root.gapLength, (length - used) / (count - 1))
    }

    // A wrapping row's natural width: its shown children's one-line widths and the gaps between those with a basis. A
    // binding on the children alone, not a value `place` writes, so asking a row for its width never lays it out, and
    // the width it was last laid out at has no part in it.
    readonly property real wrappedNaturalWidth: {
        if (!root.wrap)
            return 0
        const space = root.stepLength
        let natural = 0
        for (let i = 0; i < free.children.length; ++i) {
            const cell = free.children[i]
            if (!cell.visible)
                continue
            if (natural > 0 && root.naturalBasisOf(cell) > 0)
                natural += space
            natural += root.naturalWidth(cell)
        }
        return natural
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

    // A child's basis along a wrapping row, whatever the row's width: a stretch child's minimum width, no more than its
    // natural width when it has one (it needs no more room than it fills), else its natural width, within its bounds.
    function naturalBasisOf(cell: Item): real {
        const widget = cell as MorphWidget
        let basis = root.naturalWidth(cell)
        if (widget && widget.widthKind === 2) {
            const least = widget.minWidthUnits * Theme.unitWidth
            basis = widget.minWidthUnits < 0 ? cell.implicitWidth
                                             : cell.implicitWidth > 0 ? Math.min(least, cell.implicitWidth) : least
            if (widget.maxWidthUnits >= 0)
                basis = Math.min(basis, widget.maxWidthUnits * Theme.unitWidth)
        }
        return basis
    }

    // A child's basis or, where it can be narrower, the narrowest it can be.
    function minimumBasisOf(cell: Item): real {
        const basis = root.naturalBasisOf(cell)
        const widget = cell as MorphWidget
        return widget && widget.shrinkWidth >= 0 ? Math.min(basis, widget.shrinkWidth) : basis
    }

    // The same as the basis, at most the row's width.
    function basisOf(cell: Item): real {
        return Math.min(root.naturalBasisOf(cell), root.width - 2 * root.insetX)
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
        const space = root.stepLength
        const lineWidth = root.width - 2 * root.insetX
        const cells = []
        for (let i = 0; i < free.children.length; ++i) {
            const cell = free.children[i]
            if (!cell.visible)
                continue
            const basis = root.basisOf(cell)
            cells.push({"item": cell, "basis": basis, "width": basis})
        }
        if (lineWidth <= 0) {
            free.implicitHeight = 0
            return
        }

        const lines = cells.length > 0 ? [{"cells": [], "used": 0}] : []
        for (const placed of cells) {
            let line = lines[lines.length - 1]
            const gap = line.used > 0 && placed.basis > 0 ? space : 0
            if (placed.basis > 0 && line.used > 0 && line.used + gap + placed.basis > lineWidth) {
                line = {"cells": [], "used": 0}
                lines.push(line)
            }
            line.used += (line.used > 0 && placed.basis > 0 ? space : 0) + placed.basis
            line.cells.push(placed)
        }

        let y = 0
        for (let l = 0; l < lines.length; ++l) {
            const members = lines[l].cells
            let spare = Math.max(0, lineWidth - lines[l].used)
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

    // The shown children, in order: what the rules sit between.
    readonly property var shownItems: {
        const items = []
        for (const object of layout.children) {
            const child = object as Item
            if (child && child.visible)
                items.push(child)
        }
        return items
    }

    // The selection's bar: 3 px at the leading edge down the whole height, over the surface's background (z -1) and
    // under the children, with the surface's own leading corners.
    Rectangle {
        objectName: "selectionBar"
        z: -0.5
        visible: root.selected
        x: root.LayoutMirroring.enabled ? root.width - width : 0
        width: 3
        height: root.height
        color: root.scheme.accent
        topLeftRadius: root.LayoutMirroring.enabled ? 0 : root.surfaceRadius
        bottomLeftRadius: root.LayoutMirroring.enabled ? 0 : root.surfaceRadius
        topRightRadius: root.LayoutMirroring.enabled ? root.surfaceRadius : 0
        bottomRightRadius: root.LayoutMirroring.enabled ? root.surfaceRadius : 0
    }

    // A rule after each shown child but the last, centred in the gap that is left of the rule's own pixel: a
    // horizontal rule below a column's child, a vertical one beside a row's.
    Repeater {
        model: root.divided && !root.wrap ? root.shownItems : []
        delegate: Rectangle {
            required property Item modelData
            required property int index
            parent: root
            visible: index < root.shownItems.length - 1
            x: root.axis === 0 ? 0 : layout.x + modelData.x + modelData.width + (root.gapLength - 1) / 2
            y: root.axis === 0 ? layout.y + modelData.y + modelData.height + (root.gapLength - 1) / 2 : layout.y
            width: root.axis === 0 ? root.width : 1
            height: root.axis === 0 ? 1 : layout.height
            color: root.scheme.divider
        }
    }

    Item {
        id: free
        anchors.fill: parent
        anchors.leftMargin: root.insetX
        anchors.rightMargin: root.insetX
        anchors.topMargin: root.insetY
        anchors.bottomMargin: root.insetY
        visible: root.wrap
        onChildrenChanged: root.place()
    }

    GridLayout {
        id: layout
        // What a bleeding child reaches past: the stack's horizontal padding, and the container's it passes on.
        readonly property real childBleedInset: root.insetX + (root.passesBleed ? root.bleedInset : 0)
        visible: !root.wrap
        x: root.insetX + (root.axis === 1 && root.packed ? root.leadOf(root.width - 2 * root.insetX - width) : 0)
        y: root.insetY + (root.axis === 0 && root.packed ? root.leadOf(root.height - 2 * root.insetY - height) : 0)
        width: root.axis === 1 && root.packed ? implicitWidth : root.width - 2 * root.insetX
        height: root.axis === 0 && root.packed ? implicitHeight : root.height - 2 * root.insetY
        flow: root.axis === 0 ? GridLayout.TopToBottom : GridLayout.LeftToRight
        rowSpacing: root.axis === 0 && root.between ? root.betweenSpacing : root.gapLength
        columnSpacing: root.axis === 1 && root.between ? root.betweenSpacing : root.gapLength
    }
}
