// SPDX-License-Identifier: Apache-2.0
//
// Steps through one wizard's `w-steps`, one action-form at a time. A step is
// an ordinary action form (DynamicForm, reused verbatim) — the wizard only
// adds sequencing (Back/Next) and prefill (copying an earlier step's
// captured values into a later step's fields). Every step's DynamicForm
// instance is created once (Repeater, not Loader) and kept alive for the
// wizard's lifetime inside a StackLayout, so entered values survive
// Back/Next navigation without re-entry.
//
// Fully generic/duck-typed over `controller` (replyReceived/optionsReceived
// signals, submitIfValid/fetchOptions/resolvedValue methods) exactly like
// CollectionView.qml — it ships alongside DynamicForm/CollectionView in the
// MorphForms module rather than in examples/forms/gui_qml, the demo
// consumer, for the same reason views.md gives for CollectionView.qml.
//
// Prefill updates the target step's fieldValues (and therefore what gets
// submitted) immediately via DynamicForm.setFieldValue; it does not
// synchronize the widget's *displayed* text/selection, since DynamicForm's
// fields are write-only from the widget's perspective (typing updates
// fieldValues; nothing pushes fieldValues back into the widget) — a
// pre-existing characteristic of DynamicForm.qml, not something this wizard
// layer introduces. See docs/spec/forms/workflows_navigation.md.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: wizard

    property string wizardId
    property var wizardSchema    // the w-* document for this wizard
    property var schemas         // {actionType: schema} — same map DynamicForm reads from
    property var controller

    property var steps: wizardSchema["w-steps"] || []
    property int currentIndex: 0

    // A plain two-hop property chain (Repeater.count/currentIndex ->
    // resultOk/resultText), not a function call: reading a property through
    // a user-defined QML function inside another binding does not reliably
    // register as a dependency of that binding (observed empirically with
    // `enabled: ... && wizard.currentForm().resultOk ...` never
    // re-evaluating after the underlying DynamicForm's result changed), so
    // the Next button's `enabled` binding below reads this property chain
    // directly instead of calling currentForm().
    property var currentStepItem: stepRepeater.count > wizard.currentIndex ? stepRepeater.itemAt(wizard.currentIndex)
                                                                            : null
    readonly property bool currentStepDone: currentStepItem !== null && currentStepItem.resultOk
                                             && currentStepItem.resultText !== ""

    function currentForm() { return stepRepeater.itemAt(wizard.currentIndex) }

    function applyPrefill(index) {
        const step = wizard.steps[index]
        const prefill = (step && step.prefill) || {}
        const targetForm = stepRepeater.itemAt(index)
        if (!targetForm)
            return
        for (const field in prefill) {
            const value = wizard.controller.resolvedValue(prefill[field])
            if (value !== "")
                targetForm.setFieldValue(field, value)
        }
    }

    function goNext() {
        if (wizard.currentIndex >= wizard.steps.length - 1)
            return
        wizard.currentIndex += 1
        wizard.applyPrefill(wizard.currentIndex)
    }

    function goBack() {
        if (wizard.currentIndex <= 0)
            return
        wizard.currentIndex -= 1
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 8

        Label {
            text: (wizard.wizardSchema["w-title"] || wizard.wizardId)
                  + "  (" + (wizard.currentIndex + 1) + " / " + wizard.steps.length + ")"
            font.bold: true
            font.pixelSize: 16
        }

        StackLayout {
            Layout.fillWidth: true
            currentIndex: wizard.currentIndex

            Repeater {
                id: stepRepeater
                model: wizard.steps

                DynamicForm {
                    required property var modelData
                    Layout.fillWidth: true
                    actionType: modelData.action
                    schema: wizard.schemas[modelData.action] || ({})
                    controller: wizard.controller
                }
            }
        }

        RowLayout {
            Button {
                objectName: "wizardBack"
                text: "Back"
                enabled: wizard.currentIndex > 0
                onClicked: wizard.goBack()
            }
            Button {
                objectName: "wizardNext"
                text: "Next"
                enabled: wizard.currentIndex < wizard.steps.length - 1 && wizard.currentStepDone
                onClicked: wizard.goNext()
            }
            Label {
                visible: wizard.currentIndex >= wizard.steps.length - 1
                text: "Last step — fill it in to finish"
                opacity: 0.6
                font.italic: true
            }
        }
    }
}
