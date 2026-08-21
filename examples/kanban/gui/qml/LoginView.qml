// SPDX-License-Identifier: Apache-2.0
//
// kanban's first screen. Dev-mode login: a username field, no password, no
// schema-driven form -- unlike bookmarks/polls/pastebin, this rung's GUI
// design spec (docs/superpowers/specs/2026-08-17-kanban-gui-design.md §4)
// binds plain QVariantMap/QVariantList property bags rather than MorphForms'
// DynamicForm, so Login's one field is a hand-built TextField here.
//
// `projectAdminBridge` defaults to null so this same file also loads with
// nothing wired up, which is exactly what the offscreen engine-load smoke
// test (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page

    /// The ProjectAdminBridge gui/main.cpp builds, or null when unwired.
    property var projectAdminBridge: null

    /// Whatever the last login attempt reported, shown verbatim.
    property string status: ""
    property bool statusIsError: false

    Connections {
        target: page.projectAdminBridge

        // Main.qml navigates on the successful loggedIn(principal) signal --
        // this only has to show a failure ("invalid principal", "handler not
        // bound", ...) rather than leave the user staring at a button that
        // seemed to do nothing.
        function onFailed(message) {
            page.status = message
            page.statusIsError = true
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(page.width - 32, 460)
        spacing: 8

        Label {
            Layout.fillWidth: true
            font.bold: true
            font.pixelSize: 18
            text: "Sign in"
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            text: "Dev-mode login: a username, no password. The token the server mints for it "
                  + "is real, server-signed and checked on every subsequent action -- see "
                  + "kanban/dto/auth_dto.hpp for exactly what that does and does not mean."
        }

        TextField {
            id: usernameField
            Layout.fillWidth: true
            placeholderText: "username"
            onAccepted: signInButton.clicked()
        }

        Button {
            id: signInButton
            Layout.fillWidth: true
            text: "Sign in"
            enabled: page.projectAdminBridge !== null && usernameField.text.length > 0
            onClicked: {
                page.status = ""
                page.statusIsError = false
                page.projectAdminBridge.login(usernameField.text)
            }
        }

        Label {
            Layout.fillWidth: true
            visible: page.status !== ""
            wrapMode: Text.Wrap
            color: page.statusIsError ? "#d33" : palette.text
            text: page.status
        }
    }
}
