// SPDX-License-Identifier: Apache-2.0
//
// bookmarks' first screen. One schema-driven form and one button — there is
// no hand-built username field here, because there does not need to be: the
// generated form already renders Login's single `std::string username`
// member, complete with its required-gate (examples/IMPLEMENTATION.md rule 2,
// "schema-driven forms only"). If Login ever grows a second field, this file
// does not change.
//
// `formsController` defaults to null so this same file also loads with
// nothing wired up, which is exactly what the offscreen engine-load smoke
// test (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

Item {
    id: page

    /// The FormsBridge gui/main.cpp builds, or null when unwired.
    property var formsController: null

    /// schemaJson<Login>(), already parsed out of the controller's document.
    property var loginSchema: ({})

    /// Whatever the last submission reported, shown verbatim.
    property string status: ""
    property bool statusIsError: false

    Connections {
        target: page.formsController

        // Login's outcome arrives here like every other form's. The
        // *successful* case is handled by Main.qml, which navigates on
        // `loggedIn` — this only has to show a failure ("username is not a
        // valid principal", "handler not bound", ...) rather than leave the
        // user staring at a button that seemed to do nothing.
        function onReplyReceived(actionType, ok, payload) {
            if (actionType !== "Login")
                return
            page.status = ok ? "" : payload
            page.statusIsError = !ok
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
                  + "is real, server-signed and checked on every subsequent action — see "
                  + "bookmarks/dto/auth_dto.hpp for exactly what that does and does not mean."
        }

        DynamicForm {
            id: loginForm
            Layout.fillWidth: true
            actionType: "Login"
            schema: page.loginSchema
            // Deliberately not `controller: page.formsController`: a bound
            // DynamicForm auto-submits on every keystroke once its required
            // fields are engaged, which for Login would mint a token per typed
            // character. Left unbound it is a pure renderer/validator —
            // `ready` is the submit gate and `previewLine` is the exact JSON
            // body the button below hands over. Same reasoning, verbatim, as
            // pastebin's create form.
            controller: null
        }

        Button {
            Layout.fillWidth: true
            text: "Sign in"
            enabled: page.formsController !== null && loginForm.ready
            onClicked: page.formsController.submitIfValid("Login", loginForm.previewLine)
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
