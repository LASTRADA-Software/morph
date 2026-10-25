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
    // The name of an icon in the theme's icon set, drawn at the leading edge inside the field; empty draws none.
    property string icon: ""
    // Whether the widget has a submit handler. Only then does Enter stop at the field; otherwise the field commits
    // and Enter goes on to the ancestors' chords, a dialog's default among them.
    property bool submits: false

    // Return or Enter: the widget's own chord first; then a field that submits commits, submits and takes the key;
    // otherwise the TextField takes it, commits through `accepted` and lets it go on.
    function enterPressed(event: var) {
        if (root.takeOwnChord(event))
            return
        if (!root.submits) {
            event.accepted = false
            return
        }
        if (root.readOnly)
            return
        root.edited = false
        root.fire("commit", field.text)
        root.fire("submit", field.text)
    }

    control: field
    accessibleItem: field
    defaultRole: Accessible.EditableText

    TextField {
        font: Theme.bodyFont
        id: field
        width: root.controlWidth
        Keys.priority: Keys.BeforeItem
        Keys.onReturnPressed: event => root.enterPressed(event)
        Keys.onEnterPressed: event => root.enterPressed(event)
        // Every other key: the widget's own chords come before the field's handling of it.
        Keys.onPressed: event => {
            if (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter)
                event.accepted = root.takeOwnChord(event)
        }
        color: root.scheme.text
        placeholderTextColor: root.scheme.muted
        // The text starts after the icon.
        leftPadding: Theme.spaceOf(3) + (iconImage.visible ? iconImage.width + Theme.spaceOf(1) : 0)
        // The text stops before the unit.
        rightPadding: Theme.spaceOf(3) + (unitLabel.visible ? unitLabel.implicitWidth + Theme.spaceOf(1) : 0)
        background: MorphInputBackground {
            scheme: root.scheme
            focused: field.activeFocus
            readOnly: root.readOnly
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

        Image {
            id: iconImage
            objectName: "icon"
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceOf(3)
            anchors.verticalCenter: parent.verticalCenter
            readonly property string themedSource: Theme.iconSource(root.icon)
            visible: root.icon !== "" && themedSource !== ""
            source: themedSource
        }
    }
}
