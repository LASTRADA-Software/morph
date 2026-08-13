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

    /// True once *any* ListPastes reply has arrived — including an empty one.
    /// Gates the bootstrap timer below; see it for why this exists.
    property bool listedOnce: false

    function report(message, isError) {
        root.status = message
        root.statusIsError = isError
    }

    // The first listing cannot simply be requested from Component.onCompleted.
    // In Remote mode AppContext::onReady() fires when the *socket* connects,
    // which is when gui/main.cpp builds the presenters — but a BridgeHandler's
    // registration is a round trip, and until its reply lands the handler's
    // `currentId` is still 0 and every dispatch through it fails fast with
    // "handler not bound" (morph/core/bridge.hpp). Verified, not theorised:
    // an unconditional refresh() on completion reliably reported exactly that
    // error and left the list empty on every launch against a real server.
    // morph exposes no "registration settled" seam to wait on today (the
    // neighbouring half of docs/findings/017), so the view layer retries —
    // which is where a timer belongs anyway (examples/TESTING.md presenter
    // rule 4). Bounded, not a poll loop: the very first reply, empty or not,
    // stops it forever. Local mode registers synchronously, so its first tick
    // always succeeds.
    Timer {
        interval: 150
        repeat: true
        running: root.pasteController !== null && !root.listedOnce
        triggeredOnStart: true
        onTriggered: root.pasteController.refresh()
    }

    Connections {
        target: root.pasteController

        function onListed(rows) {
            root.rows = rows
            if (!root.listedOnce) {
                root.listedOnce = true
                // Drop the "handler not bound" the bootstrap retries above
                // provoked; anything the user caused is older than this reply
                // and equally stale.
                root.report("", false)
            }
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

                DynamicForm {
                    id: createForm
                    Layout.fillWidth: true
                    actionType: "CreatePaste"
                    schema: root.schemas["CreatePaste"] || ({})
                    // Deliberately *not* `controller: root.formsController`.
                    // DynamicForm auto-submits the moment its required fields
                    // are engaged and on every keystroke after that — right for
                    // the calculator-shaped actions it was written against,
                    // catastrophic for CreatePaste, which would store one paste
                    // per typed character. Left unbound, the form is a pure
                    // renderer/validator: `ready` is its submit gate and
                    // `previewLine` is the exact JSON body it assembled, which
                    // the button below hands to the controller on demand.
                    controller: null
                }

                Button {
                    Layout.fillWidth: true
                    text: "Create paste"
                    enabled: root.formsController !== null && createForm.ready
                    onClicked: root.formsController.submitIfValid("CreatePaste", createForm.previewLine)
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
