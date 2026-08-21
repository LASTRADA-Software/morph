// SPDX-License-Identifier: Apache-2.0
//
// ledger's desktop shell: a TabBar over the rung's four screens -- the
// ledger's accounts, its budgets, its categorisation rules, and the
// submit->poll monthly statement.
//
// Every bridge property defaults to `null` and every view guards on it. That
// is what lets this same file load under a bare QQmlApplicationEngine with
// nothing wired (tests/test_ledger_qml_smoke.cpp), which is the whole reason
// main.cpp passes the bridges as *initial properties* rather than context
// properties -- the same convention kanban's own Main.qml documents.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 900
    height: 640
    visible: true
    title: qsTr("morph ledger")

    property var ledgerBridge: null
    property var budgetBridge: null
    property var ruleBridge: null
    property var reportBridge: null

    /// The ledger every screen works against. Typed as a string because that
    /// is what the bridges' own Q_INVOKABLEs take -- ids cross this boundary
    /// as plain-number text, never as JS numbers, so a 64-bit id cannot lose
    /// precision on the way through.
    property string ledgerId: "1"

    Component.onCompleted: openLedgerEverywhere()

    function openLedgerEverywhere() {
        if (ledgerBridge) {
            ledgerBridge.openLedger(ledgerId);
        }
        if (budgetBridge) {
            budgetBridge.openLedger(ledgerId);
        }
        if (ruleBridge) {
            ruleBridge.openLedger(ledgerId);
        }
        if (reportBridge) {
            reportBridge.openLedger(ledgerId);
        }
    }

    header: TabBar {
        id: tabs
        TabButton { text: qsTr("Accounts") }
        TabButton { text: qsTr("Budgets") }
        TabButton { text: qsTr("Rules") }
        TabButton { text: qsTr("Statement") }
    }

    StackLayout {
        anchors.fill: parent
        anchors.margins: 12
        currentIndex: tabs.currentIndex

        LedgerView { bridge: root.ledgerBridge }
        BudgetView { bridge: root.budgetBridge }
        RulesView  { bridge: root.ruleBridge }
        ReportView { bridge: root.reportBridge }
    }
}
