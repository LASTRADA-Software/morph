// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Captions beside their values, one pair per row.
MorphWidget {
    id: root

    // [{label, value}].
    property var items: []

    implicitWidth: grid.implicitWidth
    implicitHeight: grid.implicitHeight

    GridLayout {
        id: grid
        anchors.fill: parent
        columns: 2
        columnSpacing: 2 * Theme.unitWidth

        Repeater {
            model: root.items.length * 2

            Label {
                font: Theme.bodyFont
                required property int index
                readonly property var pair: root.items[Math.floor(index / 2)]
                text: pair ? (index % 2 === 0 ? pair.label : pair.value) : ""
                color: index % 2 === 0 ? root.scheme.muted : root.scheme.text
                // The grid sizes from its columns' own rules, with no cap computed from its width: a caption keeps
                // its whole width on one line, and a value fills what is left and wraps in it. A cap read from the
                // grid's width and written back from a label's implicit width during the grid's own polish is a
                // binding cycle through the layout, which Qt reports as a polish loop.
                wrapMode: index % 2 === 0 ? Text.NoWrap : Text.Wrap
                Layout.fillWidth: index % 2 === 1
                Layout.minimumWidth: index % 2 === 0 ? implicitWidth : -1
                // One node per pair, named "<caption>: <value>", so a test reads a value without a second query.
                Accessible.name: pair ? pair.label + ": " + pair.value : ""
                Accessible.ignored: index % 2 === 1
            }
        }
    }
}
