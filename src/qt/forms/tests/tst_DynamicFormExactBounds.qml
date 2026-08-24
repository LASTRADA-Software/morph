// SPDX-License-Identifier: Apache-2.0
//
// The client-side integer bounds gate at INT64 extremes (morph#213).
//
// `minimum`/`maximum` reach this renderer through
// `JSON.parse(controller.schemasJson)`, which every shipped app does, so an
// int64 bound is an IEEE-754 double by the time DynamicForm sees it and
// INT64_MAX arrives already rounded up to 9223372036854775808. Comparing
// INT64_MAX + 1 against that judges it "not greater" and passes it through the
// gate the schema meant to close.
//
// `schemaJson<A>()` therefore also emits `x-exactMinimum`/`x-exactMaximum` --
// exact decimal strings, which JSON.parse cannot round -- and the gate prefers
// them. The schemas below carry both keys exactly as a generated schema does;
// tests/test_forms_exact_bounds.cpp pins that the C++ side really emits them.
//
// Every fixture here sits AT the boundary on purpose: a schema with small
// bounds passes whether or not this bug exists.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormExactBounds"
    visible: true

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)
        function submitIfValid(actionType, bodyJson) {
            replyReceived(actionType, true, JSON.stringify({ok: true}))
        }
        function fetchOptions(optionsAction) { optionsReceived(optionsAction, true, "[]") }
    }

    // Note the deliberately *rounded* numeric bounds: these are what a real
    // JSON.parse produces for INT64_MIN/INT64_MAX, so the fixture reproduces
    // the renderer's actual input rather than an idealised one.
    property var i64Schema: ({
        "$defs": {
            "int64_t": {
                type: "integer",
                minimum: -9223372036854775808,
                maximum: 9223372036854775807,
                "x-exactMinimum": "-9223372036854775808",
                "x-exactMaximum": "9223372036854775807"
            }
        },
        properties: { id: { "$ref": "#/$defs/int64_t", "x-order": 0 } },
        required: ["id"]
    })

    // The same field with no exact companions: the pre-#213 shape, kept so the
    // numeric fallback path stays covered.
    property var smallBoundSchema: ({
        properties: { n: { type: "integer", minimum: -10, maximum: 10, "x-order": 0 } },
        required: ["n"]
    })

    // The anyOf shape: a bare std::optional<std::int64_t>. Before morph#189 this
    // had no resolved type at all, so no bounds applied and the gate never ran.
    // Now that resolveProp follows the non-null anyOf branch, the field inherits
    // $defs/int64_t's bounds -- including the exact companions (morph#213).
    property var anyOfI64Schema: ({
        "$defs": {
            "int64_t": {
                type: "integer",
                minimum: -9223372036854775808,
                maximum: 9223372036854775807,
                "x-exactMinimum": "-9223372036854775808",
                "x-exactMaximum": "9223372036854775807"
            }
        },
        properties: { optId: { anyOf: [{ "$ref": "#/$defs/int64_t" }, { type: "null" }], "x-order": 0 } },
        required: []
    })

    Component {
        id: anyOfI64Form
        DynamicForm { actionType: "T_AnyOfI64"; schema: testCase.anyOfI64Schema; controller: mockController }
    }

    Component {
        id: i64Form
        DynamicForm { actionType: "T_I64"; schema: testCase.i64Schema; controller: mockController }
    }

    Component {
        id: smallForm
        DynamicForm { actionType: "T_Small"; schema: testCase.smallBoundSchema; controller: mockController }
    }

    function typeInto(form, field, text) {
        findChild(form, field).text = text
    }

    // ── at the boundary: legal values stay legal ─────────────────────────────

    function test_int64_max_is_accepted_and_stays_exact() {
        var form = createTemporaryObject(i64Form, testCase)
        typeInto(form, "field_id", "9223372036854775807")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"id":9223372036854775807') !== -1)
    }

    function test_int64_min_is_accepted_and_stays_exact() {
        var form = createTemporaryObject(i64Form, testCase)
        typeInto(form, "field_id", "-9223372036854775808")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"id":-9223372036854775808') !== -1)
    }

    // ── one past the boundary: the gate must close ───────────────────────────

    function test_int64_max_plus_one_is_rejected() {
        var form = createTemporaryObject(i64Form, testCase)
        // The whole point. Against the rounded double maximum this compares
        // equal, not greater, and was admitted -- then rejected by the server
        // with parse_number_failure.
        typeInto(form, "field_id", "9223372036854775808")
        compare(form.ready, false)
    }

    function test_int64_min_minus_one_is_rejected() {
        var form = createTemporaryObject(i64Form, testCase)
        typeInto(form, "field_id", "-9223372036854775809")
        compare(form.ready, false)
    }

    function test_a_value_far_above_the_maximum_is_rejected() {
        var form = createTemporaryObject(i64Form, testCase)
        typeInto(form, "field_id", "99999999999999999999")
        compare(form.ready, false)
    }

    // ── the digit comparison itself ──────────────────────────────────────────

    function test_leading_zeros_do_not_defeat_the_comparison() {
        var form = createTemporaryObject(i64Form, testCase)
        // Same magnitude as INT64_MAX, written with padding: still legal.
        typeInto(form, "field_id", "0009223372036854775807")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"id":9223372036854775807') !== -1)
    }

    function test_ordinary_midrange_values_are_unaffected() {
        var form = createTemporaryObject(i64Form, testCase)
        typeInto(form, "field_id", "42")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"id":42') !== -1)
        typeInto(form, "field_id", "-42")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"id":-42') !== -1)
    }

    // ── the numeric fallback still works where no exact bound is emitted ─────

    function test_small_numeric_bounds_still_gate_without_exact_companions() {
        var form = createTemporaryObject(smallForm, testCase)
        typeInto(form, "field_n", "10")
        compare(form.ready, true)
        typeInto(form, "field_n", "11")
        compare(form.ready, false)
        typeInto(form, "field_n", "-10")
        compare(form.ready, true)
        typeInto(form, "field_n", "-11")
        compare(form.ready, false)
    }

    // ── the two fixes composing ──────────────────────────────────────────────

    function test_anyOf_field_inherits_the_exact_bounds_and_rejects_past_them() {
        var form = createTemporaryObject(anyOfI64Form, testCase)
        // Measured on morph#189's branch before this fix: an anyOf int64 field
        // admitted INT64_MAX + 1, because the bound it compared against had been
        // rounded up by JSON.parse to exactly that value.
        typeInto(form, "field_optId", "9223372036854775808")
        compare(form.ready, false)
    }

    function test_anyOf_field_still_accepts_int64_max_exactly() {
        var form = createTemporaryObject(anyOfI64Form, testCase)
        typeInto(form, "field_optId", "9223372036854775807")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"optId":9223372036854775807') !== -1)
    }
}
