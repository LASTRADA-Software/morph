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

        // A GridLayout column sizes to its widest cell's unwrapped implicitWidth, which a value's own
        // Layout.fillWidth never shrinks below; without an explicit cap, a long unbroken value just grows the
        // column (and the whole widget) past whatever container it's given instead of ever wrapping. Capping
        // the value column at the grid's own available width -- the one width a GridLayout does always
        // honour -- is what lets wrapMode actually engage when the container is narrower than the content
        // would otherwise want. Unconstrained (a parent that hasn't imposed a width, i.e. the common case of
        // sizing to content), grid.width equals its own implicitWidth, so the cap is never tighter than the
        // value's natural width and nothing wraps that wouldn't otherwise.
        readonly property real valueMaxWidth: Math.max(0, grid.width - labelColumnWidth - grid.columnSpacing)
        property real labelColumnWidth: 0

        Repeater {
            model: root.items.length * 2

            Label {
                font: Theme.bodyFont
                required property int index
                readonly property var pair: root.items[Math.floor(index / 2)]
                text: pair ? (index % 2 === 0 ? pair.label : pair.value) : ""
                color: index % 2 === 0 ? root.scheme.muted : root.scheme.text
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.maximumWidth: index % 2 === 1 ? grid.valueMaxWidth : -1
                onImplicitWidthChanged: {
                    if (index % 2 === 0)
                        grid.labelColumnWidth = Math.max(grid.labelColumnWidth, implicitWidth)
                }
                // One node per pair, named "<caption>: <value>", so a test reads a value without a second query.
                Accessible.name: pair ? pair.label + ": " + pair.value : ""
                Accessible.ignored: index % 2 === 1
            }
        }
    }
}
