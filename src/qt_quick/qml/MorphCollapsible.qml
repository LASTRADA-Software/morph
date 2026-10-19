// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A section with a title, a header beside it that stays usable while closed, and content shown only while open.
// Its control asks to open or close it, and reports the state asked for.
MorphWidget {
    id: root

    property string title: ""
    property bool open: true
    readonly property Item content: body
    readonly property Item headerArea: headerSlot

    focusTarget: toggle
    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.fill: parent

        RowLayout {
            ToolButton {
                font: Theme.bodyFont
                id: toggle
                objectName: "toggle"
                text: (root.open ? "▾ " : "▸ ") + root.title
                onClicked: {
                    root.open = !root.open
                    root.fire("toggle", root.open)
                }
            }

            RowLayout {
                id: headerSlot
            }
        }

        ColumnLayout {
            id: body
            Layout.fillWidth: true
            visible: root.open
        }
    }
}
