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
        for (let i = 0; i < panes.length; ++i) {
            const size = given ? root.sizes[i] * root.unit : total / Math.max(1, panes.length)
            if (root.axis === 0)
                panes[i].SplitView.preferredHeight = size
            else
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
