// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick

// A table row's background, under its cells: the selection fill, a toned row's fill and leading bar, and the hairline
// between rows. It is the `tableRow` part a restyle replaces; the row writes its state here and keeps its placement.
// Its implicit height is the row's least height.
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

    function toneDot(index: int): color {
        switch (index) {
        case 1:
            return root.scheme.infoDot
        case 2:
            return root.scheme.successDot
        case 3:
            return root.scheme.warnDot
        case 4:
            return root.scheme.errorDot
        default:
            return root.scheme.muted
        }
    }

    Rectangle {
        objectName: "fill"
        anchors.fill: parent
        color: root.heading ? root.scheme.tableHeader
                            : root.selected ? root.scheme.selection
                                            : root.tone >= 0 ? root.toneBg(root.tone) : "transparent"
    }
    // A toned row's leading bar, kept while the row is selected.
    Rectangle {
        objectName: "toneBar"
        visible: root.tone >= 0
        width: 3
        height: parent.height
        color: root.toneDot(root.tone)
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
