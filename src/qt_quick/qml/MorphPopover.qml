// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A panel that belongs to the container it is mounted in and floats under that container's box, aligned to its
// leading edge: the results of a search under its field. It is never modal, dims nothing and takes no keyboard focus,
// so focus stays where it was. A press outside both the panel and its container closes it and reports a dismissal;
// `show` opens and closes it without one. Escape is not its to handle: a popup without focus never receives keys, so
// the focused widget it serves closes it.
MorphWidget {
    id: root

    // The panel's width in units; 0 is the container's width.
    property int widthUnits: 0
    property bool programmatic: false
    readonly property Item content: column
    readonly property bool shown: popup.visible

    function show(open: bool) {
        if (popup.visible === open)
            return
        root.programmatic = true
        if (open)
            popup.open()
        else
            popup.close()
        root.programmatic = false
    }

    focusTarget: column
    implicitWidth: 0
    implicitHeight: 0
    // Only a zero-sized placeholder for the Popup below, which takes no room in the container's layout.
    Layout.preferredWidth: -1
    Layout.fillWidth: false
    Layout.preferredHeight: -1
    Layout.fillHeight: false

    Popup {
        id: popup
        parent: root.parent
        x: 0
        y: parent ? parent.height : 0
        width: root.widthUnits > 0 ? root.widthUnits * Theme.unitWidth : (parent ? parent.width : 0)
        padding: 0
        modal: false
        dim: false
        focus: false
        // Opening and closing are immediate: what the document says is what the window shows, at once.
        enter: Transition {}
        exit: Transition {}
        closePolicy: Popup.CloseOnPressOutsideParent
        onAboutToHide: {
            if (!root.programmatic)
                root.fire("dismiss", undefined)
        }
        background: Rectangle {
            color: Theme.cardScheme.surface
            border.width: 1
            border.color: Theme.cardScheme.border
            radius: Theme.radiusCard
        }
        contentItem: Item {
            implicitHeight: column.naturalHeight
            MorphShareColumn {
                id: column
                width: parent.width
                height: column.naturalHeight
            }
        }
    }
}
