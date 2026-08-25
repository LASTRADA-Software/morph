// SPDX-License-Identifier: Apache-2.0
//
// `x-rules` is evaluated twice — once compiled (`morph::forms::allRulesSatisfied`)
// and once in JavaScript here — and nothing pinned the two to each other
// (morph#176). These cases drive the *verbatim* `schemaJson<A>()` output of a
// real action through the renderer and assert the verdict the compiled
// evaluator reaches for the same field state.
//
// The schema below is copied byte-for-byte from `schemaJson<RuleAgreementAction>()`
// (see tests/test_forms_rule_agreement.cpp, which pins the compiled side against
// the same action and the same field states). It is fed as *text* and parsed
// here, exactly as a shipped app does via
// `JSON.parse(controller.schemasJson)` — parsing is what rounds an int64
// literal, so a fixture built from a QML object literal could not reproduce
// divergence (b) at all.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormRuleAgreement"
    visible: true

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)
        property int submitCount: 0
        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            replyReceived(actionType, true, JSON.stringify({ok: true}))
        }
        function fetchOptions(optionsAction) { optionsReceived(optionsAction, true, "[]") }
    }

    // Verbatim schemaJson<RuleAgreementAction>() output, as text.
    readonly property string schemaText: '{"type":"object","properties":{"flag":{"type":["boolean","null"],"x-order":0,"title":"Flag"},"id":{"anyOf":[{"$ref":"#/$defs/int64_t"},{"type":"null"}],"x-order":1,"title":"Id"},"reason":{"type":["string","null"],"x-order":2,"title":"Reason"}},"additionalProperties":false,"$defs":{"int64_t":{"type":"integer","minimum":-9223372036854775808,"maximum":9223372036854775807,"x-exactMinimum":"-9223372036854775808","x-exactMaximum":"9223372036854775807"}},"title":"RuleAct","required":[],"x-rules":[{"kind":"requiredWhen","fields":["reason"],"when":{"kind":"equals","fields":["flag"],"value":true}},{"kind":"requiredWhen","fields":["reason"],"when":{"kind":"equals","fields":["id"],"value":9007199254740993,"valueText":"9007199254740993"}}]}'
    readonly property var parsedSchema: JSON.parse(schemaText)

    Component {
        id: form
        DynamicForm {
            actionType: "Test_RuleAgreement"
            schema: testCase.parsedSchema
            controller: mockController
        }
    }

    // ── (a) equals against a bool literal ────────────────────────────────────

    function test_equals_bool_literal_fires_requiredWhen_as_the_compiled_side_does() {
        var f = createTemporaryObject(form, testCase)
        var flag = findChild(f, "field_flag")
        verify(flag !== null)
        flag.checked = true
        flag.toggled()
        // Compiled: flag == true, so `reason` is required and unset -> not satisfied.
        // Before morph#176 the client compared "true" === true and never fired,
        // so it reported ready and submitted a body the server rejects.
        compare(f.ready, false)

        findChild(f, "field_reason").text = "because"
        compare(f.ready, true)
    }

    function test_equals_bool_literal_does_not_fire_when_false() {
        var f = createTemporaryObject(form, testCase)
        var flag = findChild(f, "field_flag")
        flag.checked = false
        flag.toggled()
        // Condition does not hold, so `reason` stays optional.
        compare(f.ready, true)
    }

    // ── (b) equals against an int64 literal beyond 2^53 ──────────────────────

    function test_equals_int64_literal_does_not_fire_one_below_the_literal() {
        var f = createTemporaryObject(form, testCase)
        // 9007199254740992 is one below the literal 9007199254740993. The
        // compiled evaluator says the condition is false, so `reason` is not
        // required. JSON.parse collapses both to the same double, so before
        // morph#176 the client believed the condition held and blocked a
        // submission the server would have accepted.
        findChild(f, "field_id").text = "9007199254740992"
        compare(f.ready, true)
    }

    function test_equals_int64_literal_fires_on_the_exact_literal() {
        var f = createTemporaryObject(form, testCase)
        findChild(f, "field_id").text = "9007199254740993"
        compare(f.ready, false)          // reason now required, still unset
        findChild(f, "field_reason").text = "because"
        compare(f.ready, true)
    }
}
