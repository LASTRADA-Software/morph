// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A column or a row: children in order along the axis, `gap` (a spacing step) apart.
MorphWidget {
    id: root

    // 0 vertical, 1 horizontal; fixed at creation.
    property int axis: 0
    property int gap: 0
    readonly property Item content: layout

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight

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

    GridLayout {
        id: layout
        width: root.axis === 1 && !root.stretchesAlong ? implicitWidth : root.width
        height: root.axis === 0 && !root.stretchesAlong ? implicitHeight : root.height
        flow: root.axis === 0 ? GridLayout.TopToBottom : GridLayout.LeftToRight
        rowSpacing: Theme.spaceOf(root.gap)
        columnSpacing: Theme.spaceOf(root.gap)
    }
}
