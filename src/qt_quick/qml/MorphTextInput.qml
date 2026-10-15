// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A single-line text input, plain or masked. Every user edit reports the whole text; Enter commits and then
// submits; focus leaving after an edit commits.
MorphField {
    id: root

    property bool password: false
    property string placeholder: ""
    // Whether the user edited the text since it was last committed.
    property bool edited: false

    control: field

    TextField {
        id: field
        width: root.controlWidth
        readOnly: root.readOnly
        placeholderText: root.placeholder
        echoMode: root.password ? TextInput.Password : TextInput.Normal
        // Declared, not left to Qt Quick Controls, so the field is a node whether or not accessibility is active.
        Accessible.role: Accessible.EditableText
        onTextEdited: {
            root.edited = true
            root.fire("change", text)
        }
        onAccepted: {
            if (root.readOnly)
                return
            root.edited = false
            root.fire("commit", text)
            root.fire("submit", text)
        }
        onActiveFocusChanged: {
            if (!activeFocus && root.edited) {
                root.edited = false
                root.fire("commit", text)
            }
        }
    }
}
