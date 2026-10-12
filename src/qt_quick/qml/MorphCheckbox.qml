// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A check box. A read-only one takes focus and refuses every toggle.
MorphField {
    id: root

    property string label: ""

    control: box

    CheckBox {
        id: box
        width: root.controlWidth
        text: root.label
        onToggled: {
            if (root.readOnly) {
                checked = !checked
                return
            }
            root.fire("toggle", checked)
        }
    }
}
