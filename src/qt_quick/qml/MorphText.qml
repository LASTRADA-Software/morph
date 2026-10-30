// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// Text: a label in one of the text roles (Normal, Muted, Heading, Error, Success, Section, Mono, Display, Small, Overline).
MorphField {
    id: root

    // The characters shown: the text, or a text's runs joined, which assistive technology reads.
    property string text: ""
    // A text's runs, `{text, strong, mono, tone}` with each text escaped; while there are any, the label shows their
    // markup (`runsHtml`), and a plain text never goes through rich text.
    property var runs: []
    readonly property bool hasRuns: runs.length > 0
    // Whether the text is drawn on one line and elided at its end, as a table sets it for a column that does not wrap.
    // A mono text is always one line: an identifier broken across lines reads as two. Runs are rich text, which is
    // not elided, so they wrap with their sentence.
    property bool singleLine: false
    readonly property bool oneLine: (singleLine || role === 6) && !hasRuns
    property int role: 0
    // Where the text sits across the widget: a table sets it from its column's `align`.
    property int textAlign: Qt.AlignLeft
    // Whether digits take one width each, so a column of numbers lines up.
    property bool tabularFigures: false

    // The widget's item is the node a driver finds, so it reads the text; the label inside it is not a second node.
    defaultName: text
    defaultRole: Accessible.StaticText
    // A one-line text cut short of its text with an ellipsis, by the label's font: the label itself is told about
    // each line it lays out, which keeps it from eliding.
    readonly property string fitted: root.oneLine ? metrics.elidedText(root.text, Qt.ElideRight, root.controlWidth)
                                                  : root.text
    // Whether the text is shown cut short.
    readonly property bool elided: root.fitted !== root.text
    // A one-line text asks for the width of its whole text, not of what it shows, so a column sized by its widest cell
    // is not narrowed by the cut it makes in that cell. It reads the advance alone: the label's own width follows
    // `fitted`, which follows the width a layout derives from this one, and reading it would close that loop.
    // A text that wraps asks for the width of its longest line, read from the font's advances for the same reason: the
    // label's own implicit width follows the width it is given, which on Qt 6.8 closes a loop through a required
    // text's mark. Spans are rich text, whose lines the label alone can measure.
    // The label lays a line out in a pixel more than its advance, so a wrapping text given exactly its advance would
    // break its last word: a text asks for the advance plus this.
    readonly property real wrapSlack: 1
    // A text has no caption, so this is the field's width with the mark that follows the text.
    implicitWidth: (root.oneLine ? metrics.advanceWidth(root.text)
                    : root.hasRuns ? label.implicitWidth
                                   : root.text.split("\n").reduce(
                                         (widest, line) => Math.max(widest, metrics.advanceWidth(line)), 0) +
                                     root.wrapSlack) +
                   (root.required ? root.markerRoom : 0)
    // While the text is elided, its full text is the tooltip, unless the node gives its own.
    tooltipText: root.tooltip !== "" ? root.tooltip : root.elided ? root.text : ""
    control: label
    // A required text's mark follows the end of its last line. Rich text reports no lines, so for spans the mark
    // follows the widest line, on the last one.
    markerFollowsText: true
    textEndX: root.hasRuns ? label.contentWidth : label.lastLineEnd
    textEndY: root.hasRuns ? Math.max(0, label.contentHeight - metrics.height) : label.lastLineY

    FontMetrics {
        id: metrics
        font: label.font
    }

    Label {
        id: label
        textFormat: root.hasRuns ? Text.RichText : Text.PlainText
        width: root.controlWidth
        text: root.hasRuns ? root.runsHtml(root.runs) : root.fitted
        wrapMode: root.oneLine ? Text.NoWrap : Text.Wrap
        horizontalAlignment: root.textAlign
        font: Theme.figuresOf(root.role === 2 ? Theme.headingFont
                                              : root.role === 5 ? Theme.strongFont
                                                                : root.role === 6 ? Theme.monoFont
                                                                : root.role === 7 ? Theme.displayFont
                                                                : root.role === 8 ? Theme.smallFont
                                                                : root.role === 9 ? Theme.overlineFont
                                                                                  : Theme.fontOf(Theme.bodyPixels,
                                                                                                 Font.Normal),
                              root.role === 6 ? Theme.monoFamilies : Theme.families, root.tabularFigures)
        // A tone is the text's colour, whatever its role; the role keeps the size, weight and face.
        color: root.tone !== 0 ? root.toneOf(root.tone) : root.textColorOf(root.role)
        Accessible.ignored: true
        // Where the last line laid out ends, and where it sits.
        property real lastLineEnd: 0
        property real lastLineY: 0
        onLineLaidOut: line => {
            if (line.isLast) {
                label.lastLineEnd = line.x + line.implicitWidth
                label.lastLineY = line.y
            }
        }
    }
}
