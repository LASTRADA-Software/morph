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

    /// Formats an exact triple from the report map under `prefix`.
    function formatPart(report, prefix) {
        if (!report || report[prefix + "Denominator"] === undefined) {
            return "-";
        }
        const denominator = report[prefix + "Denominator"] === 0 ? 1 : report[prefix + "Denominator"];
        const places = report[prefix + "DecimalPlaces"];
        return ((report[prefix + "Numerator"] / denominator) / Math.pow(10, places)).toFixed(places);
    }

    Label { text: qsTr("Budgets"); font.bold: true }

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
        TextField { id: budgetName; placeholderText: qsTr("Budget name"); Layout.fillWidth: true }
        TextField { id: budgetCategoryId; placeholderText: qsTr("Category id"); Layout.fillWidth: true }
        Button {
            text: qsTr("Create budget")
            enabled: view.bridge !== null && budgetName.text.length > 0
            onClicked: view.bridge.createBudget(budgetName.text, budgetCategoryId.text)
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
