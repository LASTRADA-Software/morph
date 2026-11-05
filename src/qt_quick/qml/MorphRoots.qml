// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Where a backend's roots are shown: one column, top to bottom, filling the host item. A root whose height stretches
// takes the height the others leave; without one, the roots keep their own heights at the top, since a column given
// more height than it needs hands the spare to the gaps between its children.
Item {
    id: root
    readonly property Item content: column
    // The relay the width below is reported through.
    property var host: null
    // The host's width in units, which the backend classes as `{"env": "widthClass"}`; reported on each change, from
    // a resize or a theme with another font.
    readonly property real widthUnits: Theme.unitWidth > 0 ? width / Theme.unitWidth : 0
    onWidthUnitsChanged: {
        if (root.host)
            root.host.fire("widthUnits", root.widthUnits)
    }

    readonly property bool stretches: {
        for (const child of column.children) {
            if (child.visible && child.heightKind === 2)
                return true
        }
        return false
    }

    // The width the roots cannot be narrower than: a column never lays a root out narrower than its minimum, so a root
    // is as wide as its content and the window cuts the rest off. The window reports that, once, as a container does.
    readonly property real needWidth: column.Layout.minimumWidth
    readonly property bool overflowing: needWidth > 0 && needWidth >= width + 2 && width > 0 && Window.window !== null
    property bool overflowReported: false

    // The root whose minimum alone exceeds the window's width, or empty.
    function overflowChild(): string {
        let found = ""
        let widest = root.width
        for (const object of column.children) {
            const child = object as MorphWidget
            if (child && child.visible && child.Layout.minimumWidth > widest) {
                widest = child.Layout.minimumWidth
                found = child.objectName
            }
        }
        return found
    }

    function checkOverflow() {
        if (root.overflowReported || !root.overflowing || !root.host)
            return
        root.overflowReported = true
        root.host.fire("overflow", {
            "where": "the window",
            "need": Math.ceil(root.needWidth),
            "have": Math.floor(root.width),
            "child": root.overflowChild(),
            "table": false
        })
    }

    // Checked once the layout has settled, as a container's is.
    Timer {
        id: overflowTimer
        interval: 60
        onTriggered: root.checkOverflow()
    }

    onOverflowingChanged: {
        if (root.overflowing)
            overflowTimer.restart()
        else
            overflowTimer.stop()
    }

    anchors.fill: parent

    // The window behind every surface.
    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.stretches ? root.height : implicitHeight
        spacing: 0
    }
}
