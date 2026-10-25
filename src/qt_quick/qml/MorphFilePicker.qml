// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// A file path with a button that opens the platform's file dialog, to open an existing file or to save one.
MorphField {
    id: root

    property bool save: false
    property string path: ""

    // Reports a path as the dialog would.
    function pick(chosen: string) {
        if (root.readOnly)
            return
        root.fire("picked", chosen)
    }

    control: row
    accessibleItem: pathField
    defaultRole: Accessible.EditableText

    RowLayout {
        id: row
        width: root.controlWidth

        // The path is the widget's node, declared as the text input's field declares it; the button beside it
        // is a node of its own.
        TextField {
            font: Theme.bodyFont
            id: pathField
            Layout.fillWidth: true
            color: root.scheme.text
            placeholderTextColor: root.scheme.muted
            leftPadding: Theme.spaceOf(3)
            rightPadding: Theme.spaceOf(3)
            background: MorphInputBackground {
                scheme: root.scheme
                focused: pathField.activeFocus
                readOnly: root.readOnly
            }
            readOnly: true
            text: root.path
            Accessible.role: root.accessibleRole
            Accessible.name: root.accessibleName
            Accessible.description: root.accessibleDescription
        }

        Button {
            font: Theme.bodyFont
            id: browse
            text: root.save ? qsTr("Save as…") : qsTr("Browse…")
            palette.buttonText: root.scheme.text
            background: MorphButtonBackground {
                scheme: root.scheme
                hovered: browse.hovered
                pressed: browse.down
                focused: browse.visualFocus
            }
            Accessible.role: Accessible.Button
            Accessible.name: text
            enabled: !root.readOnly
            onClicked: dialog.open()
        }
    }

    FileDialog {
        id: dialog
        fileMode: root.save ? FileDialog.SaveFile : FileDialog.OpenFile
        onAccepted: root.pick(selectedFile.toString())
    }
}
