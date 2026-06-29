// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    spacing: 16

    // ── Summary stat ────────────────────────────────────────────────────────
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 96
        radius: theme.radius
        color: theme.sidebar
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 22
            spacing: 2
            Text {
                text: accounts.totalBalance
                color: "#FFFFFF"
                font.pixelSize: 28
                font.weight: Font.Bold
            }
            Text {
                text: accounts.openCount + " open account(s)"
                color: "#A7A39A"
                font.pixelSize: 13
            }
        }
    }

    // ── Open account form ─────────────────────────────────────────────────
    Panel {
        Layout.fillWidth: true
        implicitHeight: 72
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            spacing: 10
            Text { text: "New account"; font.pixelSize: 16; font.weight: Font.DemiBold; color: theme.ink }
            Item { Layout.fillWidth: true }
            Picker { id: kind; model: ["Checking", "Savings", "Credit"]; implicitWidth: 150 }
            Picker { id: currency; model: ["USD", "EUR", "GBP", "CHF", "JPY"]; implicitWidth: 110 }
            Field { id: overdraft; placeholderText: "Overdraft (opt.)"; implicitWidth: 150 }
            AppButton {
                text: "Open account"
                variant: "primary"
                onClicked: {
                    accounts.openAccount(kind.currentIndex, currency.currentIndex, overdraft.text);
                    overdraft.text = "";
                }
            }
        }
    }

    // ── Account cards ─────────────────────────────────────────────────────
    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        Flow {
            width: parent.width
            spacing: 16
            Repeater {
                model: accounts.accounts
                delegate: Panel {
                    required property var modelData
                    width: 280
                    height: 150
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 20
                        spacing: 6
                        RowLayout {
                            Layout.fillWidth: true
                            Text { text: modelData.kind; font.pixelSize: 17; font.weight: Font.DemiBold; color: theme.ink }
                            Item { Layout.fillWidth: true }
                            Pill { text: modelData.statusText; kind: modelData.statusKind }
                        }
                        Text { text: modelData.number; color: theme.inkSoft }
                        Item { Layout.fillHeight: true }
                        Text { text: modelData.balanceText; font.pixelSize: 24; font.weight: Font.Bold; color: theme.ink }
                        Text {
                            text: modelData.overdraftText
                            color: theme.inkSoft
                            font.pixelSize: 12
                            visible: modelData.hasOverdraft
                        }
                    }
                }
            }
        }
    }
}
