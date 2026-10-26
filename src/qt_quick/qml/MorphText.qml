// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// Text: a label in one of the text roles (Normal, Muted, Heading, Error, Success, Section, Mono).
MorphField {
    id: root

    // The characters shown: the text, or a text's runs joined, which assistive technology reads.
    property string text: ""
    // A text's runs as escaped rich text, built in C++; while it is not empty the label shows it, and a plain text
    // never goes through rich text.
    property string richText: ""
    property int role: 0
    // Where the text sits across the widget: a table sets it from its column's `align`.
    property int textAlign: Qt.AlignLeft
    // Whether digits take one width each, so a column of numbers lines up.
    property bool tabularFigures: false

    // The widget's item is the node a driver finds, so it reads the text; the label inside it is not a second node.
    defaultName: text
    defaultRole: Accessible.StaticText
    control: label

    Label {
        id: label
        textFormat: root.richText !== "" ? Text.RichText : Text.PlainText
        width: root.controlWidth
        text: root.richText !== "" ? root.richText : root.text
        wrapMode: Text.Wrap
        horizontalAlignment: root.textAlign
        font: Theme.figuresOf(root.role === 2 ? Theme.headingFont
                                              : root.role === 5 ? Theme.strongFont
                                                                : root.role === 6 ? Theme.monoFont
                                                                                  : Theme.fontOf(Theme.bodyPixels,
                                                                                                 Font.Normal),
                              root.role === 6 ? Theme.monoFamilies : Theme.families, root.tabularFigures)
        color: root.textColorOf(root.role)
        Accessible.ignored: true
    }
}
