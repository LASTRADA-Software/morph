// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// The steps of a process in order, each marked with its state; the current one is highlighted.
MorphWidget {
    id: root

    // [{label, state (0 pending, 1 active, 2 done, 3 failed)}].
    property var steps: []
    property int current: -1

    function marker(state: int): string {
        switch (state) {
        case 1:
            return "●"
        case 2:
            return "✓"
        case 3:
            return "✗"
        default:
            return "○"
        }
    }

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    Row {
        id: row
        spacing: 2 * Theme.unitWidth

        Repeater {
            model: root.steps

            Label {
                required property int index
                required property var modelData
                text: root.marker(modelData.state) + " " + modelData.label
                font.bold: index === root.current
                color: modelData.state === 3 ? root.scheme.error : root.scheme.text
            }
        }
    }
}
