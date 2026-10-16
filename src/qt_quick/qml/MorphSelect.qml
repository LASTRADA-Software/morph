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
    accessibleItem: control
    // A drop-down the document names no name reads the option it shows; a radio group is a group of its options.
    defaultName: root.radio ? "" : combo.displayText
    defaultRole: root.radio ? Accessible.Grouping : Accessible.ComboBox

    ComboBox {
        id: combo
        visible: !root.radio
        width: root.controlWidth
        model: root.options
        currentIndex: -1
        // Declared, as the text input's field declares them.
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        // A hidden item is still in the accessibility tree: only the presentation shown is the widget's node.
        Accessible.ignored: root.radio
        onActivated: index => root.choose(index)
        // A ComboBox node has no value of its own, so the option it shows is read as the value of its display child,
        // the style's text field. Declared here, as the name is, for a platform whose accessibility never turns on.
        onContentItemChanged: combo.exposeShown()
        Component.onCompleted: combo.exposeShown()

        function exposeShown() {
            const shown = combo.contentItem
            if (!shown)
                return
            shown.Accessible.role = Accessible.EditableText
            shown.Accessible.ignored = Qt.binding(() => root.radio)
        }

        // Reported also with no options, so a failed fetch of them can be retried when the user asks for them.
        Connections {
            target: combo.popup
            function onOpened() {
                root.fire("open", 0)
            }
        }
    }

    Column {
        id: radios
        visible: root.radio
        width: root.controlWidth
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        Accessible.ignored: !root.radio

        Repeater {
            id: radioButtons
            model: root.options

            RadioButton {
                required property int index
                required property string modelData
                text: modelData
                // Each option is a node of its own, named by its label, that a driver presses.
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.ignored: !root.radio
                onClicked: root.choose(index)
            }
        }
    }
}
