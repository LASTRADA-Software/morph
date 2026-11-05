// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick

// A table row's background, under its cells, from three marks that compose: a toned row's tint, then the selection
// fill, then the selection bar at the leading edge. A selected row has the accent's 3 px bar, and the selection
// fill unless the table's select column already shows the selection in its box. A toned row has a tint only (its
// tone's fill at half strength over the surface), so an out-of-spec row reads as pale and a selected one as marked.
// A hairline separates rows. It is the `tableRow` part a restyle replaces; the row writes its state here and keeps its
// placement. Its implicit height is the row's least height.
Item {
    id: root

    property bool selected: false
    // Whether the pointer is over the row.
    property bool hovered: false
    // Whether keyboard focus is within the row.
    property bool current: false
    // Whether the row is at an odd position in its table, for zebra stripes.
    property bool alternate: false
    // The row's tone: -1 for none, else a tone index (Neutral, Info, Ok, Warn, Err).
    property int tone: -1
    // Whether the row is a group heading, filled as the header row is.
    property bool heading: false
    // Whether the table's select column shows the selection, so the row takes no fill for it.
    property bool selectColumn: false
    // The colour roles of the table the row is in.
    property MorphScheme scheme: Theme.themeScheme

    // A dense row: the control height less 8 px.
    implicitHeight: Theme.targetHeight - 8

    // A tone's fill and marker in the scheme: `color.<tone>.bg` and `.dot`; neutral is the muted colour, faint.
    function toneBg(index: int): color {
        switch (index) {
        case 1:
            return root.scheme.infoBg
        case 2:
            return root.scheme.successBg
        case 3:
            return root.scheme.warnBg
        case 4:
            return root.scheme.errorBg
        default:
            return Qt.alpha(root.scheme.muted, 0.15)
        }
    }

    // A toned row's tint: the tone's fill at half strength over the surface.
    Rectangle {
        objectName: "tint"
        anchors.fill: parent
        visible: !root.heading && root.tone >= 0
        color: Qt.tint(root.scheme.surface, Qt.alpha(root.toneBg(root.tone), 0.5))
    }
    Rectangle {
        objectName: "fill"
        anchors.fill: parent
        color: root.heading ? root.scheme.tableHeader
                            : root.selected && !root.selectColumn ? root.scheme.selection : "transparent"
    }
    // A selected row's leading bar.
    Rectangle {
        objectName: "selectionBar"
        visible: root.selected && !root.heading
        width: 3
        height: parent.height
        color: root.scheme.accent
    }
    // The hairline between rows.
    Rectangle {
        objectName: "rule"
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: root.scheme.divider
    }
}
