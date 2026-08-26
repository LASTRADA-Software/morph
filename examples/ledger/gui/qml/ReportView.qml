// SPDX-License-Identifier: Apache-2.0
//
// The monthly statement screen: submit a report, watch it poll, read the
// result. The one screen in this rung where the answer does not arrive with
// the request.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: view
    property var bridge: null
    spacing: 8

    /// The client's own UTC offset in minutes, which is what decides whether
    /// a 23:30-local transaction belongs to this month or the next one
    /// (design spec §9). Read from the running system rather than hardcoded,
    /// because the whole point of the parameter is that it is the *client's*
    /// calendar, not the server's.
    function localOffsetMinutes() {
        return -(new Date().getTimezoneOffset());
    }

    Label { text: qsTr("Monthly statement"); font.bold: true }

    RowLayout {
        Layout.fillWidth: true
        SpinBox { id: yearBox; from: 1970; to: 2999; value: 2026; editable: true }
        SpinBox { id: monthBox; from: 1; to: 12; value: 1; editable: true }
        Button {
            text: qsTr("Request")
            enabled: view.bridge !== null && view.bridge.status !== "pending"
            onClicked: view.bridge.requestMonthlyStatement(yearBox.value, monthBox.value,
                                                           view.localOffsetMinutes())
        }
        Label {
            // Four states, not a spinner: "never asked" and "still working"
            // are different answers to the only question this screen asks.
            text: view.bridge ? view.bridge.status : "idle"
            font.bold: true
            opacity: 0.8
        }
        BusyIndicator {
            running: view.bridge ? view.bridge.status === "pending" : false
            visible: running
        }
    }

    Label {
        text: qsTr("Offset applied: %1 minutes from UTC").arg(view.localOffsetMinutes())
        opacity: 0.6
    }

    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: view.bridge ? view.bridge.lines : []
        delegate: RowLayout {
            width: ListView.view ? ListView.view.width : 0
            Label { text: modelData.currency; Layout.fillWidth: true }
            Label {
                // The count is what makes the local-month boundary visible at
                // all: a per-currency total is zero for any whole set of
                // transactions, so it cannot distinguish one month from
                // another.
                text: qsTr("%1 transactions").arg(modelData.transactionCount)
                opacity: 0.8
            }
            Label {
                // Pre-rendered by the bridge with `ledger::formatMoney`
                // (design spec §7's no-float rule): QML has only IEEE
                // doubles, so dividing the exact triple here would undo
                // `Rational`'s exactness in the last three lines of the path.
                text: modelData.amountText
                font.family: "monospace"
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
