// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A tab bar over page slots. The mount keeps exactly the selected page's slot visible; the bar's highlight follows
// `select` and the user's clicks.
MorphWidget {
    id: root

    property list<string> labels: []
    readonly property Item content: pages
    readonly property int highlighted: bar.currentIndex

    // Highlights tab `index`; past the last highlights none.
    function select(index: int) {
        bar.currentIndex = index < bar.count ? index : -1
    }

    focusTarget: bar
    implicitWidth: Math.max(bar.implicitWidth, pages.implicitWidth)
    implicitHeight: bar.implicitHeight + pages.implicitHeight

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: bar
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
                    palette.buttonText: tab.checked ? root.scheme.accent : root.scheme.muted
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
