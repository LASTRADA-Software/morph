// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    spacing: 16

    // ── Add payee ─────────────────────────────────────────────────────────
    Panel {
        Layout.fillWidth: true
        implicitHeight: 72
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            spacing: 10
            Field { id: name; placeholderText: "Payee name"; Layout.fillWidth: true }
            Field { id: iban; placeholderText: "IBAN"; Layout.fillWidth: true }
            Field { id: bank; placeholderText: "Bank (optional)"; Layout.fillWidth: true }
            AppButton {
                text: "Add payee"
                variant: "primary"
                onClicked: { payees.addPayee(name.text, iban.text, bank.text); name.text = ""; iban.text = ""; bank.text = "" }
            }
        }
    }

    // ── Pay bill ──────────────────────────────────────────────────────────
    Panel {
        Layout.fillWidth: true
        implicitHeight: 72
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            spacing: 10
            Text { text: "Pay bill"; font.pixelSize: 16; font.weight: Font.DemiBold; color: theme.ink }
            Item { Layout.fillWidth: true }
            Picker { id: payAccount; model: payees.accounts; textRole: "label"; valueRole: "id"; implicitWidth: 170 }
            Picker { id: payPayee; model: payees.payees; textRole: "name"; valueRole: "id"; implicitWidth: 170 }
            Field { id: payAmount; placeholderText: "Amount"; implicitWidth: 140 }
            AppButton {
                text: "Pay"
                variant: "primary"
                onClicked: { payees.payBill(payAccount.currentValue, payPayee.currentValue, payAmount.text); payAmount.text = "" }
            }
        }
    }

    // ── Payee list ────────────────────────────────────────────────────────
    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        ColumnLayout {
            width: parent.width
            spacing: 12
            Repeater {
                model: payees.payees
                delegate: Panel {
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 74
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        ColumnLayout {
                            spacing: 4
                            Text { text: modelData.name; font.pixelSize: 16; font.weight: Font.DemiBold; color: theme.ink }
                            Text { text: modelData.iban; color: theme.inkSoft; font.pixelSize: 12 }
                        }
                        Item { Layout.fillWidth: true }
                        AppButton { text: "Remove"; variant: "danger"; onClicked: payees.removePayee(modelData.id) }
                    }
                }
            }
        }
    }
}
