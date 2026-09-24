// SPDX-License-Identifier: Apache-2.0
//
// A plain `"number"` member -- a bare `double` or `float`, with no
// `x-decimalPlaces` and no `Quantity` wrapper -- is submitted as a JSON
// *number*.
//
// Without an encoding branch of its own such a member reaches
// `fieldJsonLiteral`'s generic fall-through, `JSON.stringify(text)`, and the
// body carries `"3.5"` where the schema asks for `3.5`. The schema side and
// the encoding side each work; nothing spanned them for this shape, which is
// why every case below asserts the *submitted body*, not merely that the form
// reports ready.
//
// The fixtures are the shapes `schemaJson<A>()` really emits, copied from its
// output rather than idealised: a `double` member is a `$ref` into
// `$defs/double`, whose node carries `type: "number"` **and** the type's own
// `minimum`/`maximum` (±DBL_MAX; ±3.4e38 for a `float`). Those bounds are why
// a plain-number field needs a gate as well as an encoder: a value a `float`
// cannot hold is a value the schema declares out of range.
//
// The last two cases are the neighbouring number-ish shapes, here as
// regression guards rather than as new coverage: changing how a plain number
// encodes is exactly the change that could alter them.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormPlainNumber"
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

    // `struct { double ratio; float score; }`, exactly as schemaJson emits it.
    property var plainSchema: ({
        "$defs": {
            "double": { type: "number", minimum: -1.7976931348623157e+308, maximum: 1.7976931348623157e+308 },
            "float": { type: "number", minimum: -3.4028234663852886e+38, maximum: 3.4028234663852886e+38 }
        },
        properties: {
            ratio: { "$ref": "#/$defs/double", "x-order": 0, title: "Ratio" },
            score: { "$ref": "#/$defs/float", "x-order": 1, title: "Score" }
        },
        required: ["ratio"]
    })

    // The same member with declared bounds on it (what FieldMeta's
    // minimum/maximum/multipleOf stamp onto the property node).
    property var boundedSchema: ({
        properties: {
            temperature: { type: "number", minimum: -40.5, maximum: 120, multipleOf: 0.5, "x-order": 0 }
        },
        required: ["temperature"]
    })

    // A Quantity member as schemaJson emits it: an object of num/den/dp with
    // `x-decimalPlaces` and `ExtUnits` beside it -- its JSON type is
    // "object"/"null", not "number". The exact {num,den,dp} encoding applies
    // and must keep applying.
    property var quantitySchema: ({
        properties: {
            mass: {
                type: ["object", "null"],
                properties: { num: { type: "integer" }, den: { type: "integer" }, dp: { type: "integer" } },
                additionalProperties: false,
                ExtUnits: { unitAscii: "kg", unitUnicode: "kg" },
                "x-decimalPlaces": 2, "x-order": 0, title: "Mass"
            }
        },
        required: ["mass"]
    })

    // A declared precision on a property whose JSON type *is* "number" -- the
    // spelling a decorated schema produces. Both kind flags fire, and the
    // order in which fieldJsonLiteral asks decides: precision wins, so the
    // field encodes as an exact rational rather than as a bare number.
    property var precisionNumberSchema: ({
        properties: { reading: { type: "number", "x-decimalPlaces": 1, "x-order": 0 } },
        required: ["reading"]
    })

    // A bare `math::Rational` member, as schemaJson emits it: an inline
    // object of num/den/dp. The renderer draws one text field over it and has
    // no encoder for the shape, so it is reported *unrepresentable* and the
    // form stays unready. What the case below pins is the part that must hold
    // under any future treatment of the shape: whatever is typed, the body
    // never carries a JSON string for it.
    property var rationalSchema: ({
        "$defs": {
            "int64_t": { type: "integer", minimum: -9223372036854775808, maximum: 9223372036854775807 },
            "uint32_t": { type: "integer", minimum: 0, maximum: 4294967295 }
        },
        properties: {
            limit: {
                type: "object",
                properties: {
                    num: { "$ref": "#/$defs/int64_t" },
                    den: { "$ref": "#/$defs/int64_t" },
                    dp: { "$ref": "#/$defs/uint32_t" }
                },
                additionalProperties: false,
                "x-order": 0, title: "Limit"
            }
        },
        required: ["limit"]
    })

    // The same bare-Rational member with `x-decimalPlaces` on its property
    // node (what a decorated instance schema stamps there). That hands the
    // member the exact-decimal control, whose {num,den,dp} literal is exactly
    // the shape this schema asks for -- so the shape is encodable, and it is
    // the missing precision, not the object type, that leaves the undecorated
    // one unrepresentable.
    property var decoratedRationalSchema: ({
        properties: {
            limit: {
                type: "object",
                properties: { num: { type: "integer" }, den: { type: "integer" }, dp: { type: "integer" } },
                "x-decimalPlaces": 2, "x-order": 0, title: "Limit"
            }
        },
        required: ["limit"]
    })

    Component {
        id: plainForm
        DynamicForm { actionType: "T_Plain"; schema: testCase.plainSchema; controller: mockController }
    }

    Component {
        id: boundedForm
        DynamicForm { actionType: "T_Bounded"; schema: testCase.boundedSchema; controller: mockController }
    }

    Component {
        id: quantityForm
        DynamicForm { actionType: "T_Quantity"; schema: testCase.quantitySchema; controller: mockController }
    }

    Component {
        id: rationalForm
        DynamicForm { actionType: "T_Rational"; schema: testCase.rationalSchema; controller: mockController }
    }

    Component {
        id: precisionNumberForm
        DynamicForm {
            actionType: "T_PrecisionNumber"
            schema: testCase.precisionNumberSchema
            controller: mockController
        }
    }

    Component {
        id: decoratedRationalForm
        DynamicForm {
            actionType: "T_DecoratedRational"
            schema: testCase.decoratedRationalSchema
            controller: mockController
        }
    }

    function typeInto(form, field, text) {
        findChild(form, field).text = text
    }

    // ── the defect ───────────────────────────────────────────────────────────

    function test_a_decimal_is_submitted_as_a_json_number() {
        var form = createTemporaryObject(plainForm, testCase)
        typeInto(form, "field_ratio", "3.5")
        compare(form.ready, true)
        // The whole point: a number, not the quoted string the generic
        // fall-through produced.
        verify(form.previewLine.indexOf('"ratio":3.5') !== -1)
        verify(form.previewLine.indexOf('"ratio":"') === -1)
    }

    function test_a_whole_number_typed_into_a_number_field_stays_a_number() {
        var form = createTemporaryObject(plainForm, testCase)
        typeInto(form, "field_ratio", "4")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"ratio":4') !== -1)
        verify(form.previewLine.indexOf('"ratio":"') === -1)
    }

    function test_a_float_member_encodes_the_same_way() {
        var form = createTemporaryObject(plainForm, testCase)
        typeInto(form, "field_ratio", "1")
        typeInto(form, "field_score", "1.25")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"score":1.25') !== -1)
        verify(form.previewLine.indexOf('"score":"') === -1)
    }

    function test_zero_and_negative_values_carry_their_sign() {
        var form = createTemporaryObject(plainForm, testCase)
        typeInto(form, "field_ratio", "0")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"ratio":0') !== -1)
        typeInto(form, "field_ratio", "-0.25")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"ratio":-0.25') !== -1)
    }

    // ── the wire grammar ─────────────────────────────────────────────────────

    function test_leading_zeros_are_stripped_because_json_forbids_them() {
        var form = createTemporaryObject(plainForm, testCase)
        typeInto(form, "field_ratio", "007.50")
        compare(form.ready, true)
        // Trailing zeros are kept: "7.50" is a well-formed JSON number and
        // re-spelling the fraction is not this encoder's business.
        verify(form.previewLine.indexOf('"ratio":7.50') !== -1)
    }

    function test_text_that_is_not_a_json_number_is_refused() {
        var form = createTemporaryObject(plainForm, testCase)
        // Each of these would have been submitted as a quoted string, and the
        // form reported ready for it.
        var malformed = ["abc", "3.", ".5", "1e5", "--1", "1 2", "0x10", "3,5"]
        for (var i = 0; i < malformed.length; ++i) {
            typeInto(form, "field_ratio", malformed[i])
            compare(form.ready, false, "accepted " + malformed[i])
            compare(form.previewLine, "")
        }
    }

    function test_a_grouped_entry_is_read_the_way_a_quantity_entry_is() {
        var form = createTemporaryObject(plainForm, testCase)
        // The typed text is locale-normalised before it is encoded, exactly as
        // a Quantity's is -- a decimal field is typed with the locale's
        // separators, and the grouping is not part of the JSON number.
        typeInto(form, "field_ratio", "1,000.5")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"ratio":1000.5') !== -1)
    }

    // ── the gate ─────────────────────────────────────────────────────────────

    function test_a_value_the_member_type_cannot_hold_is_refused() {
        var form = createTemporaryObject(plainForm, testCase)
        typeInto(form, "field_ratio", "1")
        // 1e41, past a float's maximum -- which is the bound the schema's own
        // $defs/float carries, not a hand-written one.
        typeInto(form, "field_score", "100000000000000000000000000000000000000000")
        compare(form.ready, false)
        // The same magnitude in the double field is inside its range.
        typeInto(form, "field_score", "")
        typeInto(form, "field_ratio", "100000000000000000000000000000000000000000")
        compare(form.ready, true)
    }

    function test_declared_bounds_gate_a_plain_number() {
        var form = createTemporaryObject(boundedForm, testCase)
        typeInto(form, "field_temperature", "-40.5")
        compare(form.ready, true)
        typeInto(form, "field_temperature", "-41")
        compare(form.ready, false)
        typeInto(form, "field_temperature", "120")
        compare(form.ready, true)
        typeInto(form, "field_temperature", "120.5")
        compare(form.ready, false)
        // multipleOf 0.5: 0.25 is not a multiple, 0.5 is.
        typeInto(form, "field_temperature", "0.25")
        compare(form.ready, false)
        typeInto(form, "field_temperature", "0.5")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"temperature":0.5') !== -1)
    }

    function test_a_blank_optional_number_is_omitted_not_empty_stringed() {
        var form = createTemporaryObject(plainForm, testCase)
        typeInto(form, "field_ratio", "2")
        compare(form.ready, true)
        verify(form.previewLine.indexOf("score") === -1)
    }

    // ── the neighbouring number-ish shapes, unchanged ────────────────────────

    function test_a_quantity_member_still_encodes_as_an_exact_rational() {
        var form = createTemporaryObject(quantityForm, testCase)
        typeInto(form, "field_mass", "2.50")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"mass":{"num":250,"den":100,"dp":2}') !== -1)
    }

    function test_a_declared_precision_beats_the_plain_number_encoding() {
        var form = createTemporaryObject(precisionNumberForm, testCase)
        typeInto(form, "field_reading", "3.5")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"reading":{"num":35,"den":10,"dp":1}') !== -1)
    }

    function test_a_bare_rational_member_is_never_submitted_as_a_json_string() {
        var form = createTemporaryObject(rationalForm, testCase)
        typeInto(form, "field_limit", "3.5")
        // Either the renderer gains an encoder for the {num,den,dp} shape, or
        // it keeps declining to encode it -- but the one outcome that is wrong
        // either way is a body carrying a string for it.
        verify(form.previewLine.indexOf('"limit":"') === -1)
        verify(!form.ready || form.previewLine.indexOf('"limit":{') !== -1)
    }

    function test_a_declared_precision_makes_the_rational_shape_encodable() {
        var form = createTemporaryObject(decoratedRationalForm, testCase)
        typeInto(form, "field_limit", "3.5")
        compare(form.ready, true)
        // 350/100 is the same value as 7/2; the wire codec canonicalises it.
        verify(form.previewLine.indexOf('"limit":{"num":350,"den":100,"dp":2}') !== -1)
    }
}
