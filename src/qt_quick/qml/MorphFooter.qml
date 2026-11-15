// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// An overlay's footer: what C++ puts in it (the overlay's actions) in one row against the right edge, and the
// overlay's footer note, muted, leading against the left. Empty, it takes no room. While `stretching`, as a sheet's
// footer does, it fills the overlay's width and its actions share it, with the note above them.
MorphWidget {
    id: root

    readonly property Item content: row
    // Whether the footer fills its width and its actions share it equally: while the area it sits in says so.
    // Untyped, so the area may declare `stretchesFooter` or not.
    readonly property var area: parent
    readonly property bool stretching: area !== null && area !== undefined && area.stretchesFooter === true
    // The note the area it sits in carries; untyped, so the area may declare `footerNote` or not.
    readonly property string note: root.area?.footerNote ?? ""
    // Whether anything is in the footer.
    readonly property bool holding: row.children.length > 0 || root.note !== ""
    // A stretching footer's actions fill its width, so its note stands above them instead of beside them.
    readonly property bool noteAbove: root.stretching && root.note !== ""
    sharesWidth: stretching

    // Always the area's full width: the actions sit against its right edge, or share it while stretching.
    Layout.fillWidth: true

    implicitWidth: row.implicitWidth + (root.note !== "" && !root.stretching ? noteLabel.implicitWidth + Theme.actionSpacing : 0)
    implicitHeight: root.noteAbove ? noteLabel.implicitHeight + Theme.actionSpacing + row.implicitHeight
                                   : Math.max(row.implicitHeight, root.note !== "" ? noteLabel.implicitHeight : 0)

    Label {
        id: noteLabel
        objectName: "footerNote"
        visible: root.note !== ""
        text: root.note
        textFormat: Text.PlainText
        font: Theme.bodyFont
        color: Theme.cardScheme.muted
        elide: Text.ElideRight
        anchors.left: parent.left
        anchors.top: root.noteAbove ? parent.top : undefined
        anchors.verticalCenter: root.noteAbove ? undefined : parent.verticalCenter
        width: root.noteAbove ? root.width : Math.max(0, root.width - row.implicitWidth - Theme.actionSpacing)
    }

    RowLayout {
        id: row
        anchors.right: parent.right
        anchors.left: root.stretching ? parent.left : undefined
        anchors.bottom: root.noteAbove ? parent.bottom : undefined
        height: root.noteAbove ? row.implicitHeight : root.height
        spacing: Theme.actionSpacing
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
