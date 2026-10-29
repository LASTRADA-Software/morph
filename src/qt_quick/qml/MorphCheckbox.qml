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
        // A touch target while the window is compact; the indicator keeps its size.
        implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                                 implicitIndicatorHeight + topPadding + bottomPadding,
                                 Theme.touchTargets ? Theme.targetHeight : 0)
        text: root.label
        palette.windowText: root.scheme.text
        contentItem: Text {
            leftPadding: box.indicator.width + box.spacing
            text: box.text
            textFormat: Text.PlainText
            font: box.font
            color: box.palette.windowText
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        indicator: Rectangle {
            objectName: "boxIndicator"
            x: box.leftPadding
            y: (box.height - height) / 2
            implicitWidth: Theme.unitHeight
            implicitHeight: Theme.unitHeight
            radius: Theme.radiusControl / 2
            color: box.checked ? root.scheme.accent : root.scheme.controlSurface
            border.width: box.visualFocus ? 2 : 1
            // A toned box keeps its tone's border while focused; focus shows as the border's width.
            border.color: root.shownTone !== 0 ? root.toneOf(root.shownTone)
                                               : box.visualFocus ? root.scheme.focus
                                                                 : box.checked ? root.scheme.accent : root.scheme.controlBorder

            Text {
                textFormat: Text.PlainText
                anchors.centerIn: parent
                visible: box.checked
                text: "✓"
                color: "white"
                font.pixelSize: parent.height * 0.75
            }
        }
        // The widget's own chords come before the control's handling of the key.
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: event => event.accepted = root.takeOwnChord(event)
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
