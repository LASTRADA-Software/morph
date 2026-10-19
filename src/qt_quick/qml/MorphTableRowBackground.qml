// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick

// A table row's background, under its cells: the selection fill and the hairline between rows. It is the `tableRow`
// part a restyle replaces; the row writes its state here and keeps its placement. Its implicit height is the row's
// least height.
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
    // The colour roles of the table the row is in.
    property MorphScheme scheme: Theme.themeScheme

    // A dense row: the control height less 8 px.
    implicitHeight: Theme.controlHeight - 8

    Rectangle {
        anchors.fill: parent
        color: root.selected ? root.scheme.selection : "transparent"
    }
    // The hairline between rows.
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: root.scheme.border
        opacity: 0.6
    }
}
