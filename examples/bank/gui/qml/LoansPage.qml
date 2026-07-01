// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    spacing: 16

    // ── Apply ─────────────────────────────────────────────────────────────
    Panel {
        Layout.fillWidth: true
        implicitHeight: 72
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            spacing: 10
            Text { text: "Apply for a loan"; font.pixelSize: 16; font.weight: Font.DemiBold; color: theme.ink }
            Item { Layout.fillWidth: true }
            Picker { id: account; model: loans.accounts; textRole: "label"; valueRole: "id"; implicitWidth: 150 }
            Field { id: principal; placeholderText: "Principal"; implicitWidth: 130 }
            Field { id: rate; placeholderText: "Rate (bps)"; implicitWidth: 110 }
            Field { id: term; placeholderText: "Months"; implicitWidth: 90 }
            AppButton {
                text: "Apply"
                variant: "primary"
                onClicked: {
                    // Pass -1 for a blank rate so the controller can tell "left blank"
                    // apart from an intentional 0% loan (which the model allows).
                    var rateBps = rate.text.length > 0 ? parseInt(rate.text) : -1;
                    loans.apply(account.currentValue, principal.text,
                                isNaN(rateBps) ? -1 : rateBps, parseInt(term.text || "0"));
                    principal.text = ""; rate.text = ""; term.text = "";
                }
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        spacing: 16

        // ── Loan list ──────────────────────────────────────────────────────
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 200
            clip: true
            ColumnLayout {
                width: parent.width
                spacing: 12
                Repeater {
                    model: loans.loans
                    delegate: Panel {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 78
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 20
                            anchors.rightMargin: 20
                            spacing: 10
                            ColumnLayout {
                                spacing: 4
                                Text { text: modelData.title; font.pixelSize: 16; font.weight: Font.DemiBold; color: theme.ink }
                                Text { text: modelData.detail; color: theme.inkSoft; font.pixelSize: 12 }
                            }
                            Item { Layout.fillWidth: true }
                            Pill { text: modelData.statusText; kind: modelData.statusKind }
                            AppButton { text: "Schedule"; onClicked: loans.showSchedule(modelData.id) }
                            AppButton {
                                visible: modelData.active
                                text: "Repay"
                                variant: "primary"
                                onClicked: loans.repay(modelData.id, modelData.accountId, (modelData.outstanding / 100).toString())
                            }
                        }
                    }
                }
            }
        }

        // ── Amortization schedule ────────────────────────────────────────────
        Panel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 10
                Text { text: "Amortization schedule"; font.pixelSize: 16; font.weight: Font.DemiBold; color: theme.ink }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "#"; color: theme.inkSoft; font.pixelSize: 11; font.weight: Font.DemiBold; Layout.preferredWidth: 40 }
                    Text { text: "PRINCIPAL"; color: theme.inkSoft; font.pixelSize: 11; font.weight: Font.DemiBold; Layout.fillWidth: true }
                    Text { text: "INTEREST"; color: theme.inkSoft; font.pixelSize: 11; font.weight: Font.DemiBold; Layout.fillWidth: true }
                    Text { text: "REMAINING"; color: theme.inkSoft; font.pixelSize: 11; font.weight: Font.DemiBold; Layout.fillWidth: true }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: theme.border }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: loans.schedule
                    delegate: RowLayout {
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0
                        height: 34
                        Text { text: modelData.month; color: theme.ink; Layout.preferredWidth: 40 }
                        Text { text: modelData.principalText; color: theme.ink; Layout.fillWidth: true }
                        Text { text: modelData.interestText; color: theme.ink; Layout.fillWidth: true }
                        Text { text: modelData.remainingText; color: theme.ink; Layout.fillWidth: true }
                    }
                }
            }
        }
    }
}
