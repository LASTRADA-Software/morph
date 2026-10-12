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

    TextField {
        id: field
        width: root.controlWidth
        readOnly: root.readOnly
        placeholderText: root.withTime ? "yyyy-mm-dd hh:mm" : "yyyy-mm-dd"
        onTextEdited: root.edited = true
        onEditingFinished: {
            if (!root.edited || root.readOnly)
                return
            root.edited = false
            root.fire("edit", text)
        }
    }
}
