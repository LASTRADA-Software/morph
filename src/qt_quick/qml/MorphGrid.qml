// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A grid: children row-major in `columns` columns, `gap` units apart; a child's span widens it.
MorphWidget {
    id: root

    property int columns: 1
    property int gap: 0
    readonly property Item content: layout

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight

    GridLayout {
        id: layout
        anchors.fill: parent
        columns: Math.max(1, root.columns)
        rowSpacing: root.gap * Theme.unitHeight
        columnSpacing: root.gap * Theme.unitWidth
    }
}
