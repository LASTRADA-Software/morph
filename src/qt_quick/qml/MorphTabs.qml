// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A tab bar over page slots. The mount keeps exactly the selected page's slot visible; the bar's highlight follows
// `select` and the user's clicks. A hidden bar (`barShown` false) gives its height to the pages; a shown one sits a
// spacing step above them. Tabs that do not fit beside a trailing "More" button are left out of the bar and listed
// in the button's menu; choosing one there chooses that tab, as a click on its button would.
MorphWidget {
    id: root

    property list<string> labels: []
    property bool barShown: true
    property string moreLabel: "More"
    // How many leading tabs the bar shows; the rest are the More menu's.
    property int shownCount: labels.length
    readonly property bool overflowing: shownCount < labels.length
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
        if (bar.currentIndex >= 0 && list && bar.currentIndex < root.shownCount)
            list.positionViewAtIndex(bar.currentIndex, ListView.Contain)
    }

    // The More button's width for a label: the label and the chevron, in the padding a tab has.
    function moreWidthFor(label: string): real {
        return metrics.advanceWidth(label + " ▾") + 2 * Theme.spaceOf(3)
    }

    // Shows the leading tabs that fit beside More when the tabs together are wider than the row. More is measured at
    // its widest label, the wider of `moreLabel` and the longest tab it would hold, so its label changing with the
    // chosen tab changes nothing about what fits. The fit reads the tabs' implicit widths, not the widths they are
    // drawn at, so it does not depend on its own result.
    function refit() {
        const count = root.labels.length
        const widths = []
        let total = 0
        for (let i = 0; i < count; ++i) {
            const button = tabs.itemAt(i) as TabButton
            const width = button ? button.implicitWidth : 0
            widths.push(width)
            total += width
        }
        const room = tabRow.width
        if (total <= room || room <= 0) {
            root.shownCount = count
            return
        }
        for (let shown = count - 1; shown >= 0; --shown) {
            let widest = root.moreWidthFor(root.moreLabel)
            let used = 0
            for (let i = 0; i < count; ++i) {
                if (i < shown)
                    used += widths[i]
                else
                    widest = Math.max(widest, root.moreWidthFor(root.labels[i]))
            }
            if (used + widest <= room || shown === 0) {
                root.shownCount = shown
                return
            }
        }
    }

    function scheduleFit() {
        Qt.callLater(root.refit)
    }

    onLabelsChanged: scheduleFit()
    onMoreLabelChanged: scheduleFit()

    // A hidden bar never holds the focus: the workspace's chords must reach a page when a second tab arrives.
    focusTarget: bar.visible ? bar : pages
    implicitWidth: Math.max(bar.visible ? bar.implicitWidth + (more.visible ? more.implicitWidth : 0) : 0,
                            pages.implicitWidth)
    implicitHeight: (bar.visible ? bar.implicitHeight + layout.spacing : 0) + pages.implicitHeight

    FontMetrics {
        id: metrics
        font: Theme.bodyFont
    }

    ColumnLayout {
        id: layout
        anchors.fill: parent
        spacing: Theme.spaceOf(3)

        Item {
            id: tabArea
            visible: root.barShown
            Layout.fillWidth: true
            implicitHeight: tabRow.implicitHeight

            // The rule under the tabs runs the row's width, past the last tab and More.
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: root.scheme.border
            }

            RowLayout {
                id: tabRow
                anchors.fill: parent
                spacing: 0
                onWidthChanged: root.scheduleFit()

                // As wide as its shown tabs, so More follows the last one.
                TabBar {
                    id: bar
                    visible: root.barShown
                    currentIndex: -1
                    spacing: 0
                    onWidthChanged: root.keepChosenInView()
                    background: Item {
                    }

                    // The model is the count, not the list: a relabel with the same count keeps every button, and with it
                    // the highlight and the keyboard focus; a list model would rebuild them all.
                    Repeater {
                        id: tabs
                        model: root.labels.length

                        TabButton {
                            font: Theme.bodyFont
                            id: tab
                            required property int index
                            text: index < root.labels.length ? root.labels[index] : ""
                            // A tab past the fit leaves the bar, the focus chain and the accessibility tree, and takes no
                            // width; its implicit width still counts towards the fit.
                            visible: index < root.shownCount
                            width: visible ? implicitWidth : 0
                            onImplicitWidthChanged: root.scheduleFit()
                            // A touch target while the window is compact; its label keeps its own size.
                            implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                                                     Theme.touchTargets ? Theme.targetHeight : 0)
                            leftPadding: Theme.spaceOf(3)
                            rightPadding: Theme.spaceOf(3)
                            Keys.onRightPressed: event => {
                                if (index === root.shownCount - 1 && more.visible) {
                                    more.forceActiveFocus()
                                    event.accepted = true
                                }
                            }
                            contentItem: Text {
                                textFormat: Text.PlainText
                                text: tab.text
                                font: tab.font
                                color: tab.checked ? root.scheme.text : root.scheme.muted
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

                // A sibling of the bar, not one of its buttons, so the bar's count and current index stay the tabs'.
                AbstractButton {
                    id: more
                    objectName: "more"
                    visible: root.barShown && root.overflowing
                    // The hidden tab that is chosen, whose label More then shows.
                    readonly property int chosenHidden: bar.currentIndex >= root.shownCount ? bar.currentIndex : -1
                    text: (chosenHidden >= 0 ? root.labels[chosenHidden] : root.moreLabel) + " ▾"
                    font: Theme.bodyFont
                    implicitHeight: bar.implicitHeight
                    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
                    leftPadding: Theme.spaceOf(3)
                    rightPadding: Theme.spaceOf(3)
                    Layout.alignment: Qt.AlignBottom
                    Accessible.role: Accessible.Button
                    Accessible.name: text
                    contentItem: Text {
                        textFormat: Text.PlainText
                        text: more.text
                        font: more.font
                        color: more.chosenHidden >= 0 ? root.scheme.text : root.scheme.muted
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: "transparent"

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 2
                            visible: more.chosenHidden >= 0
                            color: root.scheme.accent
                        }
                    }
                    onClicked: popup.open()
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    Keys.onDownPressed: clicked()
                    Keys.onLeftPressed: {
                        const last = tabs.itemAt(root.shownCount - 1) as TabButton
                        if (last)
                            last.forceActiveFocus()
                    }
                }

                Item {
                    Layout.fillWidth: true
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

    Popup {
        id: popup
        objectName: "moreMenu"
        y: tabArea.y + tabArea.height
        padding: 2
        onAboutToShow: placement.place(popup, more)

        MorphPopupPlacement {
            id: placement
        }

        background: MorphPopupBackground {
        }

        contentItem: ColumnLayout {
            spacing: 0

            Repeater {
                model: root.overflowing ? root.labels.length - root.shownCount : 0

                ItemDelegate {
                    id: entry
                    objectName: "moreEntry"
                    required property int index
                    readonly property int tabIndex: root.shownCount + index
                    Layout.fillWidth: true
                    font: Theme.bodyFont
                    text: root.labels[tabIndex]
                    contentItem: MorphPlainContent {
                        control: entry
                        color: entry.palette.text
                        alignment: Qt.AlignLeft
                    }
                    background: Rectangle {
                        radius: Theme.radiusControl
                        color: entry.down || entry.hovered || entry.visualFocus
                               ? Theme.cardScheme.hover : "transparent"
                    }
                    Accessible.role: Accessible.MenuItem
                    onClicked: {
                        popup.close()
                        root.fire("select", tabIndex)
                    }
                }
            }
        }
    }
}
