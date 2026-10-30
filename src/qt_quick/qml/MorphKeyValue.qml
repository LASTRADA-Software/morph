// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Captions beside their values, one pair per row; or, while `captionsAbove`, each caption over its value, the pairs
// divided by rules. A toned value is drawn in its tone's colour and the strong weight; a value with runs draws them.
MorphWidget {
    id: root

    // [{label, value, runs, tone}]: `value` is the characters shown, `runs` a value's runs as a text's (empty for a
    // plain value), `tone` the `Tone` index (0 none).
    property var items: []
    // Whether each caption sits above its value; set once.
    property bool captionsAbove: false

    implicitWidth: captionsAbove ? stack.implicitWidth : grid.implicitWidth
    implicitHeight: captionsAbove ? stack.implicitHeight : grid.implicitHeight

    // A pair's value: its runs or its text, toned or not.
    component Value: Label {
        required property var pair
        readonly property bool hasRuns: pair && pair.runs.length > 0
        textFormat: hasRuns ? Text.RichText : Text.PlainText
        text: !pair ? "" : hasRuns ? root.runsHtml(pair.runs) : pair.value
        font: pair && pair.tone !== 0 ? Theme.strongFont : Theme.bodyFont
        color: pair && pair.tone !== 0 ? root.toneOf(pair.tone) : root.scheme.text
        wrapMode: Text.Wrap
        Accessible.ignored: true
    }

    GridLayout {
        id: grid
        anchors.fill: parent
        visible: !root.captionsAbove
        columns: 2
        columnSpacing: 2 * Theme.unitWidth

        Repeater {
            model: root.captionsAbove ? 0 : root.items.length

            Label {
                required property int index
                readonly property var pair: root.items[index]
                textFormat: Text.PlainText
                font: Theme.bodyFont
                text: pair ? pair.label : ""
                color: root.scheme.muted
                // The grid sizes from its columns' own rules, with no cap computed from its width: a caption keeps
                // its whole width on one line, and a value fills what is left and wraps in it. A cap read from the
                // grid's width and written back from a label's implicit width during the grid's own polish is a
                // binding cycle through the layout, which Qt reports as a polish loop.
                wrapMode: Text.NoWrap
                Layout.row: index
                Layout.column: 0
                Layout.minimumWidth: implicitWidth
                // One node per pair, named "<caption>: <value>", so a test reads a value without a second query.
                Accessible.name: pair ? pair.label + ": " + pair.value : ""
            }
        }

        Repeater {
            model: root.captionsAbove ? 0 : root.items.length

            Value {
                required property int index
                pair: root.items[index]
                Layout.row: index
                Layout.column: 1
                Layout.fillWidth: true
            }
        }
    }

    ColumnLayout {
        id: stack
        anchors.fill: parent
        visible: root.captionsAbove
        spacing: 0

        Repeater {
            model: root.captionsAbove ? root.items.length : 0

            ColumnLayout {
                id: pairColumn
                required property int index
                readonly property var pair: root.items[index]
                spacing: 0
                Layout.fillWidth: true

                Label {
                    objectName: "caption"
                    textFormat: Text.PlainText
                    font: Theme.bodyFont
                    text: pairColumn.pair ? pairColumn.pair.label : ""
                    color: root.scheme.muted
                    Layout.fillWidth: true
                    Accessible.name: pairColumn.pair ? pairColumn.pair.label + ": " + pairColumn.pair.value : ""
                }

                Value {
                    objectName: "value"
                    pair: pairColumn.pair
                    Layout.fillWidth: true
                }

                // A rule after every pair but the last, a spacing step from each.
                Rectangle {
                    objectName: "rule"
                    visible: pairColumn.index < root.items.length - 1
                    color: root.scheme.border
                    implicitHeight: 1
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spaceOf(1)
                    Layout.bottomMargin: Theme.spaceOf(1)
                }
            }
        }
    }
}
