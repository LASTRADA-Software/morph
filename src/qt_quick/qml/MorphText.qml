// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// Text: a label in one of the text roles (Normal, Muted, Heading, Error, Success, Section, Mono).
MorphField {
    id: root

    property string text: ""
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
        textFormat: Text.PlainText
        id: label
        width: root.controlWidth
        text: root.text
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
