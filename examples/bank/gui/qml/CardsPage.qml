// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    spacing: 16

    // ── Issue card ────────────────────────────────────────────────────────
    Panel {
        Layout.fillWidth: true
        implicitHeight: 72
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            spacing: 10
            Text { text: "Issue card"; font.pixelSize: 16; font.weight: Font.DemiBold; color: theme.ink }
            Item { Layout.fillWidth: true }
            Picker { id: account; model: cards.accounts; textRole: "label"; valueRole: "id"; implicitWidth: 160 }
            Picker { id: kind; model: ["Debit", "Credit"]; implicitWidth: 130 }
            Field { id: limit; placeholderText: "Daily limit (opt.)"; implicitWidth: 160 }
            AppButton {
                text: "Issue"
                variant: "primary"
                onClicked: { cards.issue(account.currentValue, kind.currentIndex, limit.text); limit.text = "" }
            }
        }
    }

    // ── Card list ─────────────────────────────────────────────────────────
    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        ColumnLayout {
            width: parent.width
            spacing: 12
            Repeater {
                model: cards.cards
                delegate: Panel {
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 78
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        spacing: 12
                        ColumnLayout {
                            spacing: 4
                            Text { text: modelData.title; font.pixelSize: 16; font.weight: Font.DemiBold; color: theme.ink }
                            Text { text: modelData.limitText; color: theme.inkSoft; font.pixelSize: 12 }
                        }
                        Item { Layout.fillWidth: true }
                        Pill { text: modelData.statusText; kind: modelData.statusKind }
                        AppButton {
                            visible: !modelData.cancelled
                            text: modelData.active ? "Freeze" : "Unfreeze"
                            onClicked: modelData.active ? cards.freeze(modelData.id) : cards.unfreeze(modelData.id)
                        }
                        AppButton {
                            visible: !modelData.cancelled
                            text: "Cancel"
                            variant: "danger"
                            onClicked: cards.cancel(modelData.id)
                        }
                    }
                }
            }
        }
    }
}
