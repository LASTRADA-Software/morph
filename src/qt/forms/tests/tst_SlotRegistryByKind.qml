// SPDX-License-Identifier: Apache-2.0
//
// SlotRegistry.byKind: one host control per renderer kind.
//
// The JSON type a field carries does not name the control it needs -- a
// Quantity and a nested object are both "object", a Choice is "integer", an
// enum and a date-time are "string" -- so a host registering one component per
// kind of control had to go field by field. Each case below builds the schema
// shape schemaJson<A>() emits for one member kind and asserts both halves: the
// descriptor's `kind`, and that a byKind slot (not the byType one registered
// for the same JSON type) is what the form loads.

pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "SlotRegistryByKind"
    visible: true

    property var kindSchema: ({
        "$defs": {
            "double": { type: "number" },
            "Row": { type: "object", properties: { a: { type: "integer", "x-order": 0 } }, required: ["a"] },
            "Sub": { type: "object", properties: { b: { type: "string", "x-order": 0 } } }
        },
        properties: {
            mass: {
                type: ["object", "null"],
                properties: { num: { type: "integer" }, den: { type: "integer" }, dp: { type: "integer" } },
                ExtUnits: { unitAscii: "kg", unitUnicode: "kg" }, "x-decimalPlaces": 2, "x-order": 0
            },
            sample: { type: "integer", "x-optionsAction": "ListSamples", "x-order": 1 },
            role: { type: "string", oneOf: [{ title: "A", const: "A" }, { title: "B", const: "B" }], "x-order": 2 },
            takenAt: { type: "string", format: "date-time", "x-order": 3 },
            day: { type: "string", format: "date", "x-order": 4 },
            done: { type: "boolean", "x-order": 5 },
            count: { type: "integer", "x-order": 6 },
            ratio: { "$ref": "#/$defs/double", "x-order": 7 },
            note: { type: "string", "x-order": 8 },
            tags: { type: "array", items: { type: "string" }, "x-order": 9 },
            rows: { type: "array", items: { "$ref": "#/$defs/Row" }, "x-order": 10 },
            sub: { "$ref": "#/$defs/Sub", "x-order": 11 }
        },
        required: []
    })

    readonly property var expectedKinds: ({
        mass: "quantity", sample: "choice", role: "enum", takenAt: "datetime", day: "date",
        done: "boolean", count: "integer", ratio: "number", note: "string", tags: "array",
        rows: "objectArray", sub: "object"
    })

    // A slot that says which kind it was registered for.
    Component {
        id: kindSlot
        Item {
            property var field
            property var setValue
            property string registeredKind
            objectName: "kindSlot_" + (field ? field.name : "")
        }
    }

    Component {
        id: typeSlot
        Item {
            property var field
            property var setValue
            objectName: "typeSlot_" + (field ? field.name : "")
        }
    }

    Component {
        id: registryComponent
        SlotRegistry {}
    }

    Component {
        id: formComponent
        DynamicForm { actionType: "T_Kind"; controller: null; schema: testCase.kindSchema }
    }

    function test_every_field_names_its_kind() {
        const form = createTemporaryObject(formComponent, testCase)
        for (const name in expectedKinds)
            compare(form.fieldByName[name].kind, expectedKinds[name], name)
    }

    function test_resolution_order_puts_kind_between_unit_and_type() {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byType("object", typeSlot)
        registry.byKind("quantity", kindSlot)
        compare(registry.resolve("T", "mass", "", "", "object", "quantity"), kindSlot)
        registry.byUnit("kg", typeSlot)
        compare(registry.resolve("T", "mass", "", "kg", "object", "quantity"), typeSlot)
        // The five-argument form still resolves as before.
        compare(registry.resolve("T", "other", "", "", "object"), typeSlot)
    }

    function test_a_kind_slot_is_loaded_for_every_field_of_that_kind() {
        const registry = createTemporaryObject(registryComponent, testCase)
        // byType for each JSON type in play: a kind must beat it.
        const jsonTypes = ["object", "integer", "string", "boolean", "number", "array"]
        for (let t = 0; t < jsonTypes.length; ++t)
            registry.byType(jsonTypes[t], typeSlot)
        for (const name in expectedKinds)
            registry.byKind(expectedKinds[name], kindSlot)
        const form = createTemporaryObject(formComponent, testCase, { slotRegistry: registry })
        for (const name in expectedKinds) {
            verify(findChild(form, "kindSlot_" + name) !== null, name)
            compare(findChild(form, "typeSlot_" + name), null, name)
        }
    }

    function test_an_unregistered_kind_falls_through_to_the_type() {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byType("integer", typeSlot)
        registry.byKind("choice", kindSlot)
        const form = createTemporaryObject(formComponent, testCase, { slotRegistry: registry })
        verify(findChild(form, "kindSlot_sample") !== null)
        verify(findChild(form, "typeSlot_count") !== null)
    }

    function test_a_kind_slot_drives_the_form() {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byKind("quantity", kindSlot)
        const form = createTemporaryObject(formComponent, testCase, { slotRegistry: registry })
        findChild(form, "kindSlot_mass").setValue("1.25")
        compare(form.previewLine, '{"mass":{"num":125,"den":100,"dp":2}}')
    }
}
