// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// An overlay's footer: what C++ puts in it (the overlay's actions) in one row against the right edge. Empty, it
// takes no room. While `stretching`, as a sheet's footer does, it fills the overlay's width and its actions share it.
MorphWidget {
    id: root

    readonly property Item content: row
    // Whether anything is in the footer.
    readonly property bool holding: row.children.length > 0
    // Whether the footer fills its width and its actions share it equally: while the area it sits in says so.
    // Untyped, so the area may declare `stretchesFooter` or not.
    readonly property var area: parent
    readonly property bool stretching: area !== null && area !== undefined && area.stretchesFooter === true
    sharesWidth: stretching

    Layout.fillWidth: stretching

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    RowLayout {
        id: row
        anchors.right: parent.right
        anchors.left: root.stretching ? parent.left : undefined
        height: root.height
        spacing: Theme.spaceOf(2)
        // A stretching footer's actions each take an equal share, so a lone action fills the footer.
        onChildrenChanged: root.share()
    }

    // Has what the footer holds take a share of its width while it stretches, and stop after. A row of actions, as an
    // overlay's `actions` are, hands its width on to the actions in it.
    function share() {
        for (const child of row.children) {
            const widget = child as MorphWidget
            if (widget)
                widget.sharesWidth = root.stretching
        }
    }
    onStretchingChanged: share()
}
