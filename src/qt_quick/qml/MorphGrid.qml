// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A grid: children row-major in `columns` columns, `gap` (a spacing step) apart; a child's span widens it.
MorphWidget {
    id: root

    property int columns: 1
    property int gap: 0
    // Where each cell's content sits in its row, a `ui::Align` index, which C++ also writes on each child as its
    // `crossAlign`.
    property int align: 0
    readonly property Item content: layout

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight

    GridLayout {
        id: layout
        anchors.fill: parent
        columns: Math.max(1, root.columns)
        rowSpacing: Theme.spaceOf(root.gap)
        columnSpacing: Theme.spaceOf(root.gap)
    }
}
