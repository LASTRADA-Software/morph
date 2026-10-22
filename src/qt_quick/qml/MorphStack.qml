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
    // Across the axis (a `ui::Align` index, which C++ also writes on each child as its `crossAlign`) and along it
    // (a `ui::Justify` index: 0 start, 1 center, 2 end, 3 between).
    property int align: 0
    property int justify: 0
    readonly property Item content: layout

    // Under `between` the layout's spacing comes from the stack's length, so its implicit length along the axis would
    // feed that length back to a parent that sizes the stack from it, growing on every pass; the stack's own natural
    // length is the children's and the gaps' instead.
    implicitWidth: root.axis === 1 && root.between ? root.naturalLength : layout.implicitWidth
    implicitHeight: root.axis === 0 && root.between ? root.naturalLength : layout.implicitHeight

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

    GridLayout {
        id: layout
        x: root.axis === 1 && root.packed ? root.leadOf(root.width - width) : 0
        y: root.axis === 0 && root.packed ? root.leadOf(root.height - height) : 0
        width: root.axis === 1 && root.packed ? implicitWidth : root.width
        height: root.axis === 0 && root.packed ? implicitHeight : root.height
        flow: root.axis === 0 ? GridLayout.TopToBottom : GridLayout.LeftToRight
        rowSpacing: root.axis === 0 && root.between ? root.betweenSpacing : root.gapLength
        columnSpacing: root.axis === 1 && root.between ? root.betweenSpacing : root.gapLength
    }
}
