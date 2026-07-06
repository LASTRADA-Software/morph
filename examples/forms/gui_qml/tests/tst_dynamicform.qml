// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DynamicForm's logic, driven against the real component:
// schema -> field descriptors, the exact digit-string arithmetic, unit
// conversion, payload composition, the readiness gate, and option-row
// extraction. No controller is wired — this is the pure form logic.

import QtQuick
import QtTest
import MorphForms

Item {
    width: 480
    height: 480

    DynamicForm {
        id: form
        actionType: "Probe"
        controller: null
        schema: ({
            "properties": {
                "slot": { "type": ["integer", "null"], "x-order": 0,
                          "x-optionsAction": "ListSlots", "x-optionValue": "id", "x-optionLabel": "name" },
                "mass": { "$ref": "#/$defs/q", "x-order": 1, "x-decimalPlaces": 3,
                          "ExtUnits": { "unitAscii": "kg", "unitUnicode": "kg" },
                          "x-unitAlternatives": [
                              { "id": "g", "display": "g", "decimals": 1, "num": 1, "den": 1000 },
                              { "id": "t", "display": "t", "decimals": 4, "num": 1000, "den": 1 }
                          ] },
                "when": { "type": ["string", "null"], "format": "date-time", "x-order": 2 },
                "note": { "type": ["string", "null"], "x-order": 3 }
            },
            "$defs": { "q": { "type": ["object", "null"] } },
            "required": ["slot", "mass", "when"]
        })
    }

    TestCase {
        name: "DynamicFormLogic"

        function test_fieldDescriptorsFromSchema() {
            compare(form.fields.length, 4)
            // Declaration order restored from x-order.
            compare(form.fields.map(f => f.name).join(","), "slot,mass,when,note")

            const slot = form.fields[0]
            verify(slot.isChoice)
            compare(slot.optionsAction, "ListSlots")
            compare(slot.valueField, "id")
            compare(slot.labelField, "name")
            verify(slot.required)

            const mass = form.fields[1]
            verify(mass.isQuantity)
            compare(mass.canonDp, 3)
            compare(mass.unit, "kg")
            compare(mass.unitOptions.length, 3)  // canonical + g + t
            compare(mass.unitOptions[1].display, "g")
            compare(mass.unitOptions[2].num, 1000)

            const when = form.fields[2]
            verify(when.isDateTime)
            verify(when.required)

            const note = form.fields[3]
            verify(!note.isChoice && !note.isDateTime && !note.isQuantity && !note.isInteger)
            verify(!note.required)
        }

        function test_longArithmetic() {
            compare(form.mulDigits("123", 45), "5535")
            compare(form.mulDigits("0", 999), "0")
            compare(form.divRoundDigits("5535", 45), "123")
            compare(form.divRoundDigits("10", 4), "3")   // 2.5 rounds half up
            compare(form.divRoundDigits("9", 4), "2")    // 2.25 rounds down
            compare(form.incDigits("199"), "200")
            compare(form.incDigits("999"), "1000")
            compare(form.scaledDigits("2650.5", 3).digits, "2650500")
            compare(form.scaledDigits("-0.5", 1).neg, true)
        }

        function test_unitConversion() {
            const kg = { decimals: 3, num: 1, den: 1 }
            const g = { decimals: 1, num: 1, den: 1000 }
            const tonne = { decimals: 4, num: 1000, den: 1 }
            compare(form.convertText("2650.5", kg, g), "2650500.0")
            compare(form.convertText("2650500.0", g, kg), "2650.500")
            compare(form.convertText("2650.5", kg, tonne), "2.6505")
            compare(form.convertText("2.6505", tonne, g), "2650500.0")
            compare(form.convertText("-0.001", kg, g), "-1.0")
            compare(form.convertText("0.05", { decimals: 2, num: 1, den: 1 },
                                     { decimals: 1, num: 1, den: 1 }), "0.1")  // half up
            compare(form.convertText("junk", kg, g), "")
        }

        function test_rationalJsonPayloads() {
            compare(form.rationalJson("2650.5", { decimals: 1, num: 1, den: 1 }, 1),
                    '{"num":26505,"den":10,"dp":1}')
            // A value typed in grams folds the exact ratio; dp stays canonical.
            compare(form.rationalJson("2650500.0", { decimals: 1, num: 1, den: 1000 }, 3),
                    '{"num":26505000,"den":10000,"dp":3}')
            compare(form.rationalJson("-0.5", { decimals: 1, num: 1, den: 1 }, 1),
                    '{"num":-5,"den":10,"dp":1}')
            // Digit-exact beyond 2^53.
            compare(form.rationalJson("123456789012345678.9", { decimals: 1, num: 1, den: 1 }, 1),
                    '{"num":1234567890123456789,"den":10,"dp":1}')
        }

        function test_readinessGateAndComposition() {
            verify(!form.ready)  // three required fields empty

            form.setFieldValue("slot", "4")                          // choice: JSON literal
            verify(!form.ready)
            form.setFieldValue("when", "2026-07-05T14:30")           // datetime, no seconds
            verify(!form.ready)
            form.setFieldValue("mass", "2650.5")
            verify(form.ready)
            // Composition follows x-order (slot, mass, when), not entry order;
            // the canonical entry "2650.5" at 3 declared decimals scales to
            // 2650500/1000.
            compare(form.previewLine,
                    '{"slot":4,"mass":{"num":2650500,"den":1000,"dp":3},"when":"2026-07-05T14:30:00Z"}')

            // Over-precise quantity input is rejected, not silently rounded.
            form.setFieldValue("mass", "2650.5001")
            verify(!form.ready)
            // Malformed datetime is rejected.
            form.setFieldValue("mass", "2650.5")
            form.setFieldValue("when", "not-a-date")
            verify(!form.ready)
            form.setFieldValue("when", "2026-07-05T14:30:15")
            verify(form.ready)

            // Switching the entry unit re-bases composition (grams here).
            form.fieldUnits["mass"] = 1
            form.setFieldValue("mass", "2650500.0")
            verify(form.ready)
            // "2650500.0" grams at 1 decimal: 26505000 tenth-grams, ratio
            // 1/1000 folded into the denominator; dp stays canonical.
            compare(form.previewLine,
                    '{"slot":4,"mass":{"num":26505000,"den":10000,"dp":3},"when":"2026-07-05T14:30:15Z"}')
            form.fieldUnits["mass"] = 0
            form.setFieldValue("mass", "2650.5")
        }

        function test_optionRows() {
            compare(form.optionRows([{ id: 1 }]).length, 1)                       // bare array
            compare(form.optionRows({ rows: [{ id: 1 }, { id: 2 }] }).length, 2)  // first array member
            compare(form.optionRows({ nothing: 1 }).length, 0)
        }
    }
}
