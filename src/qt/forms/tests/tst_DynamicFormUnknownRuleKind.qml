// SPDX-License-Identifier: Apache-2.0
//
// The forward-compatibility half of morph#176: what this renderer does with an
// `x-rules` node whose `kind` it does not recognise.
//
// forms.md ("Renderer fallback" -> "'Cannot evaluate' means defer, not block")
// settles a sentence two shipped clients once read in opposite directions.
// "Cannot evaluate" is a *third* answer, distinct from both true and false,
// and it means **defer**: hand the payload to the server, which runs the
// compiled rule list and has no unrecognised-kind case at all. A blocking
// renderer would turn every additive extension of the rule vocabulary into a
// breaking change for every renderer already deployed -- the operator gets a
// form that can never be satisfied, with no error naming why.
//
// None of this is reachable through the shared corpus
// (data/rule_corpus.json), and that is not an oversight: every corpus schema
// is verbatim `schemaJson<A>()` output, and the compiled emitter cannot emit a
// kind it does not have. The schemas below are therefore hand-authored -- the
// one place in the rules tests where that is the honest fixture rather than a
// shortcut. They are written as raw text and parsed here, the same way a real
// app parses `controller.schemasJson`.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormUnknownRuleKind"
    visible: true
    width: 400
    height: 600

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)
        property int submitCount: 0
        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            replyReceived(actionType, true, '{"ok":true}')
        }
        function fetchOptions(optionsAction) { optionsReceived(optionsAction, true, "[]") }
    }

    // Bound into the component rather than passed as an initial property:
    // createTemporaryObject's initial-properties path converts the object
    // through QVariantMap, which leaves `Array.isArray(p.type)` false and makes
    // DynamicForm misread every field's declared type.
    property var pendingSchema: ({})

    Component {
        id: formComponent
        DynamicForm {
            actionType: "Test_UnknownRuleKind"
            controller: mockController
            schema: testCase.pendingSchema
        }
    }

    // Two plain string fields and one `x-rules` array, spelled by the caller.
    function twoFieldSchema(rulesJson) {
        return '{"type":"object","properties":'
             + '{"email":{"type":["string","null"],"x-order":0,"title":"Email"},'
             + '"note":{"type":["string","null"],"x-order":1,"title":"Note"}},'
             + '"additionalProperties":false,"title":"Unknown","required":[],'
             + '"x-rules":' + rulesJson + '}'
    }

    function formFor(rulesJson) {
        testCase.pendingSchema = JSON.parse(twoFieldSchema(rulesJson))
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)
        return form
    }

    // --- top-level rules -----------------------------------------------------

    function test_anUnrecognisedTopLevelRuleKindDoesNotBlockSubmission() {
        var f = formFor('[{"kind":"quantumEntangled","fields":["email","note"]}]')
        // The renderer cannot judge this rule, so it must not veto the server's
        // judgement of it.
        compare(f.ready, true)
        verify(mockController.submitCount > 0)
    }

    function test_anUnrecognisedKindInsideNotDoesNotBecomeTrue() {
        // `not` of "cannot evaluate" is "cannot evaluate", not `true`.
        // Collapsing the third answer into `false` made this rule fire and
        // demand `note` for a reason the renderer had just said it could not
        // judge -- blocking through the back door.
        var f = formFor('[{"kind":"requiredWhen","fields":["note"],'
                      + '"when":{"kind":"not","condition":{"kind":"quantumEntangled","fields":["email"]}}}]')
        compare(f.ready, true)
    }

    function test_anUnrecognisedDisjunctDoesNotBlockAnOtherwiseFalseOr() {
        // `or` over one false child and one unevaluable child is unevaluable,
        // not false: the unknown child might have been the true one.
        var f = formFor('[{"kind":"or","conditions":['
                      + '{"kind":"engaged","fields":["email"]},'
                      + '{"kind":"quantumEntangled","fields":["note"]}]}]')
        compare(f.ready, true)
    }

    function test_aFalseConjunctStillSettlesAnAndOutright() {
        // The other direction, and the reason "defer" is not "ignore": `and`
        // over a definitely-false child is definitely false however many
        // unevaluable siblings it has, so this one really does block.
        var f = formFor('[{"kind":"and","conditions":['
                      + '{"kind":"engaged","fields":["email"]},'
                      + '{"kind":"quantumEntangled","fields":["note"]}]}]')
        compare(f.ready, false)

        // ...and stops blocking the moment the knowable child becomes true,
        // which is what proves the `false` above came from that child rather
        // than from the unknown one.
        findChild(f, "field_email").text = "a@b"
        compare(f.ready, true)
    }

    function test_anUnrecognisedRequiredWhenConditionDoesNotRequireTheField() {
        var f = formFor('[{"kind":"requiredWhen","fields":["note"],'
                      + '"when":{"kind":"quantumEntangled","fields":["email"]}}]')
        compare(f.ready, true)
    }

    // --- presentation kinds --------------------------------------------------

    function test_anUnrecognisedVisibleWhenConditionLeavesTheFieldVisible() {
        // Hiding a field over a condition the renderer could not judge removes
        // the user's only way to fill in a form the server may well accept.
        var f = formFor('[{"kind":"visibleWhen","fields":["note"],'
                      + '"when":{"kind":"quantumEntangled","fields":["email"]}}]')
        var column = findChild(f, "column_note")
        verify(column !== null)
        compare(column.visible, true)
    }

    function test_anUnrecognisedReadonlyWhenConditionLeavesTheFieldEditable() {
        var f = formFor('[{"kind":"readonlyWhen","fields":["note"],'
                      + '"when":{"kind":"quantumEntangled","fields":["email"]}}]')
        var column = findChild(f, "column_note")
        verify(column !== null)
        compare(column.enabled, true)
    }

    // --- the recognised path still works -------------------------------------

    function test_aRecognisedRuleStillBlocks() {
        // The guard against "fixing" the above by never blocking on anything.
        var f = formFor('[{"kind":"exactlyOneOf","fields":["email","note"]}]')
        compare(f.ready, false)
        findChild(f, "field_email").text = "a@b"
        compare(f.ready, true)
    }
}
