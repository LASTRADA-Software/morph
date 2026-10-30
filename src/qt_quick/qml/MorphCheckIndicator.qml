// SPDX-License-Identifier: Apache-2.0
import QtQuick

// A check box's indicator: a small rounded square outlined in the control border while unchecked, filled with the
// accent and marked with a check while checked, and with a dash while partly checked (a select-all box over a
// partial selection). A focused box has a wider outline in the focus colour, and a toned one keeps its tone's.
Rectangle {
    id: root

    // The colour roles of the surface the box is on.
    property MorphScheme scheme: Theme.themeScheme
    property bool checked: false
    // Whether only some of what the box stands for is checked.
    property bool partial: false
    property bool focused: false
    // Whether the box is outlined in a tone, and that tone's colour; focus shows as the outline's width then.
    property bool toned: false
    property color toneColor: "transparent"

    readonly property bool marked: checked || partial

    implicitWidth: Theme.unitHeight
    implicitHeight: Theme.unitHeight
    radius: Theme.radiusControl / 2
    color: marked ? scheme.accent : scheme.controlSurface
    border.width: focused ? 2 : 1
    border.color: toned ? toneColor
                        : focused ? scheme.focus : marked ? scheme.accent : scheme.controlBorder

    Text {
        objectName: "mark"
        textFormat: Text.PlainText
        anchors.centerIn: parent
        visible: root.marked
        text: root.partial && !root.checked ? "–" : "✓"
        color: "white"
        font.pixelSize: parent.height * 0.75
    }
}
