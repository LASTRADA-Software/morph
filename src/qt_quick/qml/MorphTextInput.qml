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
    // The unit, drawn muted at the trailing edge inside the field; empty draws none.
    property string unit: ""
    // The fraction digits a number is entered with, -1 for an input that holds no number: carried for a kit's
    // restyle to read; the stock input draws nothing from it.
    property int decimals: -1

    control: field
    accessibleItem: field
    defaultRole: Accessible.EditableText

    TextField {
        id: field
        width: root.controlWidth
        color: root.scheme.text
        placeholderTextColor: root.scheme.muted
        leftPadding: Theme.spaceOf(3)
        // The text stops before the unit.
        rightPadding: Theme.spaceOf(3) + (unitLabel.visible ? unitLabel.implicitWidth + Theme.spaceOf(1) : 0)
        background: MorphInputBackground {
            scheme: root.scheme
            focused: field.activeFocus
        }
        readOnly: root.readOnly
        placeholderText: root.placeholder
        echoMode: root.password ? TextInput.Password : TextInput.Normal
        // Declared on the control, which Qt Quick Controls makes an accessible node of its own accord only while the
        // platform's accessibility is active: with a name declared the field is the widget's node either way, in
        // the role the document names or else its own.
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
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

        Label {
            id: unitLabel
            objectName: "unit"
            anchors.right: parent.right
            anchors.rightMargin: Theme.spaceOf(3)
            anchors.verticalCenter: parent.verticalCenter
            visible: root.unit !== ""
            text: root.unit
            color: root.scheme.muted
        }
    }
}
