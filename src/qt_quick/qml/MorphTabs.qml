// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A tab bar over page slots. The mount keeps exactly the selected page's slot visible; the bar's highlight follows
// `select` and the user's clicks. A hidden bar (`barShown` false) gives its height to the pages; a shown one sits a
// spacing step above them.
MorphWidget {
    id: root

    property list<string> labels: []
    property bool barShown: true
    readonly property Item content: pages
    readonly property int highlighted: bar.currentIndex

    // Highlights tab `index`; past the last highlights none.
    function select(index: int) {
        bar.currentIndex = index < bar.count ? index : -1
    }

    // A hidden bar never holds the focus: the workspace's chords must reach a page when a second tab arrives.
    focusTarget: bar.visible ? bar : pages
    implicitWidth: Math.max(bar.visible ? bar.implicitWidth : 0, pages.implicitWidth)
    implicitHeight: (bar.visible ? bar.implicitHeight + layout.spacing : 0) + pages.implicitHeight

    ColumnLayout {
        id: layout
        anchors.fill: parent
        spacing: Theme.spaceOf(3)

        TabBar {
            id: bar
            visible: root.barShown
            Layout.fillWidth: true
            currentIndex: -1
            background: Rectangle {
                color: "transparent"

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: root.scheme.border
                }
            }

            Repeater {
                model: root.labels

                TabButton {
                    font: Theme.bodyFont
                    id: tab
                    required property int index
                    required property string modelData
                    text: modelData
                    width: implicitWidth
                    leftPadding: Theme.spaceOf(4)
                    rightPadding: Theme.spaceOf(4)
                    contentItem: Text {
                        text: tab.text
                        font: tab.font
                        color: tab.checked ? root.scheme.accent : root.scheme.muted
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    background: Rectangle {
                        color: "transparent"

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 2
                            visible: tab.checked
                            color: root.scheme.accent
                        }
                    }
                    onClicked: root.fire("select", index)
                }
            }
        }

        ColumnLayout {
            id: pages
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
        }
    }
}
