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

    Label {
        text: qsTr("Accounts")
        font.bold: true
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

    Label {
        text: view.bridge ? view.bridge.lastError : ""
        visible: text.length > 0
        color: "crimson"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
    }
}
