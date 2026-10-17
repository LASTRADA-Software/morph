// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A multi-line text input. Every user edit reports the whole text; focus leaving after an edit commits. Enter
// inserts a line break.
MorphField {
    id: root

    property string placeholder: ""
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
        id: area
        width: root.controlWidth
        color: root.scheme.text
        placeholderTextColor: root.scheme.muted
        padding: Theme.spaceOf(3)
        background: MorphInputBackground {
            scheme: root.scheme
            focused: area.activeFocus
            implicitHeight: Theme.controlHeight * 2
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
