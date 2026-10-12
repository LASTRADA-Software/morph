// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// Text: a label in one of the text roles (Normal, Muted, Heading, Error, Success).
MorphField {
    id: root

    property string text: ""
    property int role: 0

    control: label

    Label {
        id: label
        width: root.controlWidth
        text: root.text
        wrapMode: Text.Wrap
        font.bold: root.role === 2
        font.pixelSize: root.role === 2 ? Math.round(Theme.unitHeight * 1.2) : Theme.metrics.font.pixelSize
        color: Theme.textColor(root.role)
    }
}
