// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    spacing: 16

    // ── Move money ────────────────────────────────────────────────────────
    Panel {
        Layout.fillWidth: true
        implicitHeight: moveCol.implicitHeight + 36
        ColumnLayout {
            id: moveCol
            anchors.fill: parent
            anchors.margins: 18
            spacing: 12

            Text { text: "Move money"; font.pixelSize: 16; font.weight: Font.DemiBold; color: theme.ink }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Text { text: "Account"; color: theme.inkSoft }
                Picker {
                    id: account
                    Layout.fillWidth: true
                    model: txns.accounts
                    textRole: "label"
                    valueRole: "id"
                    onActivated: txns.selectAccount(currentValue)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Field { id: amount; placeholderText: "Amount"; Layout.fillWidth: true }
                AppButton { text: "Deposit"; variant: "primary"; onClicked: { txns.deposit(amount.text); amount.text = "" } }
                AppButton { text: "Withdraw"; onClicked: { txns.withdraw(amount.text); amount.text = "" } }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                AppButton { text: "Transfer to"; variant: "primary"; onClicked: { txns.transfer(target.currentValue, transferAmount.text); transferAmount.text = "" } }
                Picker { id: target; Layout.fillWidth: true; model: txns.accounts; textRole: "label"; valueRole: "id" }
                Field { id: transferAmount; placeholderText: "Amount"; Layout.fillWidth: true }
            }
        }
    }

    // ── Recent activity ───────────────────────────────────────────────────
    Panel {
        Layout.fillWidth: true
        Layout.fillHeight: true
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 10
            Text { text: "Recent activity"; font.pixelSize: 16; font.weight: Font.DemiBold; color: theme.ink }

            // Header row
            RowLayout {
                Layout.fillWidth: true
                Text { text: "TYPE"; color: theme.inkSoft; font.pixelSize: 11; font.weight: Font.DemiBold; Layout.fillWidth: true }
                Text { text: "AMOUNT"; color: theme.inkSoft; font.pixelSize: 11; font.weight: Font.DemiBold; Layout.preferredWidth: 160 }
                Text { text: "BALANCE"; color: theme.inkSoft; font.pixelSize: 11; font.weight: Font.DemiBold; Layout.preferredWidth: 160 }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: theme.border }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: txns.history
                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view ? ListView.view.width : 0
                    height: 40
                    color: "transparent"
                    RowLayout {
                        anchors.fill: parent
                        Text { text: modelData.kind; color: theme.ink; Layout.fillWidth: true }
                        Text {
                            text: modelData.amountText
                            color: modelData.isCredit ? theme.good : theme.bad
                            font.weight: Font.Medium
                            Layout.preferredWidth: 160
                        }
                        Text { text: modelData.balanceText; color: theme.ink; Layout.preferredWidth: 160 }
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#F0EEE7" }
                }
            }
        }
    }
}
