// SPDX-License-Identifier: Apache-2.0
//
// Writes the renderer makes on its own -- re-seeding a control the tabbed
// layout just rebuilt, or clearing the form with resetFields() -- are not user
// actions, so on an auto-submit form they must never reach
// controller.submitIfValid (docs/spec/forms/forms.md, "Tab switching destroys
// and rebuilds controls"; docs/spec/forms/views.md, "The reset is not
// optional"). Both fixtures count submissions through a real controller, which
// is what makes the suppression observable at all: with `controller: null` a
// re-submit is invisible.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormReseedSubmit"
    visible: true

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)

        property int submitCount: 0

        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            replyReceived(actionType, true, "{}")
        }

        function fetchOptions(optionsAction) {
            optionsReceived(optionsAction, true, "[]")
        }
    }

    Component {
        id: tabbedFormComponent
        DynamicForm {
            actionType: "TabResubmit"
            controller: mockController
            schema: ({
                "properties": {
                    "a": { "type": "integer", "x-order": 0, "x-section": 0 },
                    "b": { "type": ["integer", "null"], "x-order": 1, "x-section": 1 }
                },
                "required": ["a"],
                "x-layout": {
                    "groups": [
                        { "title": "One", "kind": "tab", "fields": ["a"] },
                        { "title": "Two", "kind": "tab", "fields": ["b"] }
                    ]
                }
            })
        }
    }

    // No required member: an empty form is already `ready`, so clearing it
    // leaves it ready and the reset's own closing revalidate is the only thing
    // that could submit.
    Component {
        id: optionalFormComponent
        DynamicForm {
            actionType: "ResetResubmit"
            controller: mockController
            schema: ({
                "properties": {
                    "note": { "type": ["string", "null"], "x-order": 0 }
                },
                "required": []
            })
        }
    }

    function test_tab_switch_does_not_resubmit_a_ready_form() {
        var form = createTemporaryObject(tabbedFormComponent, testCase)
        verify(form !== null)
        var tabset = findChild(form, "tabset")
        verify(tabset !== null)

        mockController.submitCount = 0
        findChild(form, "field_a").text = "1"
        compare(form.ready, true)
        compare(mockController.submitCount, 1)   // the user's own edit

        tabset.currentTab = 1                    // destroys tab One's controls
        tabset.currentTab = 0                    // ...and rebuilds them
        tryVerify(function() {
            var again = findChild(form, "field_a")
            return again !== null && again.text === "1"
        })
        compare(form.ready, true)
        compare(mockController.submitCount, 1)
    }

    function test_resetFields_does_not_submit_a_form_that_stays_ready() {
        var form = createTemporaryObject(optionalFormComponent, testCase)
        verify(form !== null)
        findChild(form, "field_note").text = "hello"

        mockController.submitCount = 0
        form.resetFields()
        compare(form.ready, true)
        compare(mockController.submitCount, 0)
    }
}
