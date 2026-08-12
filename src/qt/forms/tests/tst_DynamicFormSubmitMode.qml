// SPDX-License-Identifier: Apache-2.0
//
// Covers the schema-carried "x-submitMode": "explicit" flag (docs/spec/
// forms/forms.md, "Explicit submit mode"): a schema declaring it suppresses
// DynamicForm's default auto-fire-on-validity behavior and instead renders
// an explicit submit Button, enabled only while `ready`, that the user must
// activate to call controller.submitIfValid. A schema that omits the flag
// (or sets any other value) keeps today's auto-submit-on-validity behavior,
// unchanged.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormSubmitMode"
    visible: true

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)

        property int submitCount: 0
        property string lastBody: ""

        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            lastBody = bodyJson
            replyReceived(actionType, true, JSON.stringify({booked: true}))
        }

        function fetchOptions(optionsAction) {
            optionsReceived(optionsAction, true, "[]")
        }
    }

    // --- default (auto-submit) schema: no x-submitMode at all ---------------

    property var autoSchema: ({
        properties: {
            name: { type: "string", "x-order": 0 }
        },
        required: ["name"]
    })

    Component {
        id: autoFormComponent
        DynamicForm {
            actionType: "CFR_BookRoom"
            schema: testCase.autoSchema
            controller: mockController
        }
    }

    function test_default_schema_still_auto_submits_on_validity() {
        mockController.submitCount = 0
        var form = createTemporaryObject(autoFormComponent, testCase)
        verify(form !== null)

        findChild(form, "field_name").text = "Alice"
        compare(form.ready, true)
        compare(mockController.submitCount, 1)  // unchanged auto-fire behavior
    }

    function test_default_schema_renders_no_explicit_submit_button() {
        var form = createTemporaryObject(autoFormComponent, testCase)
        verify(form !== null)
        var button = findChild(form, "submitButton")
        compare(button, null)
    }

    // --- explicit-submit schema: "x-submitMode": "explicit" -----------------

    property var explicitSchema: ({
        properties: {
            name: { type: "string", "x-order": 0 }
        },
        required: ["name"],
        "x-submitMode": "explicit"
    })

    Component {
        id: explicitFormComponent
        DynamicForm {
            actionType: "CFR_BookRoom"
            schema: testCase.explicitSchema
            controller: mockController
        }
    }

    function test_explicit_submit_mode_suppresses_auto_fire() {
        mockController.submitCount = 0
        var form = createTemporaryObject(explicitFormComponent, testCase)
        verify(form !== null)

        findChild(form, "field_name").text = "Alice"
        compare(form.ready, true)
        compare(mockController.submitCount, 0)  // ready, but must not auto-fire
    }

    function test_explicit_submit_mode_renders_a_submit_button() {
        var form = createTemporaryObject(explicitFormComponent, testCase)
        verify(form !== null)
        var button = findChild(form, "submitButton")
        verify(button !== null)
    }

    function test_explicit_submit_button_disabled_until_ready() {
        var form = createTemporaryObject(explicitFormComponent, testCase)
        verify(form !== null)
        var button = findChild(form, "submitButton")
        verify(button !== null)
        compare(button.enabled, false)  // name still blank -> not ready

        findChild(form, "field_name").text = "Alice"
        compare(form.ready, true)
        compare(button.enabled, true)
    }

    function test_clicking_explicit_submit_button_calls_submitIfValid() {
        mockController.submitCount = 0
        var form = createTemporaryObject(explicitFormComponent, testCase)
        verify(form !== null)

        findChild(form, "field_name").text = "Alice"
        compare(form.ready, true)
        compare(mockController.submitCount, 0)

        var button = findChild(form, "submitButton")
        mouseClick(button)
        compare(mockController.submitCount, 1)
        compare(mockController.lastBody, form.previewLine)
    }

    function test_explicit_submit_button_stays_disabled_while_not_ready() {
        mockController.submitCount = 0
        var form = createTemporaryObject(explicitFormComponent, testCase)
        verify(form !== null)

        var button = findChild(form, "submitButton")
        verify(button !== null)
        compare(button.enabled, false)
        compare(mockController.submitCount, 0)
    }
}
