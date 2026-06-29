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

    // ── Error toast ────────────────────────────────────────────────────────
    function showError(message) {
        toastText.text = message;
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
        width: toastText.implicitWidth + 36
        height: 44
        z: 1000
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Text {
            id: toastText
            anchors.centerIn: parent
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
}
