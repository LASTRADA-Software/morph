// SPDX-License-Identifier: Apache-2.0
//
// The categorisation-rules screen. A MembersView-style CRUD surface: create a
// rule, edit it, and see the version an edit bumped.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: view
    property var bridge: null
    spacing: 8

    Label { text: qsTr("Categorisation rules"); font.bold: true }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        // Stated in the UI because it is a real semantic that surprises
        // people: editing a rule does not reclassify what it already
        // classified. The version is how a reader tells the two apart.
        text: qsTr("Editing a rule bumps its version. Transactions already categorised keep the version "
                 + "that categorised them — an edit never rewrites history.")
    }

    RowLayout {
        Layout.fillWidth: true
        TextField { id: matchText; placeholderText: qsTr("Description contains…"); Layout.fillWidth: true }
        TextField { id: categoryId; placeholderText: qsTr("Set category id"); Layout.fillWidth: true }
        Button {
            text: qsTr("Create rule")
            enabled: view.bridge !== null && matchText.text.length > 0
            onClicked: view.bridge.createRule(matchText.text, categoryId.text)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        TextField { id: editRuleId; placeholderText: qsTr("Rule id"); Layout.fillWidth: true }
        TextField { id: editMatch; placeholderText: qsTr("New match text"); Layout.fillWidth: true }
        TextField { id: editCategory; placeholderText: qsTr("New category id"); Layout.fillWidth: true }
        Button {
            text: qsTr("Update rule")
            enabled: view.bridge !== null && editRuleId.text.length > 0
            onClicked: view.bridge.updateRule(editRuleId.text, editMatch.text, editCategory.text)
        }
    }

    GroupBox {
        title: qsTr("Last rule")
        Layout.fillWidth: true
        GridLayout {
            columns: 2
            Label { text: qsTr("id") }
            Label { text: view.bridge && view.bridge.lastRule ? (view.bridge.lastRule.id || "-") : "-" }
            Label { text: qsTr("match") }
            Label { text: view.bridge && view.bridge.lastRule ? (view.bridge.lastRule.matchText || "-") : "-" }
            Label { text: qsTr("version") }
            Label {
                text: view.bridge && view.bridge.lastRule && view.bridge.lastRule.version !== undefined
                      ? view.bridge.lastRule.version : "-"
                font.bold: true
            }
        }
    }

    Item { Layout.fillHeight: true }

    Label {
        text: view.bridge ? view.bridge.lastError : ""
        visible: text.length > 0
        color: "crimson"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
    }
}
