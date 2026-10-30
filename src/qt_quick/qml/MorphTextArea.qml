// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A multi-line text input. Every user edit reports the whole text; focus leaving after an edit commits. Enter
// inserts a line break.
MorphField {
    id: root

    property string placeholder: ""
    // Whether the text and the placeholder are drawn in the mono face; set once.
    property bool mono: false
    property bool edited: false
    // Set while C++ writes the text, so the write is not taken for an edit.
    property bool assigning: false

    function assign(value: string) {
        if (area.text === value)
            return
        root.assigning = true
        area.text = value
        root.assigning = false
    }

    control: area
    accessibleItem: area
    defaultRole: Accessible.EditableText

    TextArea {
        font: root.mono ? Theme.monoFontOf(Theme.monoPixels, Font.Normal) : Theme.bodyFont
        id: area
        width: root.controlWidth
        // The widget's own chords come before the control's handling of the key.
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: event => event.accepted = root.takeOwnChord(event)
        color: root.scheme.text
        placeholderTextColor: root.scheme.muted
        // Each side set itself: the style's own left padding is wider than its others.
        leftPadding: Theme.inputPaddingX
        rightPadding: Theme.inputPaddingX
        // The first line sits where a single-line input's text sits.
        topPadding: Math.max(0, (Theme.targetHeight - lineMetrics.height) / 2)
        bottomPadding: topPadding
        // The area's natural width is its insets alone: the style's own also counts the wrapped text, which is as wide as
        // the area itself, so an area in a content-sized container would size from its own width.
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, leftPadding + rightPadding)
        FontMetrics {
            id: lineMetrics
            font: area.font
        }
        background: MorphInputBackground {
            tone: root.shownTone
            scheme: root.scheme
            focused: area.activeFocus
            readOnly: root.readOnly
            implicitHeight: Theme.targetHeight * 2
        }
        readOnly: root.readOnly
        placeholderText: root.placeholder
        wrapMode: TextEdit.Wrap
        // Declared, as the text input's field declares them.
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        onTextChanged: {
            if (root.assigning || root.readOnly)
                return
            root.edited = true
            root.fire("change", text)
        }
        onActiveFocusChanged: {
            if (!activeFocus && root.edited) {
                root.edited = false
                root.fire("commit", text)
            }
        }
    }
}
