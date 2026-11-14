// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A scrolled viewport on one axis that keeps the focused child visible.
MorphWidget {
    id: root

    // 0 vertical, 1 horizontal; fixed at creation.
    property int axis: 0
    // Where the children go: a column down a vertical scroll, a row along a horizontal one, as on the terminal.
    readonly property Item content: root.axis === 1 ? row.content : column
    // The item that is the scrolled content, whose size is the flickable's content size.
    readonly property Item body: root.axis === 1 ? row : column
    // The bar's part, `MorphScrollBar` or its restyle, which C++ passes at creation; instantiated once, on the axis.
    property Component bar: null
    // The bar made from `bar`, or null without one.
    property ScrollBar barItem: null
    // The horizontal bar of a vertical scroll, shown while its content cannot be narrower than the viewport.
    property ScrollBar sideBarItem: null
    // The width the content cannot be narrower than; a shortfall under 2 px is layout rounding. Only a vertical scroll
    // scrolls sideways this way: a horizontal one is as wide as its content already.
    readonly property real needX: column.Layout.minimumWidth
    readonly property bool overX: root.axis === 0 && needX > flick.width + 2
    needWidth: root.axis === 0 ? root.needX : -1
    haveWidth: flick.width
    overflowKind: "scroll"

    // Scrolls so that `item`, somewhere inside the content, is in view.
    function reveal(item: Item) {
        if (!item)
            return
        for (let up = item; up; up = up.parent) {
            if (up === root.body) {
                const at = item.mapToItem(root.body, 0, 0)
                if (root.axis === 0) {
                    if (at.y < flick.contentY)
                        flick.contentY = at.y
                    else if (at.y + item.height > flick.contentY + flick.height)
                        flick.contentY = at.y + item.height - flick.height
                } else {
                    if (at.x < flick.contentX)
                        flick.contentX = at.x
                    else if (at.x + item.width > flick.contentX + flick.width)
                        flick.contentX = at.x + item.width - flick.width
                }
                return
            }
        }
    }

    implicitWidth: root.axis === 0 ? column.implicitWidth : Math.min(row.implicitWidth, 40 * Theme.unitWidth)
    implicitHeight: root.axis === 0 ? Math.min(column.naturalHeight, 20 * Theme.unitHeight) : row.implicitHeight
    clip: true

    Flickable {
        id: flick
        objectName: "flick"
        anchors.fill: parent
        contentWidth: root.axis === 0 ? (root.overX ? root.needX : width) : row.width
        contentHeight: root.axis === 0 ? column.naturalHeight : height
        flickableDirection: root.axis === 1 ? Flickable.HorizontalFlick
                                            : root.overX ? Flickable.HorizontalAndVerticalFlick
                                                         : Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: root.axis === 0 ? root.barItem : null
        ScrollBar.horizontal: root.axis === 1 ? root.barItem : root.sideBarItem

        // The content of a vertical scroll: a column as wide as the viewport, or as its minimum when that is wider.
        MorphShareColumn {
            id: column
            visible: root.axis === 0
            height: column.naturalHeight
            width: root.overX ? root.needX : flick.width
        }
        // The content of a horizontal scroll: a row at least as wide as the viewport, so a child that stretches fills it
        // while it is narrower and scrolls once it is wider; the terminal sizes it the same way.
        MorphStack {
            id: row
            visible: root.axis === 1
            axis: 1
            height: row.implicitHeight
            width: Math.max(row.implicitWidth, flick.width)
        }
    }

    // The bar is a child of the flickable itself, not of its content: the attached ScrollBar lays out only a bar
    // whose parent is the flickable.
    Component.onCompleted: {
        if (root.bar)
            root.barItem = root.bar.createObject(flick, {
                orientation: root.axis === 0 ? Qt.Vertical : Qt.Horizontal,
                scheme: Qt.binding(() => root.scheme)
            }) as ScrollBar
        if (root.bar && root.axis === 0)
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
