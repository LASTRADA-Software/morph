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
                required property int index
                readonly property var pair: root.items[Math.floor(index / 2)]
                text: pair ? (index % 2 === 0 ? pair.label : pair.value) : ""
                color: index % 2 === 0 ? Theme.muted : Theme.text
                // One node per pair, named "<caption>: <value>", so a test reads a value without a second query.
                Accessible.name: pair ? pair.label + ": " + pair.value : ""
                Accessible.ignored: index % 2 === 1
            }
        }
    }
}
