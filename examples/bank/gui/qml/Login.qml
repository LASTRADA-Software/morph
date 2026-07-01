// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    Panel {
        anchors.centerIn: parent
        width: 380
        height: col.implicitHeight + 64

        ColumnLayout {
            id: col
            anchors.fill: parent
            anchors.margins: 32
            spacing: 12

            Text {
                text: "Morph Bank"
                font.pixelSize: 26
                font.weight: Font.Bold
                color: theme.ink
            }
            Text {
                text: "Sign in to your account"
                color: theme.inkSoft
                Layout.bottomMargin: 6
            }
            Field {
                id: username
                placeholderText: "Username"
                Layout.fillWidth: true
            }
            Field {
                id: password
                placeholderText: "Password"
                echoMode: TextInput.Password
                Layout.fillWidth: true
                onAccepted: app.login(username.text, password.text)
            }
            Field {
                id: displayName
                placeholderText: "Display name (for new accounts)"
                Layout.fillWidth: true
            }
            AppButton {
                text: "Sign in"
                variant: "primary"
                Layout.fillWidth: true
                Layout.topMargin: 4
                onClicked: app.login(username.text, password.text)
            }
            AppButton {
                text: "Create account"
                variant: "ghost"
                Layout.alignment: Qt.AlignHCenter
                onClicked: app.registerUser(username.text, password.text, displayName.text)
            }
        }
    }
}
