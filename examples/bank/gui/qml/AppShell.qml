// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

RowLayout {
    id: shell
    objectName: "appShell"
    spacing: 0
    property int current: 0
    readonly property var titles: ["Accounts", "Move Money", "Cards", "Payees & Bills", "Loans"]
    readonly property var controllers: [accounts, txns, cards, payees, loans]

    function refreshCurrent() { shell.controllers[shell.current].refresh() }
    onCurrentChanged: refreshCurrent()
    Component.onCompleted: refreshCurrent()

    // ── Sidebar ───────────────────────────────────────────────────────────
    Rectangle {
        Layout.preferredWidth: 232
        Layout.fillHeight: true
        color: theme.sidebar

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Text {
                text: "Morph Bank"
                color: "#FAF9F5"
                font.pixelSize: 19
                font.weight: Font.Bold
                Layout.leftMargin: 20
                Layout.topMargin: 22
            }
            Text {
                text: "personal banking"
                color: "#8C887F"
                font.pixelSize: 12
                Layout.leftMargin: 20
                Layout.bottomMargin: 14
            }

            Repeater {
                model: shell.titles
                delegate: Rectangle {
                    required property int index
                    required property string modelData
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    Layout.topMargin: 2
                    implicitHeight: 42
                    radius: 9
                    color: shell.current === index ? theme.accent
                         : navMouse.containsMouse ? theme.sidebarHover : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        text: modelData
                        font.pixelSize: 14
                        font.weight: shell.current === index ? Font.DemiBold : Font.Medium
                        color: shell.current === index ? "#FFFFFF" : theme.sidebarText
                    }
                    MouseArea {
                        id: navMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: shell.current = index
                    }
                }
            }

            Item { Layout.fillHeight: true }

            Text {
                text: app.displayName
                color: "#FAF9F5"
                font.weight: Font.DemiBold
                Layout.leftMargin: 20
            }
            Text {
                text: "@" + app.principal
                color: "#8C887F"
                font.pixelSize: 12
                Layout.leftMargin: 20
                Layout.bottomMargin: 8
            }
            AppButton {
                text: "Log out"
                variant: "ghost"
                Layout.leftMargin: 8
                Layout.bottomMargin: 14
                onClicked: app.logout()
            }
        }
    }

    // ── Content ───────────────────────────────────────────────────────────
    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.margins: 30
        spacing: 18

        Text {
            text: shell.titles[shell.current]
            font.pixelSize: 26
            font.weight: Font.Bold
            color: theme.ink
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: shell.current
            AccountsPage {}
            MoveMoneyPage {}
            CardsPage {}
            PayeesPage {}
            LoansPage {}
        }
    }
}
