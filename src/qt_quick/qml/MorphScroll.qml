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
        contentWidth: root.axis === 0 ? width : column.implicitWidth
        contentHeight: root.axis === 0 ? column.implicitHeight : height
        flickableDirection: root.axis === 0 ? Flickable.VerticalFlick : Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {
            visible: root.axis === 0
        }
        ScrollBar.horizontal: ScrollBar {
            visible: root.axis === 1
        }

        ColumnLayout {
            id: column
            width: root.axis === 0 ? flick.width : implicitWidth
        }
    }

    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            root.reveal(root.Window.activeFocusItem)
        }
    }
}
