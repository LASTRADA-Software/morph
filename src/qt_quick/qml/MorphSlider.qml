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
