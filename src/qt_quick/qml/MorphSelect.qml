// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// A choice among labelled options, as a drop-down, as radio buttons, or as one row of joined segments. The keys stay
// in C++: QML sees the labels and reports the index the user chose.
MorphField {
    id: root

    property bool radio: false
    // Every option a segment of one row, the marked one raised; fixed at creation.
    property bool segmented: false
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

    control: root.segmented ? segments : root.radio ? radios : combo
    accessibleItem: control
    // A drop-down the document names no name reads the option it shows; a radio group or a segmented control is a
    // group of its options (Qt has no radio-group role).
    defaultName: root.radio || root.segmented ? "" : combo.displayText
    defaultRole: root.radio || root.segmented ? Accessible.Grouping : Accessible.ComboBox

    // Left and Right on a segmented select choose the previous or the next option, and stop at the ends.
    function step(by: int): bool {
        const next = root.marked + by
        if (root.marked < 0 || next < 0 || next >= root.options.length)
            return root.marked >= 0
        root.choose(next)
        return true
    }

    ComboBox {
        font: Theme.bodyFont
        id: combo
        visible: !root.radio && !root.segmented
        // The widget's own chords come before the control's handling of the key.
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: event => event.accepted = root.takeOwnChord(event)
        width: root.controlWidth
        palette.buttonText: root.scheme.text
        palette.text: root.scheme.text
        background: MorphInputBackground {
            tone: root.shownTone
            scheme: root.scheme
            focused: combo.visualFocus || combo.popup.visible
            readOnly: root.readOnly
        }
        indicator: Image {
            objectName: "indicator"
            x: combo.width - width - Theme.spaceOf(2)
            y: (combo.height - height) / 2
            readonly property string chevron: combo.popup.visible ? "nav.collapse" : "nav.expand"
            readonly property string themedSource: Theme.iconSource(chevron)
            source: themedSource !== "" ? themedSource : ""

            Text {
                textFormat: Text.PlainText
                // The chevron the theme draws as a path when it names neither nav icon.
                anchors.centerIn: parent
                visible: parent.themedSource === ""
                text: combo.popup.visible ? "▴" : "▾"
                color: root.scheme.text
            }
        }
        model: root.options
        currentIndex: -1
        // The style's delegate draws an option's label as an IconLabel, which reads markup.
        delegate: ItemDelegate {
            id: option
            required property int index
            required property string modelData
            width: ListView.view ? ListView.view.width : implicitWidth
            text: modelData
            font.family: combo.font.family
            font.pixelSize: combo.font.pixelSize
            font.weight: combo.currentIndex === index ? Font.DemiBold : Font.Normal
            highlighted: combo.highlightedIndex === index
            hoverEnabled: combo.hoverEnabled
            contentItem: MorphPlainContent {
                control: option
                color: option.palette.text
                alignment: Qt.AlignLeft
            }
        }
        // Declared, as the text input's field declares them.
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        // A hidden item is still in the accessibility tree: only the presentation shown is the widget's node.
        Accessible.ignored: root.radio || root.segmented
        onActivated: index => root.choose(index)

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
        // The widget's own chords come before the control's handling of the key.
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: event => event.accepted = root.takeOwnChord(event)
        width: root.controlWidth
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        Accessible.ignored: !root.radio

        Repeater {
            id: radioButtons
            model: root.options

            RadioButton {
                id: radioButton
                font: Theme.bodyFont
                required property int index
                required property string modelData
                text: modelData
                // The style's indicator, with its border in the field's tone while it has one.
                indicator: Rectangle {
                    objectName: "radioIndicator"
                    implicitWidth: 28
                    implicitHeight: 28
                    x: radioButton.leftPadding
                    y: radioButton.topPadding + (radioButton.availableHeight - height) / 2
                    radius: width / 2
                    color: radioButton.down ? radioButton.palette.light : radioButton.palette.base
                    border.width: radioButton.visualFocus ? 2 : 1
                    border.color: root.shownTone !== 0 ? root.toneOf(root.shownTone)
                                                       : radioButton.visualFocus ? radioButton.palette.highlight
                                                                                 : radioButton.palette.mid

                    Rectangle {
                        x: (parent.width - width) / 2
                        y: (parent.height - height) / 2
                        width: 20
                        height: 20
                        radius: width / 2
                        color: radioButton.palette.text
                        visible: radioButton.checked
                    }
                }
                contentItem: Text {
                    leftPadding: radioButton.indicator.width + radioButton.spacing
                    text: radioButton.text
                    textFormat: Text.PlainText
                    font: radioButton.font
                    color: radioButton.palette.windowText
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                // Each option is a node of its own, named by its label, that a driver presses.
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.ignored: !root.radio
                onClicked: root.choose(index)
            }
        }
    }

    // One Tab stop, entered at the marked option: the segments take no focus of their own.
    Rectangle {
        id: segments
        objectName: "segments"
        visible: root.segmented
        activeFocusOnTab: root.segmented
        width: root.widthKind === 2 ? root.controlWidth : implicitWidth
        implicitWidth: segmentRow.implicitWidth + 4
        implicitHeight: Theme.touchTargets ? Theme.targetHeight : Theme.targetHeight - 8
        radius: Theme.radiusControl
        color: root.scheme.track
        border.width: root.shownTone !== 0 || activeFocus ? (activeFocus ? 2 : 1) : 0
        border.color: root.shownTone !== 0 ? root.toneOf(root.shownTone) : root.scheme.focus
        Accessible.role: root.accessibleRole
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription
        Accessible.ignored: !root.segmented
        Keys.onPressed: event => {
            if (root.takeOwnChord(event))
                event.accepted = true
            else if (event.key === Qt.Key_Left && event.modifiers === Qt.NoModifier)
                event.accepted = root.step(-1)
            else if (event.key === Qt.Key_Right && event.modifiers === Qt.NoModifier)
                event.accepted = root.step(1)
        }

        Row {
            id: segmentRow
            x: 2
            y: 2
            height: segments.height - 4

            Repeater {
                id: segmentButtons
                model: root.segmented ? root.options : []

                Button {
                    id: segment
                    objectName: "segment"
                    required property int index
                    required property string modelData
                    readonly property bool marked: index === root.marked
                    focusPolicy: Qt.NoFocus
                    height: segmentRow.height
                    // A stretched control shares its width equally among its segments.
                    width: root.widthKind === 2 && root.options.length > 0
                           ? (segments.width - 4) / root.options.length : implicitWidth
                    leftPadding: Theme.spaceOf(3)
                    rightPadding: Theme.spaceOf(3)
                    topPadding: 0
                    bottomPadding: 0
                    text: modelData
                    font: segment.marked ? Theme.strongFont : Theme.bodyFont
                    contentItem: MorphPlainContent {
                        control: segment
                        color: segment.marked ? root.scheme.text : root.scheme.muted
                    }
                    background: Rectangle {
                        radius: Theme.radiusControl - 1
                        color: segment.marked ? root.scheme.surface : "transparent"
                        border.width: segment.marked ? 1 : 0
                        border.color: root.scheme.border
                    }
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData
                    Accessible.checked: marked
                    Accessible.ignored: !root.segmented
                    onClicked: root.choose(index)
                }
            }
        }
    }
}
