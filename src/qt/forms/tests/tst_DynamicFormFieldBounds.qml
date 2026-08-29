// SPDX-License-Identifier: Apache-2.0
//
// The renderer half of the per-field scalar bounds (morph#310).
//
// `FieldMeta::minimum`/`::maximum`/`::multipleOf` let an action declare a
// bound the `formRules` vocabulary cannot express -- every comparison node
// there takes two member pointers, never a literal -- and `schemaJson<A>()`
// serves them as the standard JSON-Schema keys of the same names.
//
// Two things had to change here for that to be worth anything:
//
// 1. **The bound sits on the property node, not in the `$def`.** It has to:
//    `budget` and `tally` below share one `$defs/q`, so a bound written there
//    would leak onto every field of that type -- which is exactly the
//    per-*unit* behaviour `UnitTraits::bounds` already has and that this
//    feature exists to avoid. The renderer read `minimum`/`maximum` only from
//    the resolved `$ref`, so it saw nothing; it now reads the property node
//    first, the way it already does for every other per-field key.
// 2. **`multipleOf` had no gate at all.** It is the JSON-Schema spelling of
//    "whole number" -- the constraint `Quantity` cannot carry in its type,
//    since `Quantity<U, Dec>` requires `Dec >= 1`.
//
// tests/test_forms_field_bounds.cpp pins that the C++ side really emits these
// keys, on the property node, for exactly the fields that declare them.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormFieldBounds"
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

    // The pastebin shape: `burnAfterReads` is whole and at least 1; `readCount`
    // is the same Quantity type over the same unit and starts at 0. One `$def`,
    // two different sets of bounds -- the case a per-unit bound cannot express.
    property var burnSchema: ({
        "$defs": {
            q: { type: "object", ExtUnits: { unitAscii: "count", unitUnicode: "" } }
        },
        properties: {
            budget: { "$ref": "#/$defs/q", "x-order": 0, "x-decimalPlaces": 1,
                      title: "Budget", minimum: 1, multipleOf: 1 },
            tally: { "$ref": "#/$defs/q", "x-order": 1, "x-decimalPlaces": 1, title: "Tally" }
        },
        required: []
    })

    // A closed range plus a fractional step, on a plain integer and a quantity.
    property var rangeSchema: ({
        properties: {
            retries: { type: "integer", "x-order": 0, minimum: 0, maximum: 5 },
            step: { type: "object", "x-order": 1, "x-decimalPlaces": 2, multipleOf: 0.5,
                    ExtUnits: { unitAscii: "u", unitUnicode: "" } }
        },
        required: []
    })

    Component {
        id: burnForm
        DynamicForm { actionType: "T_Burn"; schema: testCase.burnSchema; controller: mockController }
    }

    Component {
        id: rangeForm
        DynamicForm { actionType: "T_Range"; schema: testCase.rangeSchema; controller: mockController }
    }

    function typeInto(form, field, text) {
        findChild(form, field).text = text
    }

    // ── the declared minimum gates the field it was declared on ──────────────

    function test_a_quantity_below_its_declared_minimum_is_rejected() {
        var form = createTemporaryObject(burnForm, testCase)
        typeInto(form, "field_budget", "0")
        compare(form.ready, false)
        typeInto(form, "field_budget", "-1")
        compare(form.ready, false)
    }

    function test_a_quantity_at_its_declared_minimum_is_accepted() {
        var form = createTemporaryObject(burnForm, testCase)
        typeInto(form, "field_budget", "1")
        compare(form.ready, true)
    }

    // ── multipleOf: the whole-number gate that had no vocabulary ─────────────

    function test_a_fractional_quantity_fails_multipleOf_one() {
        var form = createTemporaryObject(burnForm, testCase)
        typeInto(form, "field_budget", "2.5")
        compare(form.ready, false)
    }

    function test_the_neighbouring_whole_numbers_are_accepted() {
        var form = createTemporaryObject(burnForm, testCase)
        typeInto(form, "field_budget", "2")
        compare(form.ready, true)
        typeInto(form, "field_budget", "3")
        compare(form.ready, true)
    }

    // ── the sibling sharing the same $def is untouched ───────────────────────

    function test_a_sibling_of_the_same_type_keeps_its_own_freedom() {
        var form = createTemporaryObject(burnForm, testCase)
        typeInto(form, "field_budget", "1")
        // 0 and a half-read are both fine for `tally`: nothing was declared for
        // it, and the bound must not have leaked through the shared `$def`.
        typeInto(form, "field_tally", "0")
        compare(form.ready, true)
        typeInto(form, "field_tally", "0.5")
        compare(form.ready, true)
    }

    // ── a closed range on a plain integer field ──────────────────────────────

    function test_an_integer_range_gates_both_ends() {
        var form = createTemporaryObject(rangeForm, testCase)
        typeInto(form, "field_retries", "0")
        compare(form.ready, true)
        typeInto(form, "field_retries", "5")
        compare(form.ready, true)
        typeInto(form, "field_retries", "6")
        compare(form.ready, false)
        typeInto(form, "field_retries", "-1")
        compare(form.ready, false)
    }

    // ── a fractional step ────────────────────────────────────────────────────

    function test_a_fractional_multipleOf_admits_only_its_multiples() {
        var form = createTemporaryObject(rangeForm, testCase)
        typeInto(form, "field_retries", "1")
        typeInto(form, "field_step", "1.5")
        compare(form.ready, true)
        typeInto(form, "field_step", "0.75")
        compare(form.ready, false)
    }
}
