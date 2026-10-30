// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Where a switch case or a tabs page is shown: its children as they are. A scrollable slot shows a child that does
// not stretch down at its natural height in a vertical viewport of its own height, which keeps the focused item in
// view; a child that stretches down fills it, as in a slot that does not scroll, but is never shorter than its
// `minHeight`: while the slot is shorter than that, the slot scrolls the child at that height.
MorphWidget {
    id: root

    readonly property Item content: column
    property bool scrollable: false
    // The width the content cannot be narrower than; a shortfall under 2 px is layout rounding, not content that
    // does not fit.
    readonly property real needX: column.Layout.minimumWidth
    readonly property bool overX: scrollable && needX > root.width + 2
    // A slot reports content that is wider than it, whether it scrolls sideways or cuts it off.
    needWidth: root.needX
    haveWidth: root.width
    overflowKind: "page"
    // Whether the viewport scrolls down now: scrollable, and no shown child that stretches down fits.
    readonly property bool scrollsDown: scrollable && (!stretchesDown || stretchFloor > root.height)
    // Whether the viewport scrolls now, in either direction.
    readonly property bool scrolls: scrollsDown || overX
    // The bars' part, `MorphScrollBar` or its restyle, which C++ passes at creation.
    property Component bar: null
    // The vertical bar made from `bar`, or null without one.
    property ScrollBar barItem: null
    // The horizontal bar made from `bar`, or null without one.
    property ScrollBar sideBarItem: null

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

    // The tallest `minHeight` among the shown children that stretch down, in pixels; zero for none.
    readonly property real stretchFloor: {
        let floor = 0
        for (const child of column.children) {
            if (child.visible && child.heightKind === 2 && child.minHeightUnits > 0)
                floor = Math.max(floor, child.minHeightUnits * Theme.unitHeight)
        }
        return floor
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
                if (root.overX) {
                    if (at.x < flick.contentX)
                        flick.contentX = at.x
                    else if (at.x + item.width > flick.contentX + flick.width)
                        flick.contentX = at.x + item.width - flick.width
                }
                return
            }
        }
    }

    onScrollsChanged: {
        if (!scrolls) {
            flick.contentY = 0
            flick.contentX = 0
        }
    }
    onOverXChanged: {
        if (!overX)
            flick.contentX = 0
    }

    Flickable {
        id: flick
        objectName: "flick"
        anchors.fill: parent
        interactive: root.scrolls
        contentWidth: root.overX ? root.needX : width
        contentHeight: root.scrollsDown ? column.height : height
        flickableDirection: root.overX && root.scrollsDown ? Flickable.HorizontalAndVerticalFlick
                                                           : root.overX ? Flickable.HorizontalFlick
                                                                        : Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: root.barItem
        ScrollBar.horizontal: root.sideBarItem

        ColumnLayout {
            id: column
            width: root.overX ? root.needX : flick.width
            height: root.stretchesDown ? Math.max(flick.height, root.stretchFloor) : implicitHeight
            spacing: 0
        }
    }

    // The bar is a child of the flickable itself, not of its content: the attached ScrollBar lays out only a bar
    // whose parent is the flickable.
    Component.onCompleted: {
        if (root.bar)
            root.barItem = root.bar.createObject(flick, {
                orientation: Qt.Vertical,
                visible: Qt.binding(() => root.scrollsDown),
                scheme: Qt.binding(() => root.scheme)
            }) as ScrollBar
        if (root.bar)
            root.sideBarItem = root.bar.createObject(flick, {
                orientation: Qt.Horizontal,
                policy: ScrollBar.AlwaysOn,
                visible: Qt.binding(() => root.overX),
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
