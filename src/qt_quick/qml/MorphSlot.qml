// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Where a switch case or a tabs page is shown: its children as they are. A scrollable slot shows a child that does
// not stretch down at its natural height in a vertical viewport of its own height, which keeps the focused item in
// view; a child that stretches down fills it, as in a slot that does not scroll.
MorphWidget {
    id: root

    readonly property Item content: column
    property bool scrollable: false
    // Whether the viewport scrolls now: scrollable, and no shown child stretches down.
    readonly property bool scrolls: scrollable && !stretchesDown
    // The bar's part, `MorphScrollBar` or its restyle, which C++ passes at creation.
    property Component bar: null
    // The bar made from `bar`, or null without one.
    property ScrollBar barItem: null

    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight
    clip: scrolls

    // Whether a shown child stretches vertically. Without one, the column keeps its natural height and packs the
    // children at the top: given more, a layout would hand the spare height to the space between them.
    readonly property bool stretchesDown: {
        for (const child of column.children) {
            if (child.visible && child.heightKind === 2)
                return true
        }
        return false
    }

    // Scrolls so that `item`, somewhere inside the content, is in view.
    function reveal(item: Item) {
        if (!item || !root.scrolls)
            return
        for (let up = item; up; up = up.parent) {
            if (up === column) {
                const at = item.mapToItem(column, 0, 0)
                if (at.y < flick.contentY)
                    flick.contentY = at.y
                else if (at.y + item.height > flick.contentY + flick.height)
                    flick.contentY = at.y + item.height - flick.height
                return
            }
        }
    }

    onScrollsChanged: {
        if (!scrolls)
            flick.contentY = 0
    }

    Flickable {
        id: flick
        anchors.fill: parent
        interactive: root.scrolls
        contentWidth: width
        contentHeight: root.scrolls ? column.implicitHeight : height
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: root.barItem

        ColumnLayout {
            id: column
            width: flick.width
            height: root.stretchesDown ? flick.height : implicitHeight
            spacing: 0
        }
    }

    // The bar is a child of the flickable itself, not of its content: the attached ScrollBar lays out only a bar
    // whose parent is the flickable.
    Component.onCompleted: {
        if (root.bar)
            root.barItem = root.bar.createObject(flick, {
                orientation: Qt.Vertical,
                visible: Qt.binding(() => root.scrolls),
                scheme: Qt.binding(() => root.scheme)
            }) as ScrollBar
    }

    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            root.reveal(root.Window.activeFocusItem)
        }
    }
}
