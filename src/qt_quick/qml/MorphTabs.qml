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
        keepChosenInView()
    }

    // The chosen tab lies wholly inside the bar however it was chosen: placed once the bar has been laid out, since a
    // tab chosen before the first layout has no position yet, and again whenever the bar's width changes.
    function keepChosenInView() {
        Qt.callLater(root.placeChosen)
    }

    function placeChosen() {
        const list = bar.contentItem as ListView
        if (bar.currentIndex >= 0 && list)
            list.positionViewAtIndex(bar.currentIndex, ListView.Contain)
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
            onWidthChanged: root.keepChosenInView()
            background: Rectangle {
                color: "transparent"

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: root.scheme.border
                }
            }

            // The model is the count, not the list: a relabel with the same count keeps every button, and with it
            // the highlight and the keyboard focus; a list model would rebuild them all.
            Repeater {
                model: root.labels.length

                TabButton {
                    font: Theme.bodyFont
                    id: tab
                    required property int index
                    text: index < root.labels.length ? root.labels[index] : ""
                    width: implicitWidth
                    // A touch target while the window is compact; its label keeps its own size.
                    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                                             Theme.touchTargets ? Theme.targetHeight : 0)
                    leftPadding: Theme.spaceOf(4)
                    rightPadding: Theme.spaceOf(4)
                    contentItem: Text {
                        textFormat: Text.PlainText
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
