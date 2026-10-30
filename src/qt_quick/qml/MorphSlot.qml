// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Where a switch case or a tabs page is shown: its children as they are, inset by `contentInset` on every side. A
// scrollable slot scrolls only while its content does not fit: a child that does not stretch down is shown at its
// natural height, a child that stretches down fills the slot but is never shorter than its `minHeight`, and content
// that cannot be narrower than the slot scrolls sideways. The inset lies inside the scroll, so a page that scrolls
// clips at its own edges and its bar sits at them, and a bleeding child reaches them. The viewport keeps the focused
// item in view.
MorphWidget {
    id: root

    readonly property Item content: column
    property bool scrollable: false
    // The padding around the content, in pixels, which a tabs node gives each of its pages.
    property real contentInset: 0
    // The size the content needs, padding included: its minimum width and its natural height. A width shortfall
    // under 2 px is layout rounding, and a height one under half a pixel is too, not content that does not fit.
    readonly property real needX: column.Layout.minimumWidth + 2 * root.contentInset
    readonly property real needY: column.implicitHeight + 2 * root.contentInset
    // A slot reports content that is wider than it, whether it scrolls sideways or cuts it off.
    needWidth: root.needX
    haveWidth: root.width
    overflowKind: "page"
    // Whether the content is wider, or taller, than the slot: a child that stretches down is as tall as the slot
    // unless its `minHeight` is taller.
    readonly property bool overX: scrollable && needX > root.width + 2
    readonly property bool overY: scrollable && (stretchesDown ? stretchFloor + 2 * root.contentInset > root.height
                                                               : needY > root.height + 0.5)
    // Whether the viewport scrolls now, in either direction.
    readonly property bool scrolls: overX || overY
    // The bars' part, `MorphScrollBar` or its restyle, which C++ passes at creation.
    property Component bar: null
    // The vertical bar made from `bar`, or null without one.
    property ScrollBar barItem: null
    // The horizontal bar made from `bar`, or null without one.
    property ScrollBar sideBarItem: null

    implicitWidth: column.implicitWidth + 2 * root.contentInset
    implicitHeight: column.implicitHeight + 2 * root.contentInset
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

    onOverYChanged: {
        if (!overY)
            flick.contentY = 0
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
        contentHeight: root.overY ? (root.stretchesDown ? root.stretchFloor + 2 * root.contentInset : root.needY)
                                  : height
        flickableDirection: root.overX && root.overY ? Flickable.HorizontalAndVerticalFlick
                                                     : root.overX ? Flickable.HorizontalFlick
                                                                  : Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: root.barItem
        ScrollBar.horizontal: root.sideBarItem

        ColumnLayout {
            id: column
            // What a bleeding child reaches past: the page's padding, so it ends at the slot's own edges.
            readonly property real childBleedInset: root.contentInset
            x: root.contentInset
            y: root.contentInset
            width: (root.overX ? root.needX : flick.width) - 2 * root.contentInset
            height: root.stretchesDown ? Math.max(flick.height - 2 * root.contentInset, root.stretchFloor)
                                       : implicitHeight
            spacing: 0
        }
    }

    // The bar is a child of the flickable itself, not of its content: the attached ScrollBar lays out only a bar
    // whose parent is the flickable.
    Component.onCompleted: {
        if (root.bar)
            root.barItem = root.bar.createObject(flick, {
                orientation: Qt.Vertical,
                visible: Qt.binding(() => root.overY),
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
