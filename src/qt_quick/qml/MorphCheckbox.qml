// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A check box. A read-only one takes focus and refuses every toggle.
MorphField {
    id: root

    fillsAcross: false

    property string label: ""

    control: box
    accessibleItem: box
    defaultName: label
    defaultRole: Accessible.CheckBox

    CheckBox {
        font: Theme.bodyFont
        id: box
        width: root.controlWidth
        text: root.label
        palette.windowText: root.scheme.text
        indicator: Rectangle {
            x: box.leftPadding
            y: (box.height - height) / 2
            implicitWidth: Theme.unitHeight
            implicitHeight: Theme.unitHeight
            radius: Theme.radiusControl / 2
            color: box.checked ? root.scheme.accent : root.scheme.surface
            border.width: box.visualFocus ? 2 : 1
            border.color: box.visualFocus ? root.scheme.focus : box.checked ? root.scheme.accent : root.scheme.border

            Text {
                anchors.centerIn: parent
                visible: box.checked
                text: "✓"
                color: "white"
                font.pixelSize: parent.height * 0.75
            }
        }
        // Declared, as the text input's field declares them.
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        onToggled: {
            if (root.readOnly) {
                checked = !checked
                return
            }
            root.fire("toggle", checked)
        }
    }
}
