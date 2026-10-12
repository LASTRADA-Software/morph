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
    readonly property Item content: split
    readonly property real unit: root.axis === 0 ? Theme.unitHeight : Theme.unitWidth

    function apply() {
        const panes = split.contentChildren
        const given = root.sizes && root.sizes.length === panes.length
        const total = root.axis === 0 ? split.height : split.width
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

    onSizesChanged: apply()
    implicitWidth: split.implicitWidth
    implicitHeight: split.implicitHeight

    SplitView {
        id: split
        anchors.fill: parent
        orientation: root.axis === 0 ? Qt.Vertical : Qt.Horizontal
        onResizingChanged: {
            if (!resizing)
                root.fire("resize", root.currentSizes())
        }
    }
}
