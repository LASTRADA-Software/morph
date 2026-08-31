// SPDX-License-Identifier: Apache-2.0
//
// kanban's first screen. One schema-driven form and nothing else — no
// hand-built username field and no hand-built submit button: the generated
// form renders `Login`'s single `std::string username` member with its
// required-gate, and the renderer's own Submit button submits it
// (examples/IMPLEMENTATION.md rule 2, "schema-driven forms only"). If `Login`
// ever grows a second field, this file does not change.
//
// This screen used to be a hand-built `TextField` + `Button`, on the strength
// of this rung's GUI design spec
// (docs/superpowers/specs/2026-08-17-kanban-gui-design.md §4) — which settles
// the two-bridge/property-bag architecture and says nothing about forms at
// all, so it never justified the exception rule 2 requires. See morph#344, and
// the rung README's "morph subsystems exercised" section for what is still
// hand-built here and what that costs.
//
// `projectAdminBridge` defaults to null so this same file also loads with
// nothing wired up, which is exactly what the offscreen engine-load smoke
// test (tests/test_gui_qml_smoke.cpp) does — and why `loginSchema` is read
// defensively below rather than assuming a live bridge.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

Item {
    id: page

    /// The ProjectAdminBridge gui/main.cpp builds, or null when unwired.
    property var projectAdminBridge: null

    /// schemaJson<Login>(), parsed out of the bridge's schema document.
    /// `({})` while unwired, which is what the smoke test loads.
    readonly property var loginSchema: page.projectAdminBridge === null
                                       ? ({})
                                       : (JSON.parse(page.projectAdminBridge.schemasJson)["Login"] || ({}))

    /// Whatever the last login attempt reported, shown verbatim.
    property string status: ""
    property bool statusIsError: false

    Connections {
        target: page.projectAdminBridge

        // Login's outcome arrives here like every other form's. Main.qml
        // navigates on the successful loggedIn(principal) signal -- this only
        // has to show a failure ("invalid principal", "handler not bound",
        // ...) rather than leave the user staring at a button that seemed to
        // do nothing.
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
                  + "is real, server-signed and checked on every subsequent action -- see "
                  + "kanban/dto/auth_dto.hpp for exactly what that does and does not mean."
        }

        DynamicForm {
            Layout.fillWidth: true
            actionType: "Login"
            schema: page.loginSchema
            // Bound, and safe to bind: `Login` declares `explicitSubmit = true`
            // (kanban/dto/auth_dto.hpp), so its schema carries
            // `"x-submitMode": "explicit"` and the renderer never auto-submits
            // -- it renders its own Submit button, gated on the same `ready`
            // state, and that click is the sole trigger
            // (docs/spec/forms/forms.md, "Explicit submit mode").
            controller: page.projectAdminBridge
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
