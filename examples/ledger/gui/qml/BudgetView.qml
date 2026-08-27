// SPDX-License-Identifier: Apache-2.0
//
// The budgets screen: create a category, budget over it, set a month's limit,
// and read limit-versus-spent.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: view
    property var bridge: null
    spacing: 8

    /// Reads the text the bridge pre-rendered for `prefix`, or "-" before a
    /// report has arrived.
    ///
    /// The rendering itself is `ledger::formatMoney`, in the bridge: QML has
    /// only IEEE doubles, so dividing the exact
    /// numerator/denominator/decimalPlaces here would undo `Rational`'s
    /// exactness in the last three lines of the path (design spec §7's
    /// no-float rule). The exact triple is still published alongside.
    function formatPart(report, prefix) {
        if (!report || report[prefix + "Text"] === undefined) {
            return "-";
        }
        return report[prefix + "Text"];
    }

    RowLayout {
        Label { text: qsTr("Budgets"); font.bold: true }
        BusyIndicator {
            running: view.bridge ? view.bridge.busy : false
            visible: running
            implicitWidth: 20
            implicitHeight: 20
        }
    }

    // A single status line for the three create/set gestures below, told
    // apart by which one last fired. Mirrors ReportView's "answer arrives
    // asynchronously" framing: each of these calls returns nothing to bind,
    // only a signal, so without this line succeeding produced no visible
    // effect distinguishable from doing nothing.
    Label {
        id: statusLabel
        Layout.fillWidth: true
        opacity: 0.7
        text: ""
    }
    Connections {
        target: view.bridge
        function onCategoryCreated() {
            statusLabel.text = qsTr("Category created (id %1)").arg(view.bridge.lastCategoryId());
        }
        function onBudgetCreated() {
            statusLabel.text = qsTr("Budget created (id %1)").arg(view.bridge.lastBudgetId());
        }
        function onLimitSet() {
            statusLabel.text = qsTr("Limit set");
        }
    }

    RowLayout {
        Layout.fillWidth: true
        TextField { id: categoryName; placeholderText: qsTr("Category name"); Layout.fillWidth: true }
        Button {
            text: qsTr("Create category")
            enabled: view.bridge !== null && categoryName.text.length > 0
            onClicked: { view.bridge.createCategory(categoryName.text); categoryName.text = ""; }
        }
        Label {
            text: view.bridge && view.bridge.lastCategoryId() ? qsTr("id %1").arg(view.bridge.lastCategoryId()) : ""
            opacity: 0.7
        }
    }

    RowLayout {
        Layout.fillWidth: true
        TextField { id: linkAccountId; placeholderText: qsTr("Account id"); Layout.fillWidth: true }
        TextField { id: linkCategoryId; placeholderText: qsTr("Category id"); Layout.fillWidth: true }
        Button {
            text: qsTr("Link account to category")
            enabled: view.bridge !== null && linkAccountId.text.length > 0 && linkCategoryId.text.length > 0
            onClicked: view.bridge.linkAccount(linkAccountId.text, linkCategoryId.text)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        TextField { id: budgetName; placeholderText: qsTr("Budget name"); Layout.fillWidth: true }
        TextField { id: budgetCategoryId; placeholderText: qsTr("Category id"); Layout.fillWidth: true }
        Button {
            text: qsTr("Create budget")
            enabled: view.bridge !== null && budgetName.text.length > 0
            onClicked: view.bridge.createBudget(budgetName.text, budgetCategoryId.text)
        }
        Label {
            text: view.bridge && view.bridge.lastBudgetId() ? qsTr("id %1").arg(view.bridge.lastBudgetId()) : ""
            opacity: 0.7
        }
    }

    RowLayout {
        Layout.fillWidth: true
        TextField { id: limitBudgetId; placeholderText: qsTr("Budget id"); Layout.fillWidth: true }
        TextField { id: limitMonth; placeholderText: qsTr("Month (YYYY-MM)"); Layout.fillWidth: true }
        TextField {
            id: limitMinor
            placeholderText: qsTr("Limit (cents)")
            validator: IntValidator { bottom: 0 }
            Layout.fillWidth: true
        }
        Button {
            text: qsTr("Set limit")
            enabled: view.bridge !== null && limitMinor.acceptableInput
            onClicked: view.bridge.setBudgetLimit(limitBudgetId.text, limitMonth.text,
                                                  parseInt(limitMinor.text, 10), "USD")
        }
        Button {
            text: qsTr("Report")
            enabled: view.bridge !== null
            onClicked: view.bridge.getBudgetReport(limitBudgetId.text, limitMonth.text)
        }
    }

    GridLayout {
        columns: 2
        Layout.fillWidth: true
        Label { text: qsTr("Limit") }
        Label { text: view.formatPart(view.bridge ? view.bridge.report : null, "limit"); font.family: "monospace" }
        Label { text: qsTr("Spent") }
        Label { text: view.formatPart(view.bridge ? view.bridge.report : null, "spent"); font.family: "monospace" }
        Label { text: qsTr("Currency") }
        Label { text: view.bridge && view.bridge.report ? (view.bridge.report.currency || "-") : "-" }
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
