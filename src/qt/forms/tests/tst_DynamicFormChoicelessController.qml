// SPDX-License-Identifier: Apache-2.0
//
// A controller that serves no `morph::forms::Choice` field must load a form
// without the engine warning about it (morph#387).
//
// `optionsReceived` only exists on a controller that serves a Choice. A
// controller that serves none does not declare it, and deliberately does not:
// `bookmarks::gui::BookmarkFormsController`'s own "No `fetchOptions()`" note
// records that adding one with nothing to call it would be a stub. So the
// sanctioned shape used to warn once per form instance, the moment a real
// controller was attached:
//
//   QML Connections: Detected function "onOptionsReceived" in Connections
//   element. This is probably intended to be a signal handler but no signal of
//   the target matches the name.
//
// Every rung's rule-6 smoke test loads its roots with all controllers `null`,
// and a `Connections` with a null target matches nothing and warns about
// nothing -- which is why the one QML test each rung ships never saw it.
//
// The fix is a second `Connections` block for the optional signal alone, whose
// target is gated on the controller actually declaring it, so `replyReceived`
// -- which every controller has -- keeps warning when it does not match. Both
// halves are asserted here: a blanket `ignoreUnknownSignals` on the one
// original block would pass the first test and fail the third.
//
// The gate is deliberately narrower than `ignoreUnknownSignals: true` on this
// block, which would also pass all three. That flag silences a *misspelled*
// `onOptionsReceived` on a controller that does serve a Choice, turning an
// empty combo box and a form that never reaches `ready` into a failure with no
// diagnostic. Only a source mutation separates the two, so it is checked that
// way rather than asserted here: misspell the handler in DynamicForm.qml and
// the second test below fails with the engine's "no signal of the target
// matches the name" warning under the gate, and silently -- on the missing
// options alone -- under the flag.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormChoicelessController"
    visible: true

    // The sanctioned choiceless shape: `replyReceived` and `submitIfValid`,
    // and nothing options-related.
    QtObject {
        id: choicelessController
        signal replyReceived(string actionType, bool ok, string payload)
        function submitIfValid(actionType, bodyJson) {}
    }

    // Serves a Choice, so it carries both signals -- the control that shows
    // the suppression did not cost delivery.
    QtObject {
        id: choiceController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)
        function submitIfValid(actionType, bodyJson) {}
        function fetchOptions(optionsAction, body) {
            optionsReceived(optionsAction, true, '[{"id": 4, "name": "Bravo"}]')
        }
    }

    // Neither signal. Not a shape anything ships -- it stands in for the
    // misspelled `onReplyReceived` the strict block exists to catch.
    QtObject {
        id: mutePlainObject
        function submitIfValid(actionType, bodyJson) {}
    }

    property var plainSchema: ({
        "type": "object",
        "properties": {
            "title": { "type": "string", "x-order": 0, "title": "Title" },
            "count": { "type": "integer", "x-order": 1, "title": "Count" }
        },
        "required": ["title"],
        "title": "CreateNote"
    })

    property var choiceSchema: ({
        "type": "object",
        "properties": {
            "columnId": { "type": "integer", "x-order": 0, "title": "Column Id",
                          "x-optionsAction": "ListColumns",
                          "x-optionValue": "id", "x-optionLabel": "name" }
        },
        "required": ["columnId"],
        "title": "MoveCard"
    })

    Component {
        id: choicelessForm
        DynamicForm { actionType: "CreateNote"; schema: testCase.plainSchema; controller: choicelessController }
    }

    Component {
        id: choiceForm
        DynamicForm { actionType: "MoveCard"; schema: testCase.choiceSchema; controller: choiceController }
    }

    Component {
        id: muteForm
        DynamicForm { actionType: "CreateNote"; schema: testCase.plainSchema; controller: mutePlainObject }
    }

    function test_a_choiceless_controller_loads_without_a_connections_warning() {
        failOnWarning(/no signal of the target matches the name/)
        var form = createTemporaryObject(choicelessForm, testCase)
        verify(form !== null)
        // The form is real, not an empty shell that would warn about nothing.
        compare(form.fields.length, 2)
    }

    function test_a_choice_serving_controller_still_receives_its_options() {
        failOnWarning(/no signal of the target matches the name/)
        var form = createTemporaryObject(choiceForm, testCase)
        verify(form !== null)
        compare(form.fieldByName["columnId"].isChoice, true)
        // Component.onCompleted fetched, and the reply landed through the
        // handler that now lives in its own Connections block.
        compare(form.fieldOptions["columnId"].length, 1)
        compare(form.fieldOptions["columnId"][0].label, "Bravo")
    }

    function test_an_absent_replyReceived_is_still_reported() {
        // The strict half. If the fix were `ignoreUnknownSignals` on the one
        // original block, no warning would be produced and ignoreWarning()
        // would fail this test for the missing warning.
        ignoreWarning(/Detected function "onReplyReceived" in Connections element/)
        failOnWarning(/Detected function "onOptionsReceived" in Connections element/)
        var form = createTemporaryObject(muteForm, testCase)
        verify(form !== null)
    }
}
