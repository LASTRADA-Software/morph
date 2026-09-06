// SPDX-License-Identifier: Apache-2.0
//
// The accounts screen: every account with its exact balance, plus the two
// gestures that change them.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: view
    property var bridge: null
    spacing: 8

    // Balances are bound as `balanceText`, which the bridge pre-renders with
    // `ledger::formatMoney` (design spec §7's no-float rule). This file does
    // no arithmetic on money: QML has only IEEE doubles, so dividing the
    // exact numerator/denominator/decimalPlaces here would undo `Rational`'s
    // exactness in the last three lines of the path and drift past 2^53. The
    // exact triple is still published for a view that needs the parts.

    RowLayout {
        Label {
            text: qsTr("Accounts")
            font.bold: true
        }
        BusyIndicator {
            running: view.bridge ? view.bridge.busy : false
            visible: running
            implicitWidth: 20
            implicitHeight: 20
        }
        Item { Layout.fillWidth: true }
        Button {
            text: qsTr("Refresh")
            enabled: view.bridge !== null
            onClicked: view.bridge.refresh()
        }
    }

    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: view.bridge ? view.bridge.accounts : []
        delegate: RowLayout {
            width: ListView.view ? ListView.view.width : 0
            Label { text: modelData.name; Layout.fillWidth: true }
            Label { text: modelData.kind; opacity: 0.7 }
            Label { text: modelData.currency; opacity: 0.7 }
            Label { text: modelData.balanceText; font.family: "monospace" }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        TextField {
            id: accountName
            placeholderText: qsTr("New account name")
            Layout.fillWidth: true
        }
        ComboBox {
            id: accountKind
            model: ["asset", "expense", "revenue", "liability"]
        }
        Button {
            text: qsTr("Open account")
            enabled: view.bridge !== null && accountName.text.length > 0
            onClicked: {
                view.bridge.openAccount(accountName.text, accountKind.currentText, "USD");
                accountName.text = "";
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        TextField { id: fromId; placeholderText: qsTr("From account id"); Layout.fillWidth: true }
        TextField { id: toId; placeholderText: qsTr("To account id"); Layout.fillWidth: true }
        TextField {
            id: amountMinor
            placeholderText: qsTr("Amount (cents)")
            // Integer-only: minor units cross the boundary as a whole number,
            // so no decimal string is ever parsed into a float here.
            validator: IntValidator { bottom: 1 }
            Layout.fillWidth: true
        }
        TextField { id: description; placeholderText: qsTr("Description"); Layout.fillWidth: true }
        Button {
            text: qsTr("Store")
            enabled: view.bridge !== null && amountMinor.acceptableInput
            onClicked: view.bridge.storeTransaction(fromId.text, toId.text,
                                                    parseInt(amountMinor.text, 10), description.text)
        }
    }

    // ── Entries, and the Undo control they feed ──────────────────────────
    // The journal id is shown rather than typed. Until morph#428 this was a
    // bare "Journal id to undo" TextField, and no screen in this rung -- and
    // no reply on the wire -- ever displayed a journal id, so the only way to
    // fill it in was to guess. `listTransactions` is where the ids come from
    // now; `undoTransaction` is handed one of them straight back.
    RowLayout {
        Layout.fillWidth: true
        Label { text: qsTr("Entries") ; font.bold: true }
        TextField {
            id: entryMonth
            placeholderText: qsTr("Month (YYYY-MM)")
            // The listing is month-bounded on the wire, so a malformed month
            // is a refusal from the model rather than an unbounded read. The
            // mask keeps the ordinary case from having to see that refusal.
            inputMask: "9999-99"
            Layout.fillWidth: true
        }
        Button {
            text: qsTr("List")
            enabled: view.bridge !== null && entryMonth.text.length === 7
            onClicked: view.bridge.listTransactions(entryMonth.text)
        }
    }

    ListView {
        Layout.fillWidth: true
        Layout.preferredHeight: 120
        clip: true
        model: view.bridge ? view.bridge.entries : []
        delegate: RowLayout {
            width: ListView.view ? ListView.view.width : 0
            Label { text: modelData.id; font.family: "monospace"; opacity: 0.7 }
            Label { text: modelData.description; Layout.fillWidth: true }
            Label { text: modelData.dateText; opacity: 0.7 }
            Button {
                text: qsTr("Undo")
                enabled: view.bridge !== null
                // The id comes from the row the user is looking at. Nothing
                // here parses a number a person typed.
                onClicked: view.bridge.undoTransaction(modelData.id)
            }
        }
    }

    Label {
        text: view.bridge ? view.bridge.lastError : ""
        visible: text.length > 0
        color: "crimson"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
    }
}
