// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Captions beside their values, one pair per row; or, while `captionsAbove`, each caption over its value in the caption
// size, as a form's stacked caption is, the pairs divided by rules. A toned value is drawn in its tone's colour and the
// strong weight; a value with runs draws them.
MorphWidget {
    id: root

    // [{label, value, runs, tone}]: `value` is the characters shown, `runs` a value's runs as a text's (empty for a
    // plain value), `tone` the `Tone` index (0 none).
    property var items: []
    // Whether each caption sits above its value; set once.
    property bool captionsAbove: false
    // The caption column's width in units; -1 sizes it to the widest caption. Set once.
    property int captionUnits: -1
    readonly property bool captionSet: captionUnits >= 0

    implicitWidth: captionsAbove ? stack.implicitWidth : grid.implicitWidth
    implicitHeight: captionsAbove ? stack.implicitHeight : grid.implicitHeight

    // The widest unbreakable segment of a pair's value, drawn in @p base with its runs in their faces; 0 until the
    // backend's measure is there.
    function valueMinimumOf(pair: var, base: font): real {
        if (!Theme.textMeasure || !pair)
            return 0
        const pieces = []
        if (pair.runs.length > 0) {
            for (const run of pair.runs)
                pieces.push({"text": root.plainOf(run.text), "font": root.runFontOf(base, Theme.families, run.strong === true, run.mono === true)})
        } else {
            pieces.push({"text": pair.value, "font": base})
        }
        return Theme.textMeasure.widestSegment(pieces)
    }
    // The widest of the values' minimums, each in the font the value is drawn in.
    readonly property real widestValueMinimum: {
        let widest = 0
        for (const pair of root.items)
            widest = Math.max(widest, root.valueMinimumOf(pair, pair.tone !== 0 ? Theme.strongFont : Theme.bodyFont))
        return widest
    }
    // The widest caption on one line, in the font it is drawn in. Reading the face's height makes a binding that
    // measures through here depend on the font: a method call alone registers no dependency.
    function widestCaption(): real {
        let widest = 0
        for (const pair of root.items)
            widest = Math.max(widest, captionMetrics.height < 0 ? 0 : captionMetrics.advanceWidth(pair.label))
        return widest
    }
    // Beside its values a caption is as wide as the caption column, a column and a gap of two units before the widest
    // value segment; above, the widest caption or value segment.
    contentMinimumWidth: {
        if (!Theme.textMeasure)
            return -1
        if (root.captionsAbove)
            return Math.max(root.widestValueMinimum, root.widestCaption())
        const column = root.captionSet ? root.captionUnits * Theme.unitWidth : root.widestCaption()
        return column + 2 * Theme.unitWidth + root.widestValueMinimum
    }
    FontMetrics {
        id: captionMetrics
        font: root.captionsAbove ? Theme.captionFont : Theme.bodyFont
    }

    // A pair's value: its runs or its text, toned or not.
    component Value: Label {
        required property var pair
        readonly property bool hasRuns: pair && pair.runs.length > 0
        textFormat: hasRuns ? Text.RichText : Text.PlainText
        text: !pair ? "" : hasRuns ? root.runsHtml(pair.runs) : pair.value
        font: pair && pair.tone !== 0 ? Theme.strongFont : Theme.bodyFont
        color: pair && pair.tone !== 0 ? root.toneOf(pair.tone) : root.scheme.text
        wrapMode: Text.Wrap
        // Never narrower than its widest unbreakable segment, whatever room the layout offers.
        Layout.minimumWidth: root.valueMinimumOf(pair, font)
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
                // binding cycle through the layout, which Qt reports as a polish loop. A set column width is a
                // fixed number of units, so a longer caption wraps at word boundaries in it without reading the grid.
                wrapMode: root.captionSet ? Text.Wrap : Text.NoWrap
                Layout.row: index
                Layout.column: 0
                // A caption shares its value's first baseline, whichever of the two wraps and whatever face the
                // value is drawn in; a top or centre alignment would put a mono or toned value's line off the caption's.
                Layout.alignment: Qt.AlignBaseline
                Layout.minimumWidth: root.captionSet ? root.captionUnits * Theme.unitWidth : implicitWidth
                Layout.preferredWidth: root.captionSet ? root.captionUnits * Theme.unitWidth : -1
                Layout.maximumWidth: root.captionSet ? root.captionUnits * Theme.unitWidth : -1
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
                Layout.alignment: Qt.AlignBaseline
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
                    font: Theme.captionFont
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
