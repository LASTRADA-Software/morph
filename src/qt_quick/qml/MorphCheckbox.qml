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
        // No vertical padding of its own: the box is as tall as its label's line or its indicator, and the container's
        // gap spaces the rows. A touch target while the window is compact.
        topPadding: 0
        bottomPadding: 0
        // The box starts at the widget's edge and its label follows it by a step, so a column of check boxes lines up
        // with the labels above it; the indicator draws its focus ring inside its own extent.
        leftPadding: 0
        rightPadding: 0
        spacing: Theme.spaceOf(2)
        implicitHeight: Math.max(implicitContentHeight, implicitIndicatorHeight,
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
        indicator: MorphCheckIndicator {
            objectName: "boxIndicator"
            x: box.leftPadding
            y: (box.height - height) / 2
            scheme: root.scheme
            checked: box.checked
            focused: box.visualFocus
            // A toned box keeps its tone's border while focused; focus shows as the border's width.
            toned: root.shownTone !== 0
            toneColor: root.toneOf(root.shownTone)
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
