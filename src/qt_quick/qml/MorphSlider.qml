// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// An integer slider. A move reports the rounded value; a read-only slider takes focus and refuses every move.
MorphField {
    id: root

    // The value C++ last showed, which a refused move returns to.
    property real shownValue: 0
    // Whether the value moved during the press that is still down, so its release commits.
    property bool movedWhilePressed: false

    function show(value: real) {
        root.shownValue = value
        slider.value = value
    }

    control: slider
    accessibleItem: slider
    defaultRole: Accessible.Slider

    Slider {
        id: slider
        width: root.controlWidth
        // The style's handle, with its border in the field's tone while it has one.
        handle: Rectangle {
            objectName: "handle"
            x: slider.leftPadding + (slider.horizontal ? slider.visualPosition * (slider.availableWidth - width)
                                                       : (slider.availableWidth - width) / 2)
            y: slider.topPadding + (slider.horizontal ? (slider.availableHeight - height) / 2
                                                      : slider.visualPosition * (slider.availableHeight - height))
            implicitWidth: 28
            implicitHeight: 28
            radius: width / 2
            color: slider.pressed ? slider.palette.light : slider.palette.window
            border.width: slider.visualFocus ? 2 : 1
            border.color: root.shownTone !== 0 ? root.toneOf(root.shownTone)
                                               : slider.activeFocus ? slider.palette.highlight : slider.palette.mid
        }
        // The widget's own chords come before the control's handling of the key.
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: event => event.accepted = root.takeOwnChord(event)
        // Declared, as the text input's field declares them.
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        onMoved: {
            if (root.readOnly) {
                value = root.shownValue
                return
            }
            root.shownValue = Math.round(value)
            root.fire("change", root.shownValue)
            if (slider.pressed) {
                root.movedWhilePressed = true
            } else {
                root.fire("commit", root.shownValue)
            }
        }
        onPressedChanged: {
            if (!pressed && root.movedWhilePressed) {
                root.movedWhilePressed = false
                root.fire("commit", root.shownValue)
            }
        }
    }
}
