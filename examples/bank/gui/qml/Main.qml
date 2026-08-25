// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic

ApplicationWindow {
    id: window
    visible: true
    width: 1160
    height: 760
    title: "Morph Bank"
    color: theme.paper

    // Swap between the login screen and the signed-in shell.
    Loader {
        anchors.fill: parent
        sourceComponent: app.authenticated ? shellComponent : loginComponent
    }
    Component { id: loginComponent; Login {} }
    Component { id: shellComponent; AppShell {} }

    // ── Toast ──────────────────────────────────────────────────────────────
    // One transient strip for both outcomes, told apart by the dot: money
    // moving is the one thing in this app a user must be able to confirm
    // happened, and a screen that only speaks up when something goes wrong
    // leaves "did that go through?" unanswered.
    function showError(message) { window.showToast(message, theme.bad) }
    function showNotice(message) { window.showToast(message, theme.good) }
    function showToast(message, accent) {
        toastText.text = message;
        toastDot.color = accent;
        toast.opacity = 1;
        toastTimer.restart();
    }
    Timer { id: toastTimer; interval: 3200; onTriggered: toast.opacity = 0 }

    Rectangle {
        id: toast
        opacity: 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        radius: 10
        color: "#2A2724"
        width: toastText.implicitWidth + 50
        height: 44
        z: 1000
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Rectangle {
            id: toastDot
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 16
            width: 8
            height: 8
            radius: 4
            color: theme.bad
        }
        Text {
            id: toastText
            objectName: "toastText"
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: toastDot.right
            anchors.leftMargin: 10
            color: "#FFFFFF"
            font.weight: Font.Medium
        }
    }

    Connections { target: app; function onError(m) { window.showError(m) } }
    Connections { target: accounts; function onError(m) { window.showError(m) } }
    Connections { target: txns; function onError(m) { window.showError(m) } }
    Connections { target: cards; function onError(m) { window.showError(m) } }
    Connections { target: payees; function onError(m) { window.showError(m) } }
    Connections { target: loans; function onError(m) { window.showError(m) } }

    // The success half. `payees.paid` is the *only* feedback a bill payment
    // produces at all — PayeeController::payBill reloads nothing on success —
    // and `txns.posted` covers the deposit/withdrawal/transfer that a refreshed
    // history does not confirm on its own (a transfer's other side is on a
    // different screen).
    Connections { target: txns; function onPosted() { window.showNotice("Transaction posted") } }
    Connections { target: payees; function onPaid() { window.showNotice("Bill paid") } }
}
