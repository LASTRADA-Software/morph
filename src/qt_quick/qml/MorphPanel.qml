// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A framed region with a title and `padding` (a spacing step) around its content. A collapsible panel is an accordion section:
// its header button asks to collapse or expand it, and a collapsed panel hides its content.
MorphWidget {
    id: root

    property string title: ""
    property int padding: 0
    property bool collapsible: false
    property bool collapsed: false
    readonly property Item content: column

    implicitWidth: frame.implicitWidth
    implicitHeight: frame.implicitHeight

    Rectangle {
        id: frame
        anchors.fill: parent
        implicitWidth: inner.implicitWidth + 2 * Theme.spaceOf(root.padding) + 2
        implicitHeight: inner.implicitHeight + 2 * Theme.spaceOf(root.padding) + 2
        // A panel is drawn as a card on its scheme's surface.
        color: root.scheme.surface
        radius: Theme.radiusCard
        border.color: root.scheme.border

        ColumnLayout {
            id: inner
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceOf(root.padding) + 1
            anchors.rightMargin: Theme.spaceOf(root.padding) + 1
            anchors.topMargin: Theme.spaceOf(root.padding) + 1
            anchors.bottomMargin: Theme.spaceOf(root.padding) + 1

            RowLayout {
                visible: root.title !== "" || root.collapsible

                ToolButton {
                    font: Theme.bodyFont
                    id: toggle
                    visible: root.collapsible
                    icon.source: Theme.iconSource(root.collapsed ? "nav.expand" : "nav.collapse")
                    icon.color: "transparent"
                    text: icon.source.toString() !== "" ? "" : root.collapsed ? "▸" : "▾"
                    onClicked: {
                        root.collapsed = !root.collapsed
                        root.fire("toggle", root.collapsed)
                    }
                }

                Label {
                    font: Theme.strongFont
                    text: root.title
                }
            }

            ColumnLayout {
                id: column
                visible: !root.collapsible || !root.collapsed
                Layout.fillWidth: true
            }
        }
    }
}
