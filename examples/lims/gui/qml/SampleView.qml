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
                //
                // Bound, and safe to bind: both actions declare
                // `explicitSubmit = true` (lims/dto/sample_dto.hpp), so their
                // schemas carry `"x-submitMode": "explicit"` and the renderer
                // never auto-submits — it renders its own Submit button,
                // gated on the same `ready` state, and that click is the sole
                // trigger (docs/spec/forms/forms.md, "Explicit submit mode").
                DynamicForm {
                    id: clientForm
                    Layout.fillWidth: true
                    actionType: "RegisterClient"
                    schema: page.schemas["RegisterClient"] || ({})
                    controller: page.sampleBridge
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
                    controller: page.sampleBridge
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
                // duplicated as an `enabled:` expression here. Both actions
                // declare `explicitSubmit = true`, so each carries its own
                // gated Submit button — see the registration forms above for
                // why binding the controller directly is safe.
                //
                // Each form is still gated on the sample's current lifecycle
                // state: that is not something the schema can express (it
                // does not know which sample is attached), so it is applied
                // to the whole form rather than to a button — `enabled` on a
                // Frame propagates to every child, the Submit button
                // included.
                DynamicForm {
                    id: reworkForm
                    Layout.fillWidth: true
                    actionType: "ReturnForRework"
                    schema: page.schemas["ReturnForRework"] || ({})
                    controller: page.sampleBridge
                    enabled: page.sampleState === "to-be-verified"
                }

                DynamicForm {
                    id: rejectForm
                    Layout.fillWidth: true
                    actionType: "RejectSample"
                    schema: page.schemas["RejectSample"] || ({})
                    controller: page.sampleBridge
                    enabled: page.sampleState === "registered" || page.sampleState === "received"
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
