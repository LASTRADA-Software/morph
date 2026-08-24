// SPDX-License-Identifier: Apache-2.0
//
// Covers the two field shapes DynamicForm used to encode with the wrong JSON
// type, producing payloads the server rejected (morph#189):
//
//   1. {"type": "boolean"} fell through to the plain TextField, which applied
//      no validation and emitted the typed text as a JSON *string* --
//      {"flag":"true"}, or {"flag":"banana"} for anything at all.
//   2. A nullable member whose type is a $ref emits
//      {"anyOf":[{"$ref":...},{"type":"null"}]} with no top-level "type" key,
//      so every kind flag was false and the integer went out quoted.
//
// glaze coerces neither: the first is expected_true_or_false, the second
// parse_number_failure. tests/test_forms_boolean_anyof_wire.cpp pins that
// acceptance/rejection on the C++ side against the same literals asserted here.
//
// Integers are checked against the raw previewLine *text*, never JSON.parse'd:
// parsing would round at 2^53 and hide the very exactness being asserted.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormBooleanAndAnyOf"
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
            replyReceived(actionType, true, JSON.stringify({ok: true}))
        }

        function fetchOptions(optionsAction) {
            optionsReceived(optionsAction, true, "[]")
        }
    }

    // `flag` is required; `note` keeps the form from being trivially empty.
    property var boolSchema: ({
        properties: {
            note: { type: "string", "x-order": 0 },
            flag: { type: "boolean", "x-order": 1 }
        },
        required: ["note", "flag"]
    })

    // A nullable boolean, as {"type": ["boolean", "null"]}.
    property var nullableBoolSchema: ({
        properties: {
            flag: { type: ["boolean", "null"], "x-order": 0 }
        },
        required: []
    })

    // The anyOf shape glaze emits for a bare std::optional<std::int64_t>:
    // a $ref to the integer definition, plus a null branch, and no top-level
    // "type". `refI64` is the top-level-$ref shape that already worked and
    // must keep working.
    property var anyOfSchema: ({
        "$defs": {
            "int64_t": { type: "integer" },
            "TagId": { type: "integer" }
        },
        properties: {
            optI64: { anyOf: [{ "$ref": "#/$defs/int64_t" }, { type: "null" }], "x-order": 0 },
            refI64: { "$ref": "#/$defs/TagId", "x-order": 1 }
        },
        required: []
    })

    Component {
        id: boolForm
        DynamicForm { actionType: "T_Bool"; schema: testCase.boolSchema; controller: mockController }
    }

    Component {
        id: nullableBoolForm
        DynamicForm { actionType: "T_NullableBool"; schema: testCase.nullableBoolSchema; controller: mockController }
    }

    Component {
        id: anyOfForm
        DynamicForm { actionType: "T_AnyOf"; schema: testCase.anyOfSchema; controller: mockController }
    }

    // ── boolean: control ─────────────────────────────────────────────────────

    function test_boolean_field_renders_a_checkbox_not_a_text_field() {
        var form = createTemporaryObject(boolForm, testCase)
        verify(form !== null)
        var control = findChild(form, "field_flag")
        verify(control !== null)
        // A CheckBox has `checked`; the plain TextField has `text` instead.
        // This is what stops "banana" from ever being typed into a boolean.
        // A CheckBox has `checked`; the plain TextField does not. (A CheckBox
        // does have a `text` label property, so its absence is not the tell.)
        verify(control.checked !== undefined)
        verify(control.placeholderText === undefined)
    }

    // ── boolean: wire type ───────────────────────────────────────────────────

    function test_checked_boolean_serialises_as_a_bare_true() {
        var form = createTemporaryObject(boolForm, testCase)
        findChild(form, "field_note").text = "n"
        var box = findChild(form, "field_flag")
        box.checked = true
        box.toggled()
        compare(form.ready, true)
        // The defect was a quoted string. Assert on the raw text so a
        // regression to {"flag":"true"} cannot pass by JSON.parse coercion.
        verify(form.previewLine.indexOf('"flag":true') !== -1)
        verify(form.previewLine.indexOf('"flag":"true"') === -1)
        compare(typeof JSON.parse(form.previewLine).flag, "boolean")
    }

    function test_unchecked_required_boolean_is_valid_and_serialises_as_a_bare_false() {
        var form = createTemporaryObject(boolForm, testCase)
        findChild(form, "field_note").text = "n"
        // Never touched: a checkbox always shows a definite state, so a
        // required one seeds "false" rather than blocking `ready` invisibly.
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"flag":false') !== -1)
        verify(form.previewLine.indexOf('"flag":"false"') === -1)
        compare(typeof JSON.parse(form.previewLine).flag, "boolean")
    }

    function test_nullable_boolean_serialises_the_same_way() {
        var form = createTemporaryObject(nullableBoolForm, testCase)
        var box = findChild(form, "field_flag")
        verify(box !== null)
        verify(box.checked !== undefined)
        box.checked = true
        box.toggled()
        verify(form.previewLine.indexOf('"flag":true') !== -1)
        verify(form.previewLine.indexOf('"flag":"true"') === -1)
    }

    function test_untouched_optional_boolean_is_omitted_rather_than_sent_as_false() {
        var form = createTemporaryObject(nullableBoolForm, testCase)
        // Distinguishes "not answered" from an explicit false, which is the
        // whole point of a std::optional<bool> member.
        compare(form.previewLine.indexOf("flag"), -1)
    }

    // ── anyOf integers ───────────────────────────────────────────────────────

    function test_anyOf_integer_serialises_as_a_bare_exact_number() {
        var form = createTemporaryObject(anyOfForm, testCase)
        // 2^53 + 1 -- not representable as a double, so a round-trip through
        // JSON.parse would corrupt it. The payload is assembled as text.
        findChild(form, "field_optI64").text = "9007199254740993"
        verify(form.previewLine.indexOf('"optI64":9007199254740993') !== -1)
        verify(form.previewLine.indexOf('"optI64":"9007199254740993"') === -1)
    }

    function test_anyOf_integer_survives_int64_max_exactly() {
        var form = createTemporaryObject(anyOfForm, testCase)
        findChild(form, "field_optI64").text = "9223372036854775807"
        verify(form.previewLine.indexOf('"optI64":9223372036854775807') !== -1)
    }

    function test_anyOf_integer_field_rejects_non_numeric_text() {
        var form = createTemporaryObject(anyOfForm, testCase)
        // Before the fix this was accepted and shipped as {"optI64":"banana"}:
        // resolving the anyOf is what makes the integer validation apply.
        findChild(form, "field_optI64").text = "banana"
        compare(form.ready, false)
    }

    function test_top_level_ref_integer_still_serialises_exactly() {
        var form = createTemporaryObject(anyOfForm, testCase)
        // Regression guard: this path already worked (bookmarks::RenameTag.id)
        // and the anyOf change must not disturb it.
        findChild(form, "field_refI64").text = "18446744073709551615"
        verify(form.previewLine.indexOf('"refI64":18446744073709551615') !== -1)
        verify(form.previewLine.indexOf('"refI64":"18446744073709551615"') === -1)
    }
}
