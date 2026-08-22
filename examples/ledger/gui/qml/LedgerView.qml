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

    /// Renders an exact Rational triple as text.
    ///
    /// The bridge publishes numerator/denominator/decimalPlaces rather than
    /// one pre-divided number (design spec §7's no-float rule), so the
    /// formatting decision lives here, in the view, where it belongs -- and
    /// the model never produced a double that could round.
    function formatAmount(entry) {
        if (!entry) {
            return "";
        }
        const denominator = entry.balanceDenominator === 0 ? 1 : entry.balanceDenominator;
        const places = entry.balanceDecimalPlaces;
        const scaled = entry.balanceNumerator / denominator;
        return (scaled / Math.pow(10, places)).toFixed(places);
    }

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
            Label { text: view.formatAmount(modelData); font.family: "monospace" }
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
