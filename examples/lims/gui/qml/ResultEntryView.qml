// SPDX-License-Identifier: Apache-2.0
//
// The result-entry surface: the analysis picker fed by the read-only
// catalogue, the capture form, the result table, four-eyes verification, and
// the conflicts offline replay flagged.
//
// The capture form is rendered from schemaJson<CaptureConcentration>()
// through the shipped DynamicForm, and that is the point of this screen
// rather than an implementation detail. Everything §3, §4 and §5 of this
// rung argue about is carried in that one served document, and rendering it
// here is what puts it in front of the framework's *own* evaluator:
//
//  - `exactlyOneOf(value, qualifier)` — the sum-type encoding. The renderer's
//    submit gate is what makes "a number *and* a below-LOD flag"
//    unsubmittable, without this file knowing the rule exists.
//  - `requiredWhen`/`visibleWhen` over the dilution factor — the conditional
//    pair. The factor's field appears, and becomes required, only when the
//    preparation says "diluted".
//  - `x-decimalPlaces` and `x-unitAlternatives` on the reading — the exact
//    entry-unit machinery.
//
// A hidden field's draft value still travels; this file deliberately does not
// clear it, because the rung's §5 decision is that the *server* ignores a
// factor whose preparation says neat. An outcome that depended on this file
// remembering to blank a control would be one the server could not verify.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MorphForms

Item {
    id: page

    property var resultBridge: null

    /// The served `{actionType: schema}` document, parsed once.
    readonly property var schemas: page.resultBridge ? JSON.parse(page.resultBridge.schemasJson) : ({})

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            // Read-only catalogue: the picker exists so an operator can see
            // which analysis version id to enter, not to edit anything. The
            // capture form's own `analysisVersionId` field is where the value
            // is entered, because that is what the schema says it is.
            Label { text: "Catalogue" }
            ComboBox {
                id: analysisPicker
                Layout.fillWidth: true
                textRole: "name"
                model: page.resultBridge ? page.resultBridge.analyses : []
            }
            Label {
                opacity: 0.7
                text: page.resultBridge && analysisPicker.currentIndex >= 0
                      ? "version id " + page.resultBridge.analyses[analysisPicker.currentIndex].versionId
                      : ""
            }
            Button {
                text: "Refresh catalogue"
                enabled: page.resultBridge !== null
                onClicked: page.resultBridge.refreshAnalyses()
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: "Capture"

            ColumnLayout {
                anchors.fill: parent
                spacing: 6

                DynamicForm {
                    id: captureForm
                    Layout.fillWidth: true
                    actionType: "CaptureConcentration"
                    schema: page.schemas["CaptureConcentration"] || ({})
                    // Unbound on purpose: a bound DynamicForm auto-submits
                    // the moment its required fields are engaged, which for a
                    // lab reading would file a result mid-keystroke.
                    controller: null
                }

                Button {
                    Layout.fillWidth: true
                    text: "Capture"
                    // `ready` is the renderer's own verdict on the served
                    // x-rules — including `exactlyOneOf`, which is why there
                    // is one button here and not one per branch of the sum.
                    enabled: page.resultBridge !== null && captureForm.ready
                    onClicked: page.resultBridge.submitIfValid("CaptureConcentration", captureForm.previewLine)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                text: "Refresh results"
                enabled: page.resultBridge !== null
                onClicked: page.resultBridge.refreshResults()
            }
            Button {
                text: "Refresh conflicts"
                enabled: page.resultBridge !== null
                onClicked: page.resultBridge.refreshConflicts()
            }
            Item { Layout.fillWidth: true }
        }

        ListView {
            id: resultList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: page.resultBridge ? page.resultBridge.results : []

            delegate: RowLayout {
                required property var modelData
                width: resultList.width

                Label {
                    Layout.preferredWidth: 60
                    text: modelData.id
                }
                Label {
                    Layout.fillWidth: true
                    // A reading shows its exact decimal; a non-reading shows
                    // which of the three claims it is. Never a blank cell —
                    // "we did not look" and "below the detection limit" are
                    // different statements and the table says which.
                    text: modelData.hasValue
                          ? modelData.valueText + " " + modelData.unit
                          : modelData.qualifier
                }
                Label {
                    Layout.preferredWidth: 120
                    text: modelData.capturedBy
                }
                Button {
                    text: "Verify"
                    onClicked: page.resultBridge.verifyResult(modelData.id)
                }
            }
        }

        ListView {
            id: conflictList
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            clip: true
            visible: count > 0
            model: page.resultBridge ? page.resultBridge.conflicts : []

            delegate: Label {
                required property var modelData
                width: conflictList.width
                text: "conflict " + modelData.id + " — " + modelData.reason
                      + " (base v" + modelData.baseVersion
                      + " vs server v" + modelData.serverVersion + ") — " + modelData.status
            }
        }

        // Resolving a conflict is a form: a person chooses what to do and
        // types why. So it is rendered from schemaJson<ResolveConflict>()
        // like every other typed-field action, rather than from a
        // hand-written note field beside two buttons — which is also what
        // keeps "a reason is required" in the DTO's own validate() instead of
        // duplicated as an `enabled:` expression here.
        DynamicForm {
            id: resolveForm
            Layout.fillWidth: true
            visible: conflictList.count > 0
            actionType: "ResolveConflict"
            schema: page.schemas["ResolveConflict"] || ({})
            controller: null
        }
        Button {
            Layout.fillWidth: true
            visible: conflictList.count > 0
            text: "Resolve conflict"
            enabled: page.resultBridge !== null && resolveForm.ready
            onClicked: page.resultBridge.submitIfValid("ResolveConflict", resolveForm.previewLine)
        }

        Label {
            Layout.fillWidth: true
            color: "red"
            wrapMode: Text.Wrap
            visible: text !== ""
            text: page.resultBridge ? page.resultBridge.lastError : ""
        }
    }
}
