// SPDX-License-Identifier: Apache-2.0
//
// pastebin's desktop shell. Three panes' worth of behavior, none of it
// domain logic (examples/TESTING.md presenter rule 6, "QML is bindings-only"):
//
//   * the create form is the shipped MorphForms renderer (DynamicForm) driven
//     entirely by schemaJson<CreatePaste>() — nothing here knows CreatePaste
//     has a `syntax` field, a burn budget, or an expiry;
//   * the list and the detail pane are read-only displays of server-computed
//     state relayed by PastePresenter (via gui/main.cpp's PasteBridge);
//   * every error string shown is the model's own `what()`.
//
// `formsController` / `pasteController` are supplied by gui/main.cpp through
// QQmlApplicationEngine::setInitialProperties. They default to null so this
// same file also loads with nothing wired up, which is exactly what the
// offscreen engine-load smoke test (tests/test_gui_qml_smoke.cpp) does.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

ApplicationWindow {
    id: root
    width: 980
    height: 720
    visible: true
    title: "pastebin — morph application ladder, rung 1"

    property var formsController: null
    property var pasteController: null

    property var schemas: root.formsController ? JSON.parse(root.formsController.schemasJson) : ({})
    property var rows: []
    property var currentPaste: null
    property string status: ""
    property bool statusIsError: false

    function report(message, isError) {
        root.status = message
        root.statusIsError = isError
    }

    // The first listing cannot simply be requested from Component.onCompleted.
    // In Remote mode AppContext::onReady() fires when the *socket* connects,
    // which is when gui/main.cpp builds the presenters — but a BridgeHandler's
    // registration is a round trip, and until its reply lands every dispatch
    // through it fails fast with "handler not bound" (morph/core/bridge.hpp).
    // Verified, not theorised: an unconditional refresh() on completion
    // reliably reported exactly that error and left the list empty on every
    // launch against a real server. `PasteBridge::bound` (backed by
    // `Bridge::whenBound()`) is that round trip's settlement signal — Local
    // mode's handler is already bound by construction, so this fires
    // synchronously there.
    Connections {
        target: root.pasteController

        function onBound() {
            root.pasteController.refresh()
        }

        function onListed(rows) {
            root.rows = rows
            root.report("", false)
        }

        function onLoaded(paste) {
            root.currentPaste = paste
            root.report("opened " + paste.id + " — read " + paste.readCount + " time(s)", false)
            // A read is a mutation in this rung: GetPaste consumes one unit of
            // burn budget, and the read that spends the last unit destroys the
            // paste server-side (README, "burn-after-read atomicity"). Re-listing
            // is what makes that visible instead of leaving a stale row on screen.
            root.pasteController.refresh()
        }

        function onRemoved() {
            root.currentPaste = null
            root.report("deleted", false)
            root.pasteController.refresh()
        }

        function onFailed(message) {
            root.report(message, true)
        }
    }

    Connections {
        target: root.formsController

        // The create form submits through PasteFormsController, not through
        // PastePresenter, so this — not `pasteController.created` — is where a
        // create's outcome arrives.
        function onReplyReceived(actionType, ok, payload) {
            if (!ok) {
                root.report(payload, true)
                return
            }
            root.report(actionType + " ok: " + payload, false)
            createForm.resetFields()
            if (root.pasteController)
                root.pasteController.refresh()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        Label {
            Layout.fillWidth: true
            visible: root.status !== ""
            wrapMode: Text.Wrap
            color: root.statusIsError ? "#d33" : palette.text
            text: root.status
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            ColumnLayout {
                Layout.preferredWidth: 430
                Layout.fillHeight: true
                spacing: 8

                // Bound, and safe to bind: `CreatePaste` declares
                // `explicitSubmit = true` (pastebin/dto/paste_dto.hpp), so
                // its schema carries `"x-submitMode": "explicit"` and the
                // renderer never auto-submits — it renders its own Submit
                // button, enabled only while the form is ready, and that
                // click is the sole trigger (docs/spec/forms/forms.md,
                // "Explicit submit mode").
                DynamicForm {
                    id: createForm
                    Layout.fillWidth: true
                    actionType: "CreatePaste"
                    schema: root.schemas["CreatePaste"] || ({})
                    controller: root.formsController
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        text: "Refresh list"
                        enabled: root.pasteController !== null
                        onClicked: root.pasteController.refresh()
                    }

                    Label {
                        Layout.fillWidth: true
                        opacity: 0.7
                        text: root.rows.length + " public paste(s)"
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.rows

                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        text: modelData.id + "  ·  " + modelData.syntax + "  ·  " + modelData.visibility
                              + "  ·  " + modelData.createdAt
                        onClicked: {
                            if (root.pasteController)
                                root.pasteController.open(modelData.id)
                        }
                    }
                }
            }

            PasteView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                paste: root.currentPaste
                onDeleteRequested: pasteId => {
                    if (root.pasteController)
                        root.pasteController.remove(pasteId)
                }
            }
        }
    }
}
