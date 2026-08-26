// SPDX-License-Identifier: Apache-2.0
//
// The sample-lifecycle surface.
//
// Every action that carries a field a person types is rendered from the
// served schema through the shipped DynamicForm — there is no hand-built
// text field here (examples/IMPLEMENTATION.md rule 2). The zero-field
// transitions are plain buttons, because an action with no fields has no
// form to generate.
//
// Every enabled/disabled state is a read of the sample's own `state` field as
// the *model* reported it. The lifecycle table lives in
// lims::isLegalTransition and nothing here duplicates it — a button being
// enabled is a hint, and the model refuses an illegal transition on every
// dispatch path regardless (examples/IMPLEMENTATION.md rule 1: client gates
// are UX, not security).
//
// `sampleBridge` defaults to null so this file loads standalone in the
// offscreen engine-load smoke test.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

Item {
    id: page

    property var sampleBridge: null

    readonly property string sampleState: page.sampleBridge && page.sampleBridge.sample.state !== undefined
                                          ? page.sampleBridge.sample.state
                                          : ""

    /// The served `{actionType: schema}` document, parsed once.
    readonly property var schemas: page.sampleBridge ? JSON.parse(page.sampleBridge.schemasJson) : ({})

    /// The last schema-driven submission's outcome, as one line of text.
    property string reply: ""
    property bool replyIsError: false

    function report(message, isError) {
        page.reply = message
        page.replyIsError = isError
    }

    // Every form on this screen submits through `submitIfValid`, and
    // `submitIfValid` reports *both* outcomes on `replyReceived` — not on
    // `failed`, which carries only the typed invokables' errors. So this, and
    // not the `lastError` label in Main.qml, is where a refused registration,
    // rework or rejection arrives. Without it the operator clicks a button and
    // the screen does not change, which on a rung whose subject is refusals is
    // the entire story going missing.
    //
    // The same shape bookmarks' BookmarkListView.qml, pastebin's Main.qml and
    // polls' VoteView.qml use, for the same reason.
    Connections {
        target: page.sampleBridge

        function onReplyReceived(actionType, ok, payload) {
            if (!ok) {
                page.report(actionType + ": " + payload, true)
                return
            }
            page.report(actionType + " ok", false)
            // Only a form that was actually accepted is cleared: a refused
            // submission keeps what the operator typed, so the correction is
            // an edit rather than a re-entry.
            if (actionType === "RegisterClient")
                clientForm.resetFields()
            else if (actionType === "RegisterSample")
                sampleForm.resetFields()
            else if (actionType === "ReturnForRework")
                reworkForm.resetFields()
            else if (actionType === "RejectSample")
                rejectForm.resetFields()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: text !== ""
            color: page.replyIsError ? "red" : "black"
            text: page.reply
        }

        GroupBox {
            Layout.fillWidth: true
            title: "Register"

            ColumnLayout {
                anchors.fill: parent
                spacing: 6

                // Both registration forms come straight from
                // schemaJson<RegisterClient>() / schemaJson<RegisterSample>().
                // If either DTO grows a field, this file does not change.
                DynamicForm {
                    id: clientForm
                    Layout.fillWidth: true
                    actionType: "RegisterClient"
                    schema: page.schemas["RegisterClient"] || ({})
                    // Left unbound deliberately: a bound DynamicForm
                    // auto-submits once its required fields are engaged,
                    // which here would register a client per keystroke.
                    // `ready` is the submit gate and `previewLine` is the
                    // exact body the button hands over.
                    controller: null
                }
                Button {
                    Layout.fillWidth: true
                    text: "Register client"
                    enabled: page.sampleBridge !== null && clientForm.ready
                    onClicked: page.sampleBridge.submitIfValid("RegisterClient", clientForm.previewLine)
                }

                Label {
                    Layout.fillWidth: true
                    opacity: 0.7
                    text: "Latest client id: " + (page.sampleBridge ? page.sampleBridge.clientId : -1)
                }

                DynamicForm {
                    id: sampleForm
                    Layout.fillWidth: true
                    actionType: "RegisterSample"
                    schema: page.schemas["RegisterSample"] || ({})
                    controller: null
                }
                Button {
                    Layout.fillWidth: true
                    text: "Register sample"
                    enabled: page.sampleBridge !== null && sampleForm.ready
                    onClicked: page.sampleBridge.submitIfValid("RegisterSample", sampleForm.previewLine)
                }

                RowLayout {
                    Layout.fillWidth: true

                    Label { text: "Open existing" }
                    SpinBox {
                        id: openId
                        from: 1
                        to: 1000000
                    }
                    Button {
                        text: "Open sample"
                        enabled: page.sampleBridge !== null
                        // Not a schema form: `OpenSample` carries only the id
                        // this control already holds, and asking somebody to
                        // retype it into a generated field would be worse.
                        onClicked: page.sampleBridge.openSample(openId.value)
                    }
                }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: "Lifecycle"

            RowLayout {
                anchors.fill: parent
                spacing: 6

                Button {
                    text: "Receive"
                    enabled: page.sampleState === "registered"
                    onClicked: page.sampleBridge.receiveSample()
                }
                Button {
                    text: "Start work"
                    enabled: page.sampleState === "received"
                    onClicked: page.sampleBridge.startWork()
                }
                Button {
                    text: "Submit for verification"
                    enabled: page.sampleState === "in-progress"
                    onClicked: page.sampleBridge.submitForVerification()
                }
                Button {
                    text: "Publish"
                    enabled: page.sampleState === "to-be-verified"
                    onClicked: page.sampleBridge.publishSample()
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "Refresh"
                    enabled: page.sampleBridge !== null
                    onClicked: page.sampleBridge.refresh()
                }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: "Return or reject"

            ColumnLayout {
                anchors.fill: parent
                spacing: 6

                // Both carry a required `reason`, and both are rendered from
                // their own schema rather than from a shared hand-built text
                // field — which is also what keeps the "a reason is required"
                // rule in one place (the DTO's own validate()) instead of
                // duplicated as an `enabled:` expression here.
                DynamicForm {
                    id: reworkForm
                    Layout.fillWidth: true
                    actionType: "ReturnForRework"
                    schema: page.schemas["ReturnForRework"] || ({})
                    controller: null
                }
                Button {
                    Layout.fillWidth: true
                    text: "Return for rework"
                    enabled: page.sampleState === "to-be-verified" && reworkForm.ready
                    onClicked: page.sampleBridge.submitIfValid("ReturnForRework", reworkForm.previewLine)
                }

                DynamicForm {
                    id: rejectForm
                    Layout.fillWidth: true
                    actionType: "RejectSample"
                    schema: page.schemas["RejectSample"] || ({})
                    controller: null
                }
                Button {
                    Layout.fillWidth: true
                    text: "Reject"
                    enabled: (page.sampleState === "registered" || page.sampleState === "received")
                             && rejectForm.ready
                    onClicked: page.sampleBridge.submitIfValid("RejectSample", rejectForm.previewLine)
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
