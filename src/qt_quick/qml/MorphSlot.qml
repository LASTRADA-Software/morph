// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Where a switch case or a tabs page is shown: its children as they are.
MorphWidget {
    id: root

    readonly property Item content: column

    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    // Whether a shown child stretches vertically. Without one, the column keeps its natural height and packs the
    // children at the top: given more, a layout would hand the spare height to the space between them.
    readonly property bool stretchesDown: {
        for (const child of column.children) {
            if (child.visible && child.heightKind === 2)
                return true
        }
        return false
    }

    ColumnLayout {
        id: column
        width: root.width
        height: root.stretchesDown ? root.height : implicitHeight
        spacing: 0
    }
}
