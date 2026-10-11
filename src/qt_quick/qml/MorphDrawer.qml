// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A modal panel along the start or end edge of the window, with a dialog's rules: Escape or a click outside closes
// it and reports a dismissal; `show` opens and closes it without one.
MorphWidget {
    id: root

    property string title: ""
    // 0 start, 1 end; fixed at creation.
    property int side: 0
    property bool programmatic: false
    readonly property Item content: column
    readonly property bool shown: drawer.visible

    function show(open: bool) {
        if (drawer.visible === open)
            return
        root.programmatic = true
        if (open)
            drawer.open()
        else
            drawer.close()
        root.programmatic = false
    }

    function dismiss() {
        if (drawer.visible)
            drawer.close()
    }

    focusTarget: column
    implicitWidth: 0
    implicitHeight: 0

    Drawer {
        id: drawer
        edge: root.side === 0 ? Qt.LeftEdge : Qt.RightEdge
        interactive: false
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: Math.max(column.implicitWidth + 2 * Theme.unitWidth, 30 * Theme.unitWidth)
        height: Overlay.overlay ? Overlay.overlay.height : 0
        onAboutToHide: {
            if (!root.programmatic)
                root.fire("dismiss", undefined)
        }

        contentItem: ColumnLayout {
            Label {
                visible: root.title !== ""
                text: root.title
                font.bold: true
            }

            ColumnLayout {
                id: column
                Layout.fillWidth: true
            }

            Item {
                Layout.fillHeight: true
            }
        }
    }
}
