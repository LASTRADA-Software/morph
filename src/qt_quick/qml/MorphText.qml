// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// Text: a label in one of the text roles (Normal, Muted, Heading, Error, Success).
MorphField {
    id: root

    property string text: ""
    property int role: 0

    // The widget's item is the node a driver finds, so it reads the text; the label inside it is not a second node.
    defaultName: text
    defaultRole: Accessible.StaticText
    control: label

    Label {
        id: label
        width: root.controlWidth
        text: root.text
        wrapMode: Text.Wrap
        font: root.role === 2 ? Theme.headingFont : Theme.fontOf(Theme.bodyPixels, Font.Normal)
        color: root.textColorOf(root.role)
        Accessible.ignored: true
    }
}
