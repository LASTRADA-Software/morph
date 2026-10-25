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
    readonly property Item content: column
    // The bar's part, `MorphScrollBar` or its restyle, which C++ passes at creation; instantiated once, on the axis.
    property Component bar: null
    // The bar made from `bar`, or null without one.
    property ScrollBar barItem: null

    // Scrolls so that `item`, somewhere inside the content, is in view.
    function reveal(item: Item) {
        if (!item)
            return
        for (let up = item; up; up = up.parent) {
            if (up === column) {
                const at = item.mapToItem(column, 0, 0)
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

    implicitWidth: root.axis === 0 ? column.implicitWidth : Math.min(column.implicitWidth, 40 * Theme.unitWidth)
    implicitHeight: root.axis === 0 ? Math.min(column.implicitHeight, 20 * Theme.unitHeight) : column.implicitHeight
    clip: true

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: root.axis === 0 ? width : column.width
        contentHeight: root.axis === 0 ? column.implicitHeight : height
        flickableDirection: root.axis === 0 ? Flickable.VerticalFlick : Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: root.axis === 0 ? root.barItem : null
        ScrollBar.horizontal: root.axis === 1 ? root.barItem : null

        // Along a horizontal axis the content is at least the viewport's width, so a child that stretches fills the
        // viewport while it is narrower, and scrolls once it is wider; the terminal sizes it the same way.
        ColumnLayout {
            id: column
            width: root.axis === 0 ? flick.width : Math.max(implicitWidth, flick.width)
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
    }

    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            root.reveal(root.Window.activeFocusItem)
        }
    }
}
