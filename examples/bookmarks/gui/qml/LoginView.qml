// SPDX-License-Identifier: Apache-2.0
//
// bookmarks' first screen. One schema-driven form and nothing else — there is
// no hand-built username field here, and no hand-built submit button either:
// the generated form renders Login's single `std::string username` member
// with its required-gate, and the renderer's own explicit Submit button
// submits it (examples/IMPLEMENTATION.md rule 2, "schema-driven forms only").
// If Login ever grows a second field, this file does not change.
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
            Layout.fillWidth: true
            actionType: "Login"
            schema: page.loginSchema
            // Bound, and safe to bind: `Login` declares `explicitSubmit = true`
            // (bookmarks/dto/auth_dto.hpp), so its schema carries
            // `"x-submitMode": "explicit"` and the renderer never auto-submits
            // — it renders its own Submit button, gated on the same `ready`
            // state, and that click is the sole trigger
            // (docs/spec/forms/forms.md, "Explicit submit mode").
            controller: page.formsController
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
