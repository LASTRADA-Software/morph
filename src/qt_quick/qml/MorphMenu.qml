// SPDX-License-Identifier: Apache-2.0
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A menu bar: one button per top-level entry. An entry with a submenu opens it in a popup, one level at a time; an
// entry without one is chosen, and the menu reports its path, its index at each level from the top.
MorphWidget {
    id: root

    // The entries: [{label, icon, keys, checked (null, true or false), enabled, items: [...]}].
    property var entries: []
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

    // Activates the entry at `path` as a click on it would, opening each submenu on the way. A path that names no
    // entry, an entry with a submenu, or an entry that is disabled or inside a disabled one does nothing.
    function choose(path: var) {
        let level = root.entries
        for (let depth = 0; depth < path.length; ++depth) {
            const index = path[depth]
            if (!level || index < 0 || index >= level.length || !level[index].enabled)
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

    function open(path: var) {
        root.openPath = path
        if (!popup.visible)
            popup.open()
    }

    function entryText(entry: var): string {
        const mark = entry.checked === true ? "✓ " : entry.checked === false ? "  " : ""
        const more = entry.items && entry.items.length > 0 ? " ›" : ""
        return mark + (entry.icon ? entry.icon + " " : "") + entry.label + more
    }

    focusTarget: bar
    implicitWidth: bar.implicitWidth
    implicitHeight: bar.implicitHeight

    RowLayout {
        id: bar
        anchors.fill: parent
        spacing: 0

        Repeater {
            model: root.entries

            ToolButton {
                id: top
                required property int index
                required property var modelData
                text: root.entryText(modelData)
                enabled: modelData.enabled
                onClicked: {
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
        y: bar.height
        padding: 2

        contentItem: ColumnLayout {
            spacing: 0

            Repeater {
                model: root.openEntries

                ItemDelegate {
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    text: root.entryText(modelData) + (modelData.keys ? "    " + modelData.keys : "")
                    enabled: modelData.enabled
                    onClicked: {
                        const path = root.openPath.concat([index])
                        if (modelData.items && modelData.items.length > 0)
                            root.openPath = path
                        else
                            root.choose(path)
                    }
                }
            }
        }
    }
}
