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
    // Text drawn muted at the leading edge inside the field, after the icon; empty draws none.
    property string prefix: ""
    // Whether the text and the placeholder are drawn in the mono face; set once.
    property bool mono: false
    // The screen's focal field, such as a scanner's: larger text, the touch height, and an accent outline; set once.
    property bool prominent: false
    announcedUnit: unit
    announcedPrefix: prefix
    // The fraction digits a number is entered with, -1 for an input that holds no number: carried for a kit's
    // restyle to read; the stock input draws nothing from it.
    property int decimals: -1
    // The name of an icon in the theme's icon set, drawn at the leading edge inside the field; empty draws none.
    property string icon: ""
    // Whether the widget has a submit handler. Only then does Enter stop at the field; otherwise the field commits
    // and Enter goes on to the ancestors' chords, a dialog's default among them.
    property bool submits: false
    // Where the text sits across the field: a table sets it from its column's `align`.
    property int textAlign: Qt.AlignLeft
    // Whether digits take one width each, so a column of numbers lines up.
    property bool tabularFigures: false

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

    // The text's font: the heading size at the theme's strong weight for the focal field, else the body size; in the
    // mono role's face when asked. A weight is always the theme's, never a literal one, since a family without it
    // may match another face.
    readonly property font textFont: {
        if (root.prominent)
            return root.mono ? Theme.monoFontOf(Theme.headingPixels, Theme.strongWeight)
                             : Theme.fontOf(Theme.headingPixels, Theme.strongWeight)
        if (root.mono)
            return Theme.figuresOf(Theme.monoFontOf(Theme.monoPixels, Font.Normal), Theme.monoFamilies,
                                   root.tabularFigures)
        return Theme.figuresOf(Theme.bodyFont, Theme.families, root.tabularFigures)
    }

    TextField {
        font: root.textFont
        id: field
        width: root.controlWidth
        horizontalAlignment: root.textAlign
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
        // The text starts after the icon and the prefix.
        leftPadding: Theme.spaceOf(3) + (iconImage.visible ? iconImage.width + Theme.spaceOf(1) : 0) +
                     (prefixLabel.visible ? prefixLabel.implicitWidth + Theme.spaceOf(1) : 0)
        // The text stops before the unit.
        rightPadding: Theme.spaceOf(3) + (unitLabel.visible ? unitLabel.implicitWidth + Theme.spaceOf(1) : 0)
        background: MorphInputBackground {
            tone: root.shownTone
            prominent: root.prominent
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
            textFormat: Text.PlainText
            id: unitLabel
            objectName: "unit"
            anchors.right: parent.right
            anchors.rightMargin: Theme.spaceOf(3)
            anchors.verticalCenter: parent.verticalCenter
            visible: root.unit !== ""
            text: root.unit
            color: root.scheme.muted
        }

        Label {
            textFormat: Text.PlainText
            id: prefixLabel
            objectName: "prefix"
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceOf(3) + (iconImage.visible ? iconImage.width + Theme.spaceOf(1) : 0)
            anchors.verticalCenter: parent.verticalCenter
            visible: root.prefix !== ""
            text: root.prefix
            font: root.textFont
            color: root.scheme.muted
        }

        Image {
            id: iconImage
            objectName: "icon"
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceOf(3)
            anchors.verticalCenter: parent.verticalCenter
            readonly property string themedSource: Theme.iconSourceIn(root.icon, root.scheme.accent)
            visible: root.icon !== "" && themedSource !== ""
            source: themedSource
        }
    }
}
