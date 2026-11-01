// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// Panes side by side, or one above the other, with handles between them. Sizes are in units along the axis; an
// empty list, or one whose length is not the pane count, shares the space equally. A drag reports every pane's
// size once the user lets go.
MorphWidget {
    id: root

    // 0 vertical (panes top to bottom), 1 horizontal; fixed at creation.
    property int axis: 1
    property var sizes: []
    // Whether the user has dragged a handle: from then on the panes keep the sizes the user gave them.
    property bool userSized: false
    readonly property Item content: split
    readonly property real unit: root.axis === 0 ? Theme.unitHeight : Theme.unitWidth

    function apply() {
        const panes = split.contentChildren
        const given = root.sizes && root.sizes.length === panes.length
        const total = root.axis === 0 ? split.height : split.width
        // Without sizes, a content-sized splitter keeps each pane at its own size: an equal share of the panes' total
        // would cut the widest one short.
        const along = root.axis === 0 ? root.heightKind : root.widthKind
        if (!given && (total <= 0 || along === 0))
            return
        if (!given && root.axis === 1) {
            root.applyOwn(panes, total)
            return
        }
        for (let i = 0; i < panes.length; ++i) {
            const size = given ? root.sizes[i] * root.unit : total / Math.max(1, panes.length)
            // Sizes, or an equal share, leave no pane to fill; a pane that did before stops.
            panes[i].SplitView.fillWidth = false
            if (root.axis === 0)
                panes[i].SplitView.preferredHeight = size
            else
                panes[i].SplitView.preferredWidth = size
        }
    }

    // @p size held within the width bounds @p pane gives.
    function boundedWidth(pane: var, size: real): real {
        let bounded = size
        if (pane.minWidthUnits >= 0)
            bounded = Math.max(bounded, pane.minWidthUnits * root.unit)
        if (pane.maxWidthUnits >= 0)
            bounded = Math.min(bounded, pane.maxWidthUnits * root.unit)
        return bounded
    }

    // Without sizes, a horizontal splitter places each pane by its own width: a fixed one in units, a percent one as a
    // share of the splitter's width, and the rest, a content pane counting as a stretch of weight 1, share what is
    // left by weight. The heaviest of those fills, so rounding never leaves a gap.
    function applyOwn(panes: var, total: real) {
        const fixedSize = []
        const weightOf = []
        let taken = Math.max(0, panes.length - 1) * root.handleExtent
        let weights = 0
        let heaviest = -1
        let heaviestWeight = 0
        for (let i = 0; i < panes.length; ++i) {
            const kind = panes[i].widthKind ?? 0
            const amount = panes[i].widthAmount ?? 0
            if (kind === 1 || kind === 3) {
                const size = root.boundedWidth(panes[i], kind === 1 ? amount * root.unit : amount / 100 * total)
                fixedSize.push(size)
                weightOf.push(0)
                taken += size
            } else {
                const weight = kind === 2 ? Math.max(1, amount) : 1
                fixedSize.push(-1)
                weightOf.push(weight)
                weights += weight
                if (weight > heaviestWeight) {
                    heaviest = i
                    heaviestWeight = weight
                }
            }
        }
        const left = Math.max(0, total - taken)
        for (let i = 0; i < panes.length; ++i) {
            const size = fixedSize[i] >= 0 ? fixedSize[i]
                                           : root.boundedWidth(panes[i], left * weightOf[i] / Math.max(1, weights))
            panes[i].SplitView.fillWidth = i === heaviest
            panes[i].SplitView.preferredWidth = size
        }
    }

    function currentSizes(): var {
        const out = []
        const panes = split.contentChildren
        for (let i = 0; i < panes.length; ++i)
            out.push(Math.round((root.axis === 0 ? panes[i].height : panes[i].width) / root.unit))
        return out
    }

    // The handle's part, `MorphSplitterHandle` or its restyle, which C++ passes at creation; the split view makes one
    // per gap.
    property Component handle: null
    // The first handle the split view made, or null while there is none.
    property Item handleItem: null
    // The room a handle takes between two panes: its implicit extent along the axis.
    readonly property real handleExtent: root.handleItem ? (root.axis === 0 ? root.handleItem.implicitHeight
                                                                             : root.handleItem.implicitWidth) : 0

    // Tells each handle the split view made its axis and scheme. The handles are the split view's own children that
    // declare `axis`; the panes are in its content item. Neither `contentItem` nor `background` is read to tell them
    // apart: reading a deferred one makes it, which adds a child and calls this again.
    function dressHandles() {
        let first = null
        for (let i = 0; i < split.children.length; ++i) {
            const child = split.children[i]
            if (!("axis" in child))
                continue
            child.axis = root.axis
            child.scheme = Qt.binding(() => root.scheme)
            if (first === null)
                first = child
        }
        root.handleItem = first
    }

    // What the panes ask for: along the axis, each pane's size in units when `sizes` matches the pane count and its
    // implicit extent otherwise, plus the handles between them; across, the largest pane's implicit extent. A
    // SplitView's own implicit size is 0x0, so a splitter sized `content` would otherwise have no area.
    readonly property var paneExtents: {
        const panes = split.contentChildren
        const given = root.sizes && root.sizes.length === panes.length
        let along = Math.max(0, panes.length - 1) * root.handleExtent
        let across = 0
        for (let i = 0; i < panes.length; ++i) {
            const pane = panes[i]
            along += given ? root.sizes[i] * root.unit : (root.axis === 0 ? pane.implicitHeight : pane.implicitWidth)
            across = Math.max(across, root.axis === 0 ? pane.implicitWidth : pane.implicitHeight)
        }
        return [along, across]
    }

    // What the panes' own sizing comes to; applying again when it changes keeps a pane sized by a late `setLayout`.
    readonly property string paneSizing: {
        const keys = []
        for (const pane of split.contentChildren)
            keys.push([pane.widthKind, pane.widthAmount, pane.minWidthUnits, pane.maxWidthUnits].join(","))
        return keys.join(";")
    }
    onPaneSizingChanged: {
        if (!root.userSized)
            root.apply()
    }
    onSizesChanged: apply()
    implicitWidth: root.axis === 0 ? paneExtents[1] : paneExtents[0]
    implicitHeight: root.axis === 0 ? paneExtents[0] : paneExtents[1]

    SplitView {
        id: split
        anchors.fill: parent
        orientation: root.axis === 0 ? Qt.Vertical : Qt.Horizontal
        handle: root.handle
        onChildrenChanged: root.dressHandles()
        // The share is re-applied as the split view's length changes, until the user drags: panes are adopted while
        // the splitter is still small, and a share applied then would stay that small.
        onWidthChanged: {
            if (root.axis === 1 && !root.userSized)
                root.apply()
        }
        onHeightChanged: {
            if (root.axis === 0 && !root.userSized)
                root.apply()
        }
        onResizingChanged: {
            if (!resizing) {
                root.userSized = true
                root.fire("resize", root.currentSizes())
            }
        }
    }
}
