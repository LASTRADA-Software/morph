// SPDX-License-Identifier: Apache-2.0
//
// Read-only display of one fetched paste. Every value shown is server-computed
// and arrives already rendered as text from gui/main.cpp's PasteBridge — this
// file formats nothing and decides nothing (examples/IMPLEMENTATION.md rule 2's
// "pure glue" allowance for read-only displays; there is no hand-rolled input
// widget here, only a Delete button that relays an id).
//
// Zero styling effort by rule: default Qt Quick controls, default fonts, no
// theming.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: pane

    /// The property bag PasteBridge emits with `loaded`, or null when nothing
    /// is open yet.
    property var paste: null

    /// Emitted when the user asks for the currently displayed paste to go.
    signal deleteRequested(string pasteId)

    property var facts: pane.paste ? [
        { key: "syntax", value: pane.paste.syntax },
        { key: "visibility", value: pane.paste.visibility },
        { key: "editability", value: pane.paste.editability },
        { key: "created", value: pane.paste.createdAt },
        { key: "expires", value: pane.paste.expiresAt === "" ? "never" : pane.paste.expiresAt },
        { key: "reads", value: pane.paste.readCount },
        { key: "burn after", value: pane.paste.burnAfterReads === "N/A" ? "no limit" : pane.paste.burnAfterReads }
    ] : []

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        Label {
            Layout.fillWidth: true
            font.bold: true
            elide: Text.ElideRight
            text: pane.paste ? pane.paste.id : "no paste open — pick one from the list"
        }

        // One "key: value" line per fact rather than a two-column grid: a
        // Repeater contributes one item per model entry, so a grid would need
        // either two Repeaters (which can desynchronise) or a per-row wrapper —
        // neither of which buys anything at this rung's styling budget.
        Repeater {
            model: pane.facts

            delegate: Label {
                required property var modelData
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: modelData.key + ": " + modelData.value
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                readOnly: true
                wrapMode: TextArea.Wrap
                text: pane.paste ? pane.paste.content : ""
            }
        }

        Button {
            text: "Delete this paste"
            enabled: pane.paste !== null
            onClicked: pane.deleteRequested(pane.paste.id)
        }
    }
}
