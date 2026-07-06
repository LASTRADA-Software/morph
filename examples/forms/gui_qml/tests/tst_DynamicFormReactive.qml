// SPDX-License-Identifier: Apache-2.0
//
// Covers DynamicForm's auto-fire-on-valid behavior: with no submit button,
// the form must call controller.submitIfValid exactly once, the moment the
// assembled body becomes valid, and not before.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormReactive"

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)

        property int submitCount: 0
        property string lastActionType: ""
        property string lastBodyJson: ""

        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            lastActionType = actionType
            lastBodyJson = bodyJson
            replyReceived(actionType, true, JSON.stringify({sum: 42}))
        }

        function fetchOptions(optionsAction) {
            optionsReceived(optionsAction, true, "[]")
        }
    }

    property var testSchema: ({
        properties: {
            a: { type: "integer", "x-order": 0 },
            b: { type: "integer", "x-order": 1 }
        },
        required: ["a", "b"]
    })

    Component {
        id: formComponent
        DynamicForm {
            actionType: "TestAction"
            schema: testCase.testSchema
            controller: mockController
        }
    }

    function test_fires_automatically_without_button() {
        mockController.submitCount = 0
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)

        var fieldA = findChild(form, "field_a")
        var fieldB = findChild(form, "field_b")
        verify(fieldA !== null)
        verify(fieldB !== null)

        fieldA.text = "3"
        fieldB.text = "4"

        compare(mockController.submitCount, 1)
        compare(mockController.lastActionType, "TestAction")
        compare(form.resultText !== "", true)
    }

    function test_does_not_fire_when_incomplete() {
        mockController.submitCount = 0
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)

        var fieldA = findChild(form, "field_a")
        verify(fieldA !== null)
        fieldA.text = "3"
        compare(mockController.submitCount, 0)
    }
}
