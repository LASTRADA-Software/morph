// SPDX-License-Identifier: Apache-2.0
//
// A plain number's display unit and decimals -- `ExtUnits` and
// `x-displayDecimals` on a `"number"` property, what `FieldMeta::unit` /
// `::decimals` stamp there (tests/test_forms_display_unit.cpp pins the C++
// half against the same key names).
//
// The unit is read exactly where a Quantity's is, so the suffix label and the
// `byUnit` slot tier see it with no code of their own. The decimals are the
// new part: an entry limit that keeps the plain JSON-number encoding, which is
// why every case asserts the submitted body and not only `ready`.

import QtQuick
import QtQuick.Controls
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormDisplayUnit"
    visible: true

    // `struct { double density; double temperature; }` with
    // FieldMeta{.unit = "kg/m³", .decimals = 3} / FieldMeta{.unit = "°C"}.
    property var readingSchema: ({
        "$defs": {
            "double": { type: "number", minimum: -1.7976931348623157e+308, maximum: 1.7976931348623157e+308 }
        },
        properties: {
            density: {
                "$ref": "#/$defs/double", "x-order": 0, title: "Density",
                ExtUnits: { unitAscii: "kg/m³", unitUnicode: "kg/m³" }, "x-displayDecimals": 3
            },
            temperature: {
                "$ref": "#/$defs/double", "x-order": 1, title: "Temperature",
                ExtUnits: { unitAscii: "°C", unitUnicode: "°C" }
            }
        },
        required: ["density"]
    })

    // A declared precision beside a display precision: the first encodes, so
    // the second is not read at all.
    property var precisionSchema: ({
        properties: {
            reading: { type: "number", "x-decimalPlaces": 1, "x-displayDecimals": 3, "x-order": 0 }
        },
        required: ["reading"]
    })

    // A display precision of zero: whole numbers only, still a JSON number.
    property var wholeSchema: ({
        properties: { count: { type: "number", "x-displayDecimals": 0, "x-order": 0 } },
        required: ["count"]
    })

    Component {
        id: readingForm
        DynamicForm { actionType: "T_Reading"; schema: testCase.readingSchema; controller: null }
    }

    Component {
        id: precisionForm
        DynamicForm { actionType: "T_Precision"; schema: testCase.precisionSchema; controller: null }
    }

    Component {
        id: wholeForm
        DynamicForm { actionType: "T_Whole"; schema: testCase.wholeSchema; controller: null }
    }

    // A host slot registered for the unit: what it is handed is the contract.
    Component {
        id: unitSlot
        TextField {
            objectName: "unitSlot"
            property var field
            property var setValue
            onTextChanged: if (setValue) setValue(text)
        }
    }

    Component {
        id: slotRegistryComponent
        SlotRegistry {}
    }

    function typeInto(form, field, text) {
        findChild(form, field).text = text
    }

    function descriptor(form, name) {
        return form.fieldByName[name]
    }

    // Depth-first search for a visible Label showing exactly `text`.
    function visibleLabel(item, text) {
        if (!item)
            return null
        if (item instanceof Label && item.text === text && item.visible)
            return item
        const kids = item.children || []
        for (let i = 0; i < kids.length; ++i) {
            const found = visibleLabel(kids[i], text)
            if (found)
                return found
        }
        return null
    }

    function test_the_descriptor_carries_the_unit_and_the_decimals() {
        const form = createTemporaryObject(readingForm, testCase)
        const density = descriptor(form, "density")
        compare(density.unit, "kg/m³")
        compare(density.unitAscii, "kg/m³")
        compare(density.decimals, 3)
        compare(density.decimalsDeclared, true)
        compare(density.isNumber, true)
        compare(density.isQuantity, false)

        const temperature = descriptor(form, "temperature")
        compare(temperature.unit, "°C")
        compare(temperature.decimals, 0)
        compare(temperature.decimalsDeclared, false)
    }

    function test_the_unit_is_shown_beside_the_control() {
        const form = createTemporaryObject(readingForm, testCase)
        verify(visibleLabel(form, "kg/m³") !== null)
        verify(visibleLabel(form, "°C") !== null)
    }

    function test_the_placeholder_shows_the_declared_decimals() {
        const form = createTemporaryObject(readingForm, testCase)
        compare(findChild(form, "field_density").placeholderText, "0.000")
        compare(findChild(form, "field_temperature").placeholderText, "")
    }

    function test_an_entry_within_the_decimals_is_a_json_number() {
        const form = createTemporaryObject(readingForm, testCase)
        typeInto(form, "field_density", "2.505")
        compare(form.ready, true)
        compare(form.previewLine, '{"density":2.505}')
    }

    function test_more_decimals_than_declared_are_refused_not_rounded() {
        const form = createTemporaryObject(readingForm, testCase)
        typeInto(form, "field_density", "2.5051")
        compare(form.ready, false)
        compare(form.previewLine, "")
    }

    function test_a_unit_without_decimals_leaves_the_fraction_unlimited() {
        const form = createTemporaryObject(readingForm, testCase)
        typeInto(form, "field_density", "1")
        typeInto(form, "field_temperature", "20.123456")
        compare(form.ready, true)
        compare(form.previewLine, '{"density":1,"temperature":20.123456}')
    }

    function test_zero_decimals_accepts_whole_numbers_only() {
        const form = createTemporaryObject(wholeForm, testCase)
        compare(findChild(form, "field_count").placeholderText, "0")
        typeInto(form, "field_count", "12")
        compare(form.previewLine, '{"count":12}')
        typeInto(form, "field_count", "12.5")
        compare(form.ready, false)
    }

    function test_a_declared_precision_still_wins() {
        const form = createTemporaryObject(precisionForm, testCase)
        const reading = descriptor(form, "reading")
        compare(reading.isQuantity, true)
        compare(reading.decimals, 1)
        typeInto(form, "field_reading", "3.5")
        compare(form.previewLine, '{"reading":{"num":35,"den":10,"dp":1}}')
    }

    function test_a_unit_slot_receives_the_unit_and_the_decimals() {
        const registry = createTemporaryObject(slotRegistryComponent, testCase)
        registry.byUnit("kg/m³", unitSlot)
        const form = createTemporaryObject(readingForm, testCase, { slotRegistry: registry })
        const slot = findChild(form, "unitSlot")
        verify(slot !== null)
        compare(slot.field.name, "density")
        compare(slot.field.unit, "kg/m³")
        compare(slot.field.decimals, 3)
        slot.text = "1.25"
        compare(form.ready, true)
        compare(form.previewLine, '{"density":1.25}')
    }
}
