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
        property var fetchCalls: []

        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            lastActionType = actionType
            lastBodyJson = bodyJson
            replyReceived(actionType, true, JSON.stringify({sum: 42}))
        }

        function fetchOptions(optionsAction, bodyJson) {
            fetchCalls.push({action: optionsAction, body: bodyJson})
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

    property var depSchema: ({
        properties: {
            country: { type: ["integer", "null"], "x-order": 0,
                       "x-optionsAction": "ListCountries", "x-optionValue": "id", "x-optionLabel": "name" },
            city: { type: ["integer", "null"], "x-order": 1,
                    "x-optionsAction": "ListCities", "x-optionValue": "id", "x-optionLabel": "name",
                    "x-optionsDependsOn": ["country"] }
        },
        required: ["country", "city"]
    })

    Component {
        id: formComponent
        DynamicForm {
            actionType: "TestAction"
            schema: testCase.testSchema
            controller: mockController
        }
    }

    Component {
        id: depFormComponent
        DynamicForm {
            actionType: "ShipTo"
            schema: testCase.depSchema
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

    function test_fetches_only_independent_choice_on_load() {
        mockController.fetchCalls = []
        var form = createTemporaryObject(depFormComponent, testCase)
        verify(form !== null)

        compare(mockController.fetchCalls.length, 1)
        compare(mockController.fetchCalls[0].action, "ListCountries")
        compare(mockController.fetchCalls[0].body, "{}")
    }

    function test_refetches_dependent_choice_when_parent_changes() {
        mockController.fetchCalls = []
        var form = createTemporaryObject(depFormComponent, testCase)
        verify(form !== null)

        form.setFieldValue("country", "1")

        compare(mockController.fetchCalls.length, 2)  // ListCountries (load) + ListCities (country set)
        compare(mockController.fetchCalls[1].action, "ListCities")
        compare(mockController.fetchCalls[1].body, '{"country":1}')
    }

    function test_clears_stale_child_selection_on_refetch() {
        mockController.fetchCalls = []
        var form = createTemporaryObject(depFormComponent, testCase)
        verify(form !== null)

        form.setFieldValue("country", "1")
        form.setFieldValue("city", "10")           // pretend the user picked city 10
        verify(form.fieldValues["city"] !== "")

        // Country changes: DynamicForm re-fetches; the mock replies with an
        // empty options list, so city 10 is no longer among the results.
        form.setFieldValue("country", "2")
        compare(form.fieldValues["city"], "")
    }
}
