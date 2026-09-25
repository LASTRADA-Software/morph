// SPDX-License-Identifier: Apache-2.0
//
// Loading a stored payload into DynamicForm for editing: prefill(values) /
// prefillFromJson(text), the inverse of the form's own encoders.
//
// The claim is a round trip: prefilling a form from a payload and changing
// nothing must assemble the same payload again. Every case below therefore
// asserts previewLine against the JSON it was fed (in the canonical spelling
// the encoders produce), plus the half a user sees -- the drawn control, or a
// slot's fieldText/rows, holding the value -- and that prefilling never
// submits.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormPrefill"
    visible: true
    width: 600
    height: 900

    QtObject {
        id: recordingController
        property int submissions: 0
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)
        function submitIfValid(actionType, bodyJson) { submissions++ }
        function fetchOptions(optionsAction, bodyJson) {
            optionsReceived(optionsAction, true, '[{"id":9007199254740993,"name":"Big"},{"id":2,"name":"Two"}]')
        }
    }

    // One member of every kind the section DTOs use, in schemaJson<A>() shape.
    property var sampleSchema: ({
        "$defs": {
            "double": { type: "number" },
            "int64_t": { type: "integer" },
            "Row": {
                type: "object",
                properties: {
                    sieve: { "$ref": "#/$defs/double", "x-order": 0 },
                    passing: {
                        type: ["object", "null"],
                        properties: { num: { type: "integer" }, den: { type: "integer" }, dp: { type: "integer" } },
                        "x-decimalPlaces": 1, "x-order": 1
                    }
                },
                required: ["sieve", "passing"]
            }
        },
        properties: {
            id: { "$ref": "#/$defs/int64_t", "x-order": 0 },
            density: {
                type: ["object", "null"],
                properties: { num: { type: "integer" }, den: { type: "integer" }, dp: { type: "integer" } },
                ExtUnits: { unitAscii: "kg_per_m3", unitUnicode: "kg/m³" }, "x-decimalPlaces": 2, "x-order": 1,
                "x-unitAlternatives": [{ id: "g_per_cm3", display: "g/cm³", decimals: 5, num: 1000, den: 1 }]
            },
            temperature: { "$ref": "#/$defs/double", "x-order": 2 },
            takenAt: { type: "string", format: "date-time", "x-order": 3 },
            done: { type: "boolean", "x-order": 4 },
            role: { type: "string", oneOf: [{ title: "A", const: "A" }, { title: "B", const: "B" }], "x-order": 5 },
            note: { type: ["string", "null"], "x-order": 6 },
            tags: { type: "array", items: { type: "string" }, "x-order": 7 },
            rows: { type: "array", items: { "$ref": "#/$defs/Row" }, "x-order": 8 }
        },
        required: ["id", "density", "temperature", "takenAt", "done", "role", "rows"]
    })

    readonly property string storedPayload:
        '{"id":9007199254740993,"density":{"num":245050,"den":100,"dp":2},"temperature":21.5,'
        + '"takenAt":"2026-07-20T09:00:00Z","done":true,"role":"B","note":"retest","tags":["a","b"],'
        + '"rows":[{"sieve":31.5,"passing":{"num":1000,"den":10,"dp":1}},{"sieve":0.1,"passing":{"num":42,"den":10,"dp":1}}]}'

    // The payload in the spelling the encoders produce -- here, the same one.
    readonly property string canonicalPayload:
        '{"id":9007199254740993,"density":{"num":245050,"den":100,"dp":2},"temperature":21.5,'
        + '"takenAt":"2026-07-20T09:00:00Z","done":true,"role":"B","note":"retest","tags":["a","b"],'
        + '"rows":[{"sieve":31.5,"passing":{"num":1000,"den":10,"dp":1}},{"sieve":0.1,"passing":{"num":42,"den":10,"dp":1}}]}'

    property var choiceSchema: ({
        properties: { sample: { type: "integer", "x-optionsAction": "ListSamples", "x-order": 0 } },
        required: ["sample"]
    })

    Component {
        id: gridSlot
        Item {
            objectName: "gridSlot"
            property var field
            property var setValue
            property var rows: []
            property string fieldText
        }
    }

    Component {
        id: noteSlot
        Item {
            objectName: "noteSlot"
            property var field
            property var setValue
            property string fieldText
        }
    }

    Component {
        id: registryComponent
        SlotRegistry {}
    }

    Component {
        id: sampleForm
        DynamicForm { actionType: "T_Sample"; schema: testCase.sampleSchema; controller: recordingController }
    }

    Component {
        id: choiceForm
        DynamicForm { actionType: "T_Choice"; schema: testCase.choiceSchema; controller: recordingController }
    }

    function makeSampleForm(extra) {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byField("T_Sample", "rows", gridSlot)
        registry.byField("T_Sample", "note", noteSlot)
        const props = { slotRegistry: registry }
        for (const key in (extra || {}))
            props[key] = extra[key]
        return createTemporaryObject(sampleForm, testCase, props)
    }

    // ── the round trip ───────────────────────────────────────────────────────

    function test_a_stored_payload_round_trips_through_the_form() {
        recordingController.submissions = 0
        const form = makeSampleForm()
        verify(form.prefillFromJson(testCase.storedPayload))
        compare(form.ready, true)
        compare(form.previewLine, testCase.canonicalPayload)
        // A ready form after a prefill still did not submit.
        compare(recordingController.submissions, 0)
    }

    function test_the_drawn_controls_show_the_prefilled_values() {
        const form = makeSampleForm()
        form.prefillFromJson(testCase.storedPayload)
        compare(findChild(form, "field_id").text, "9007199254740993")
        compare(findChild(form, "field_density").text, "2450.50")
        compare(findChild(form, "field_temperature").text, "21.5")
        compare(findChild(form, "datetime_takenAt").text, "2026-07-20T09:00:00")
        compare(findChild(form, "field_done").checked, true)
        compare(findChild(form, "field_role").currentText, "B")
        compare(findChild(form, "field_tags").text, "a, b")
    }

    function test_slots_receive_the_prefilled_values() {
        const form = makeSampleForm()
        form.prefillFromJson(testCase.storedPayload)
        compare(findChild(form, "noteSlot").fieldText, "retest")
        const rows = findChild(form, "gridSlot").rows
        compare(rows.length, 2)
        compare(rows[0].sieve, "31.5")
        compare(rows[0].passing, "100.0")
        compare(rows[1].sieve, "0.1")
        compare(rows[1].passing, "4.2")
    }

    function test_an_edit_after_prefill_is_submitted_as_edited() {
        const form = makeSampleForm()
        form.prefillFromJson(testCase.storedPayload)
        findChild(form, "field_temperature").text = "22.00"
        verify(form.previewLine.indexOf('"temperature":22.00') !== -1)
        verify(form.previewLine.indexOf('"id":9007199254740993') !== -1)
    }

    // ── locale and zone ──────────────────────────────────────────────────────

    function test_numbers_are_prefilled_in_the_display_locale() {
        const form = makeSampleForm({ displayLocale: "de_DE" })
        form.prefillFromJson(testCase.storedPayload)
        compare(findChild(form, "field_density").text, "2450,50")
        compare(findChild(form, "field_temperature").text, "21,5")
        compare(form.previewLine, testCase.canonicalPayload)
    }

    function test_a_timestamp_is_prefilled_in_the_display_zone() {
        const form = makeSampleForm({ displayOffsetMinutes: 120 })
        form.prefillFromJson(testCase.storedPayload)
        compare(findChild(form, "datetime_takenAt").text, "2026-07-20T11:00:00")
        verify(form.previewLine.indexOf('"takenAt":"2026-07-20T09:00:00Z"') !== -1)
    }

    // ── what a prefill replaces ──────────────────────────────────────────────

    function test_a_member_absent_from_the_payload_starts_blank() {
        const form = makeSampleForm()
        form.prefillFromJson(testCase.storedPayload)
        form.prefillFromJson('{"id":5}')
        compare(findChild(form, "field_temperature").text, "")
        compare(findChild(form, "gridSlot").rows.length, 0)
        compare(findChild(form, "noteSlot").fieldText, "")
        compare(form.ready, false)
    }

    function test_the_unit_selector_returns_to_the_canonical_unit() {
        const form = makeSampleForm()
        const selector = findChild(form, "unit_density")
        selector.currentIndex = 1
        selector.activated(1)
        form.prefillFromJson(testCase.storedPayload)
        compare(selector.currentIndex, 0)
        compare(findChild(form, "field_density").text, "2450.50")
        verify(form.previewLine.indexOf('"density":{"num":245050,"den":100,"dp":2}') !== -1)
    }

    function test_text_that_is_not_an_object_changes_nothing() {
        const form = makeSampleForm()
        form.prefillFromJson(testCase.storedPayload)
        const before = form.previewLine
        verify(!form.prefillFromJson("not json"))
        verify(!form.prefillFromJson("[1,2]"))
        compare(form.previewLine, before)
    }

    // ── a fetched Choice ─────────────────────────────────────────────────────

    function test_a_fetched_choice_is_selected_once_its_options_arrive() {
        const form = createTemporaryObject(choiceForm, testCase)
        form.prefillFromJson('{"sample":9007199254740993}')
        compare(form.previewLine, '{"sample":9007199254740993}')
        // Options arrived at construction; the prefilled id selects its row.
        const combos = []
        function collect(item) {
            if (!item)
                return
            if (item instanceof ComboBox && item.visible)
                combos.push(item)
            for (let i = 0; i < item.children.length; ++i)
                collect(item.children[i])
        }
        collect(form)
        compare(combos.length, 1)
        compare(combos[0].currentText, "Big")
        // A later refetch keeps the selection.
        recordingController.fetchOptions("ListSamples", "{}")
        compare(combos[0].currentText, "Big")
    }

    // ── the decoder on its own ───────────────────────────────────────────────

    function test_decode_is_the_inverse_of_encode_per_kind() {
        const form = makeSampleForm()
        const density = form.fieldByName["density"]
        compare(form.decodeFieldValue(density, { num: 5, den: 4, dp: 2 }), "1.25")
        compare(form.decodeFieldValue(density, { num: -1, den: 3, dp: 2 }), "-0.33")
        compare(form.decodeFieldValue(density, "oops"), "")
        const temperature = form.fieldByName["temperature"]
        compare(form.decodeFieldValue(temperature, 1e-7), "0.0000001")
        compare(form.decodeFieldValue(temperature, 3), "3")
        // A declared display precision (FieldMeta::decimals' x-displayDecimals)
        // pads, and never rounds a finer stored value away.
        const twoPlaces = { isNumber: true, displayDecimals: 2 }
        compare(form.decodeFieldValue(twoPlaces, 3), "3.00")
        compare(form.decodeFieldValue(twoPlaces, 21.5), "21.50")
        compare(form.decodeFieldValue(twoPlaces, 1.23456), "1.23456")
        compare(form.decodeFieldValue(form.fieldByName["done"], false), "false")
        compare(form.decodeFieldValue(form.fieldByName["role"], "A"), '"A"')
        compare(form.decodeFieldValue(form.fieldByName["takenAt"], "2026-01-01T00:30:00+01:00"),
                "2025-12-31T23:30:00")
    }
}
