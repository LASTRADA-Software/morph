// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A date, or a date and a time, typed in the display zone as `yyyy-MM-dd` or `yyyy-MM-dd HH:mm`. C++ formats and
// parses it; an edit is reported once the user finishes it, and an empty field is a cleared value.
MorphField {
    id: root

    property bool withTime: false
    property bool edited: false

    control: field
    accessibleItem: field
    defaultRole: Accessible.EditableText

    TextField {
        id: field
        width: root.controlWidth
        color: root.scheme.text
        placeholderTextColor: root.scheme.muted
        leftPadding: Theme.spaceOf(3)
        rightPadding: Theme.spaceOf(3)
        background: MorphInputBackground {
            scheme: root.scheme
            focused: field.activeFocus
        }
        readOnly: root.readOnly
        placeholderText: root.withTime ? "yyyy-mm-dd hh:mm" : "yyyy-mm-dd"
        // Declared, as the text input's field declares them.
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        onTextEdited: root.edited = true
        onEditingFinished: {
            if (!root.edited || root.readOnly)
                return
            root.edited = false
            root.fire("edit", text)
        }
    }
}
