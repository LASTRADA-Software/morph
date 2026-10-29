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

    // Whether a shown body child stretches down; the body then takes the spare height, else a filler does, which keeps
    // the body under the header instead of centred in the spare height.
    readonly property bool bodyStretches: {
        for (const child of body.children) {
            if (child.visible && child.heightKind === 2)
                return true
        }
        return false
    }

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
                readonly property string chevron: root.open ? "nav.collapse" : "nav.expand"
                icon.source: Theme.iconSource(chevron)
                icon.color: "transparent"
                text: (icon.source.toString() !== "" ? "" : root.open ? "▾ " : "▸ ") + root.title
                contentItem: MorphPlainContent {
                    control: toggle
                    color: toggle.palette.windowText
                }
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
            Layout.fillHeight: root.bodyStretches
            visible: root.open
        }

        Item {
            Layout.fillHeight: true
            visible: !root.bodyStretches
        }
    }
}
