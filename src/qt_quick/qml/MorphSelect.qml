// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A choice among labelled options, as a drop-down or as radio buttons. The keys stay in C++: QML sees the labels
// and reports the index the user chose.
MorphField {
    id: root

    property bool radio: false
    property list<string> options: []
    // The option shown as chosen, or -1.
    property int marked: -1

    // Shows option `index` as chosen, or none for -1.
    function mark(index: int) {
        root.marked = index
        combo.currentIndex = index
        for (let i = 0; i < radioButtons.count; ++i) {
            const button = radioButtons.itemAt(i) as RadioButton
            if (button)
                button.checked = i === index
        }
    }

    function choose(index: int) {
        if (root.readOnly) {
            root.mark(root.marked)
            return
        }
        root.marked = index
        root.fire("select", index)
    }

    control: root.radio ? radios : combo

    ComboBox {
        id: combo
        visible: !root.radio
        width: root.controlWidth
        model: root.options
        currentIndex: -1
        onActivated: index => root.choose(index)
    }

    Column {
        id: radios
        visible: root.radio
        width: root.controlWidth

        Repeater {
            id: radioButtons
            model: root.options

            RadioButton {
                required property int index
                required property string modelData
                text: modelData
                onClicked: root.choose(index)
            }
        }
    }
}
