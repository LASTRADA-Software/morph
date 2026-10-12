// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// An integer slider. A move reports the rounded value; a read-only slider takes focus and refuses every move.
MorphField {
    id: root

    // The value C++ last showed, which a refused move returns to.
    property real shownValue: 0

    function show(value: real) {
        root.shownValue = value
        slider.value = value
    }

    control: slider

    Slider {
        id: slider
        width: root.controlWidth
        onMoved: {
            if (root.readOnly) {
                value = root.shownValue
                return
            }
            root.shownValue = Math.round(value)
            root.fire("change", root.shownValue)
        }
    }
}
