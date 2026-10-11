// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A column or a row: children in order along the axis, `gap` units apart.
MorphWidget {
    id: root

    // 0 vertical, 1 horizontal; fixed at creation.
    property int axis: 0
    property int gap: 0
    readonly property Item content: layout

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight

    GridLayout {
        id: layout
        anchors.fill: parent
        flow: root.axis === 0 ? GridLayout.TopToBottom : GridLayout.LeftToRight
        rowSpacing: root.gap * Theme.unitHeight
        columnSpacing: root.gap * Theme.unitWidth
    }
}
