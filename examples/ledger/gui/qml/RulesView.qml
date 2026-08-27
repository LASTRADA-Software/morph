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

    RowLayout {
        Label { text: qsTr("Categorisation rules"); font.bold: true }
        BusyIndicator {
            running: view.bridge ? view.bridge.busy : false
            visible: running
            implicitWidth: 20
            implicitHeight: 20
        }
    }

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

    // `ruleCreated`/`ruleUpdated` carry no payload of their own -- the row
    // they describe is `lastRule`, already bound below -- so this status
    // line exists only to tell the two outcomes apart at the moment they
    // happen, which the "Last rule" box alone cannot do (it looks the same
    // either way once both have landed).
    Label {
        id: ruleStatusLabel
        Layout.fillWidth: true
        opacity: 0.7
        text: ""
    }
    Connections {
        target: view.bridge
        function onRuleCreated() { ruleStatusLabel.text = qsTr("Rule created"); }
        function onRuleUpdated() { ruleStatusLabel.text = qsTr("Rule updated"); }
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
