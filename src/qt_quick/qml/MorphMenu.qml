// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A menu bar: one button per top-level entry; or, with `buttonStyle`, one button that opens the top level in a popup.
// An entry with a submenu opens it in the popup, one level at a time; an entry without one is chosen, and the menu
// reports its path, its index at each level from the top, separators counted. A separator is a line that is never
// chosen; a description is a muted line under its entry.
MorphWidget {
    id: root

    // The entries: [{label, icon, keys, checked (null, true or false), enabled, separator, description, items: [...]}].
    property var entries: []
    // Whether the menu is one button that opens its entries; set once, at creation.
    property bool buttonStyle: false
    // The trigger of a button-style menu: its label, icon name and emphasis (0 normal, 1 primary, 2 danger, 3 ghost).
    property string triggerLabel: ""
    property string triggerIcon: ""
    property int triggerEmphasis: 0
    // What the trigger shows as text: its label; for an icon-only trigger nothing when the theme draws the icon, the
    // glyph of the framework's `more` (`⋯`) and `nav.menu` (`☰`), and otherwise its accessible name, so it is never
    // blank.
    readonly property string triggerText: {
        if (root.triggerLabel !== "")
            return root.triggerLabel
        if (root.triggerIcon !== "" && Theme.iconSource(root.triggerIcon) !== "")
            return ""
        if (root.triggerIcon === "more")
            return "⋯"
        return root.triggerIcon === "nav.menu" ? "☰" : root.accessibleName
    }
    // A button-style menu's item is its one accessible node, as a button's is: it carries the trigger's name, the
    // menu-button role and the press action, and the control inside it is not a second node.
    defaultName: root.triggerLabel
    defaultRole: root.buttonStyle ? Accessible.ButtonMenu : Accessible.Client
    Accessible.onPressAction: {
        if (root.buttonStyle)
            root.openFromTrigger()
    }
    // The control the popup opens from: the trigger of a button-style menu, else the bar's entry that was chosen.
    property Item openFrom: null
    // The path of the submenu the popup shows.
    property var openPath: []
    readonly property var openEntries: root.entriesAt(root.openPath)

    function entriesAt(path: var): var {
        let level = root.entries
        for (const index of path) {
            if (!level || index < 0 || index >= level.length)
                return []
            level = level[index].items
        }
        return level ? level : []
    }

    // Activates the entry at `path` as a click on it would, opening a button-style menu from its trigger and each
    // submenu on the way. A path that names no entry, a separator, an entry with a submenu, or an entry that is
    // disabled or inside a disabled one runs nothing.
    function choose(path: var) {
        if (root.buttonStyle)
            root.openFromTrigger()
        let level = root.entries
        for (let depth = 0; depth < path.length; ++depth) {
            const index = path[depth]
            if (!level || index < 0 || index >= level.length || !level[index].enabled || level[index].separator)
                return
            if (depth + 1 < path.length) {
                root.open(path.slice(0, depth + 1))
                level = level[index].items
            } else if (level[index].items && level[index].items.length > 0) {
                return
            }
        }
        popup.close()
        root.fire("activate", path)
    }

    // A button-style menu opens its top level from the trigger, as a click on it does.
    function openFromTrigger() {
        root.openPath = []
        if (!popup.visible)
            popup.open()
    }

    function open(path: var) {
        root.openPath = path
        if (!popup.visible)
            popup.open()
    }

    // Places the popup under the control it opens from.
    function placePopup() {
        placement.place(popup, root.buttonStyle ? trigger : root.openFrom ?? bar)
    }

    function entryText(entry: var): string {
        const mark = entry.checked === true ? "✓ " : entry.checked === false ? "  " : ""
        const more = entry.items && entry.items.length > 0 ? " ›" : ""
        // An icon the theme's set names is drawn as the entry's icon; another is its name, as text.
        const named = entry.icon && Theme.iconSource(entry.icon) === "" ? entry.icon + " " : ""
        return mark + named + entry.label + more
    }

    focusTarget: root.buttonStyle ? trigger : bar
    implicitWidth: root.buttonStyle ? trigger.implicitWidth : bar.implicitWidth
    implicitHeight: root.buttonStyle ? trigger.implicitHeight : bar.implicitHeight

    Button {
        id: trigger
        anchors.fill: parent
        visible: root.buttonStyle
        text: root.triggerText
        leftPadding: root.triggerEmphasis === 3 ? Theme.spaceOf(1) : Theme.spaceOf(4)
        rightPadding: root.triggerEmphasis === 3 ? Theme.spaceOf(1) : Theme.spaceOf(4)
        palette.buttonText: root.triggerEmphasis === 3 ? root.scheme.accent
                            : root.triggerEmphasis === 0 ? root.scheme.text : "white"
        font: root.triggerEmphasis === 0 || root.triggerEmphasis === 3 ? Theme.bodyFont : Theme.strongFont
        icon.source: Theme.iconSource(root.triggerIcon)
        icon.color: "transparent"
        contentItem: MorphPlainContent {
            control: trigger
            color: trigger.palette.buttonText
        }
        background: MorphButtonBackground {
            scheme: root.scheme
            fill: root.triggerEmphasis === 1 ? root.scheme.accent
                : root.triggerEmphasis === 2 ? root.scheme.error
                : root.triggerEmphasis === 3 ? "transparent" : root.scheme.surface
            filled: root.triggerEmphasis === 1 || root.triggerEmphasis === 2
            ghost: root.triggerEmphasis === 3
            hovered: trigger.hovered
            pressed: trigger.down
            focused: trigger.visualFocus
        }
        Accessible.ignored: true
        // A tooltip of its own: the attached ToolTip's content item reads markup, and the label is a document's text.
        ToolTip {
            id: triggerTip
            text: root.triggerLabel
            visible: root.triggerLabel !== "" && trigger.text === "" && trigger.hovered
            contentItem: Text {
                text: triggerTip.text
                textFormat: Text.PlainText
                font: triggerTip.font
                wrapMode: Text.Wrap
                color: triggerTip.palette.toolTipText
            }
        }
        onClicked: root.openFromTrigger()
        Keys.onReturnPressed: root.openFromTrigger()
        Keys.onEnterPressed: root.openFromTrigger()
    }

    RowLayout {
        id: bar
        anchors.fill: parent
        spacing: 0
        visible: !root.buttonStyle

        Repeater {
            model: root.entries

            ToolButton {
                font: Theme.bodyFont
                id: topEntry
                required property int index
                required property var modelData
                visible: !modelData.separator
                text: root.entryText(modelData)
                contentItem: MorphPlainContent {
                    control: topEntry
                    color: topEntry.palette.windowText
                }
                icon.source: Theme.iconSource(modelData.icon ? modelData.icon : "")
                icon.color: "transparent"
                enabled: modelData.enabled
                onClicked: {
                    root.openFrom = topEntry
                    if (modelData.items && modelData.items.length > 0)
                        root.open([index])
                    else
                        root.choose([index])
                }
            }
        }
    }

    Popup {
        id: popup
        y: root.buttonStyle ? trigger.height : bar.height
        padding: 2
        onAboutToShow: root.placePopup()
        onWidthChanged: {
            if (visible)
                root.placePopup()
        }

        MorphPopupPlacement {
            id: placement
        }

        background: MorphPopupBackground {
        }

        contentItem: ColumnLayout {
            spacing: 0

            Repeater {
                model: root.openEntries

                ColumnLayout {
                    id: entry
                    required property int index
                    required property var modelData
                    spacing: 0
                    Layout.fillWidth: true

                    MenuSeparator {
                        visible: entry.modelData.separator === true
                        Layout.fillWidth: true
                    }
                    ItemDelegate {
                        id: entryButton
                        font: Theme.bodyFont
                        visible: entry.modelData.separator !== true
                        Layout.fillWidth: true
                        text: root.entryText(entry.modelData)
                              + (entry.modelData.keys ? "    " + entry.modelData.keys : "")
                        icon.source: Theme.iconSource(entry.modelData.icon ? entry.modelData.icon : "")
                        icon.color: "transparent"
                        contentItem: MorphPlainContent {
                            control: entryButton
                            color: entryButton.palette.text
                            alignment: Qt.AlignLeft
                        }
                        background: Rectangle {
                            radius: Theme.radiusControl
                            color: entryButton.down || entryButton.hovered || entryButton.visualFocus
                                   ? Theme.cardScheme.hover : "transparent"
                        }
                        enabled: entry.modelData.enabled
                        Accessible.role: Accessible.MenuItem
                        Accessible.description: entry.modelData.description ? entry.modelData.description : ""
                        onClicked: {
                            const path = root.openPath.concat([entry.index])
                            if (entry.modelData.items && entry.modelData.items.length > 0)
                                root.openPath = path
                            else
                                root.choose(path)
                        }
                    }
                    Label {
                        textFormat: Text.PlainText
                        visible: entry.modelData.separator !== true && !!entry.modelData.description
                        text: entry.modelData.description ? entry.modelData.description : ""
                        font: Theme.bodyFont
                        color: root.scheme.muted
                        leftPadding: Theme.spaceOf(3)
                        bottomPadding: Theme.spaceOf(1)
                        Layout.fillWidth: true
                        Accessible.ignored: true
                    }
                }
            }
        }
    }
}
