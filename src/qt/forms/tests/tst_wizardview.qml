// SPDX-License-Identifier: Apache-2.0
//
// Covers WizardView's stepper logic against a mock controller: Next stays
// disabled until the current step's action has actually replied ok, Next/
// Back move currentIndex, and prefill copies a resolved value into the next
// step's fieldValues (see WizardView.qml's file header for why the widget's
// displayed text is not also synchronized).

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "WizardView"

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)

        property var resolvedValues: ({})

        function submitIfValid(actionType, bodyJson) {
            if (actionType === "WizStepOne") {
                resolvedValues["WizStepOne.id"] = "1"
                replyReceived(actionType, true, JSON.stringify({ id: 1, label: "x" }))
            } else {
                replyReceived(actionType, true, JSON.stringify({ summary: "ok" }))
            }
        }

        function fetchOptions(optionsAction) {
            optionsReceived(optionsAction, true, "[]")
        }

        function resolvedValue(path) {
            return resolvedValues[path] !== undefined ? resolvedValues[path] : ""
        }
    }

    property var testWizardSchema: ({
        "w-title": "Test flow",
        "w-steps": [
            { action: "WizStepOne", title: "One" },
            { action: "WizStepTwo", title: "Two", prefill: { refId: "WizStepOne.id" } }
        ]
    })

    property var testSchemas: ({
        WizStepOne: { properties: { label: { type: "string", "x-order": 0 } }, required: ["label"] },
        WizStepTwo: { properties: { refId: { type: "integer", "x-order": 0 } }, required: ["refId"] }
    })

    Component {
        id: wizardComponent
        WizardView {
            wizardId: "TestWizard"
            wizardSchema: testCase.testWizardSchema
            schemas: testCase.testSchemas
            controller: mockController
        }
    }

    function test_next_disabled_until_step_replies() {
        var wizard = createTemporaryObject(wizardComponent, testCase)
        verify(wizard !== null)

        var nextButton = findChild(wizard, "wizardNext")
        verify(nextButton !== null)
        compare(nextButton.enabled, false)

        var labelField = findChild(wizard, "field_label")
        verify(labelField !== null)
        labelField.text = "sample"

        tryCompare(nextButton, "enabled", true)
    }

    function test_next_advances_and_back_returns() {
        var wizard = createTemporaryObject(wizardComponent, testCase)
        var labelField = findChild(wizard, "field_label")
        labelField.text = "sample"
        var nextButton = findChild(wizard, "wizardNext")
        tryCompare(nextButton, "enabled", true)

        nextButton.clicked()
        compare(wizard.currentIndex, 1)

        var backButton = findChild(wizard, "wizardBack")
        backButton.clicked()
        compare(wizard.currentIndex, 0)
    }

    function test_prefill_copies_resolved_value_into_next_step() {
        var wizard = createTemporaryObject(wizardComponent, testCase)
        var labelField = findChild(wizard, "field_label")
        labelField.text = "sample"
        var nextButton = findChild(wizard, "wizardNext")
        tryCompare(nextButton, "enabled", true)
        nextButton.clicked()

        compare(wizard.currentIndex, 1)
        var stepTwoForm = wizard.currentForm()
        verify(stepTwoForm !== null)
        compare(stepTwoForm.fieldValues["refId"], "1")
    }
}
