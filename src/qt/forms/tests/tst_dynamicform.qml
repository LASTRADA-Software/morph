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
                "slot": { "type": ["integer", "null"], "x-order": 0, "title": "Slot",
                          "x-optionsAction": "ListSlots", "x-optionValue": "id", "x-optionLabel": "name" },
                "mass": { "$ref": "#/$defs/q", "x-order": 1, "x-decimalPlaces": 3, "title": "Mass",
                          "x-placeholder": "e.g. 1050",
                          "ExtUnits": { "unitAscii": "kg", "unitUnicode": "kg" },
                          "x-unitAlternatives": [
                              { "id": "g", "display": "g", "decimals": 1, "num": 1, "den": 1000 },
                              { "id": "t", "display": "t", "decimals": 4, "num": 1000, "den": 1 }
                          ] },
                "when": { "type": ["string", "null"], "format": "date-time", "x-order": 2, "title": "When",
                          "x-readonly": true },
                "note": { "type": ["string", "null"], "x-order": 3, "title": "Notes", "x-hidden": true }
            },
            "$defs": { "q": { "type": ["object", "null"] } },
            "required": ["slot", "mass", "when"]
        })
    }

    DynamicForm {
        id: layoutForm
        actionType: "LayoutProbe"
        controller: null
        schema: ({
            "properties": {
                "sampleId": { "type": ["integer", "null"], "x-order": 0, "x-group": "Identity", "x-section": 0 },
                "density": { "type": ["number", "null"], "x-order": 1, "x-group": "Measurement", "x-section": 1 },
                "moisture": { "type": ["number", "null"], "x-order": 2, "x-group": "Measurement", "x-section": 1 },
                "notes": { "type": ["string", "null"], "x-order": 3, "x-group": "Notes", "x-section": 2, "x-colspan": 2 },
                "remarks": { "type": ["string", "null"], "x-order": 4 }
            },
            "required": ["sampleId"],
            "x-layout": {
                "groups": [
                    { "title": "Identity", "kind": "section", "fields": ["sampleId"] },
                    { "title": "Measurement", "kind": "section", "fields": ["moisture", "density"] },
                    { "title": "Notes", "kind": "accordion", "fields": ["notes"] }
                ]
            }
        })
    }

    DynamicForm {
        id: tabForm
        actionType: "TabProbe"
        controller: null
        schema: ({
            "properties": {
                "a": { "type": ["integer", "null"], "x-order": 0, "x-section": 0 },
                "b": { "type": ["integer", "null"], "x-order": 1, "x-section": 1 },
                "c": { "type": ["integer", "null"], "x-order": 2, "x-section": 2 }
            },
            "required": [],
            "x-layout": {
                "groups": [
                    { "title": "One", "kind": "tab", "fields": ["a"] },
                    { "title": "Two", "kind": "tab", "fields": ["b"] },
                    { "title": "Solo", "kind": "section", "fields": ["c"] }
                ]
            }
        })
    }

    // A fourth, independent form probing x-widget/x-min/x-max/x-step — kept
    // separate from `form`/`layoutForm`/`tabForm` above so these fields never
    // perturb their assertions (field count, previewLine, readiness gate).
    DynamicForm {
        id: whForm
        actionType: "WidgetHintsProbe"
        controller: null
        schema: ({
            "properties": {
                "summary": { "type": ["string", "null"], "x-order": 0, "x-widget": "textarea" },
                "level": { "type": ["integer", "null"], "x-order": 1,
                           "x-widget": "slider", "x-min": 0, "x-max": 100, "x-step": 5 },
                "mode": { "type": ["integer", "null"], "x-order": 2, "x-widget": "radio",
                          "x-optionsAction": "ListModes", "x-optionValue": "id", "x-optionLabel": "name" },
                "plain": { "type": ["string", "null"], "x-order": 3 }
            },
            "required": ["summary", "level", "mode"]
        })
    }

    // A fifth, independent form probing x-optionsDependsOn (cascading
    // Choice options) — kept separate so its country/city fields never
    // perturb the other fixtures' assertions.
    DynamicForm {
        id: depForm
        actionType: "ShipTo"
        controller: null
        schema: ({
            "properties": {
                "country": { "type": ["integer", "null"], "x-order": 0,
                             "x-optionsAction": "ListCountries", "x-optionValue": "id", "x-optionLabel": "name" },
                "city": { "type": ["integer", "null"], "x-order": 1,
                          "x-optionsAction": "ListCities", "x-optionValue": "id", "x-optionLabel": "name",
                          "x-optionsDependsOn": ["country"] }
            },
            "required": ["country", "city"]
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
            compare(slot.title, "Slot")
            verify(!slot.readOnly && !slot.hidden)

            const mass = form.fields[1]
            verify(mass.isQuantity)
            compare(mass.canonDp, 3)
            compare(mass.unit, "kg")
            compare(mass.unitOptions.length, 3)  // canonical + g + t
            compare(mass.unitOptions[1].display, "g")
            compare(mass.unitOptions[2].num, 1000)
            compare(mass.title, "Mass")
            compare(mass.placeholder, "e.g. 1050")

            const when = form.fields[2]
            verify(when.isDateTime)
            verify(when.required)
            compare(when.title, "When")
            verify(when.readOnly)

            const note = form.fields[3]
            verify(!note.isChoice && !note.isDateTime && !note.isQuantity && !note.isInteger)
            verify(!note.required)
            compare(note.title, "Notes")
            verify(note.hidden)
        }

        function test_widgetHintFieldDescriptors() {
            compare(whForm.fields.length, 4)

            const summary = whForm.fields[0]
            verify(summary.isMultiline)
            verify(!summary.isSlider && !summary.isRadioChoice)

            const level = whForm.fields[1]
            verify(level.isSlider)
            verify(!level.isMultiline)
            compare(level.sliderMin, 0)
            compare(level.sliderMax, 100)
            compare(level.sliderStep, 5)

            const mode = whForm.fields[2]
            verify(mode.isChoice)
            verify(mode.isRadioChoice)

            const plain = whForm.fields[3]
            verify(!plain.isMultiline && !plain.isSlider && !plain.isRadioChoice)
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

        function test_layoutGroupsBucketFieldsInOrder() {
            compare(layoutForm.sections.length, 4)  // Identity, Measurement, Notes, trailing "remarks"
            compare(layoutForm.sections[0].title, "Identity")
            compare(layoutForm.sections[0].kind, "section")
            compare(layoutForm.sections[0].fields.map(f => f.name).join(","), "sampleId")
            // The schema's x-layout.groups[1].fields deliberately lists
            // "moisture" before "density" (the opposite of their x-order):
            // the renderer buckets by each field's own x-section, then
            // relies on `fields` already being x-order-sorted, so the
            // bucket comes out "density,moisture" regardless of the
            // declared array order — x-order is the sole intra-group
            // ordering authority.
            compare(layoutForm.sections[1].fields.map(f => f.name).join(","), "density,moisture")
            compare(layoutForm.sections[2].kind, "accordion")
            compare(layoutForm.sections[2].fields.map(f => f.name).join(","), "notes")
            // The field absent from every declared group falls into the
            // implicit trailing group, never dropped.
            compare(layoutForm.sections[3].title, "")
            compare(layoutForm.sections[3].fields.map(f => f.name).join(","), "remarks")
            // colspan surfaces per field; the default (1) applies when
            // x-colspan is absent.
            compare(layoutForm.fields.find(f => f.name === "notes").colspan, 2)
            compare(layoutForm.fields.find(f => f.name === "sampleId").colspan, 1)
        }

        function test_tabSectionsMergeIntoOneRun() {
            compare(tabForm.renderRuns.length, 2)  // {a,b} tabset, then solo "c" section
            compare(tabForm.renderRuns[0].type, "tabset")
            compare(tabForm.renderRuns[0].sections.length, 2)
            compare(tabForm.renderRuns[0].sections[0].title, "One")
            compare(tabForm.renderRuns[0].sections[1].title, "Two")
            compare(tabForm.renderRuns[1].type, "single")
            compare(tabForm.renderRuns[1].section.title, "Solo")
        }

        function test_noXLayoutFallsBackToOneFlatSection() {
            // `form` (the file-scope fixture) declares no x-layout.
            compare(form.sections.length, 1)
            compare(form.sections[0].kind, "flat")
            compare(form.sections[0].fields.length, form.fields.length)
            compare(form.renderRuns.length, 1)
            compare(form.renderRuns[0].type, "single")
        }

        function test_dependsOnFieldDescriptor() {
            compare(depForm.fields[0].dependsOn.length, 0)     // country: independent
            compare(depForm.fields[1].dependsOn.length, 1)
            compare(depForm.fields[1].dependsOn[0], "country")
        }

        function test_dependentsIsReverseOfDependsOn() {
            compare(depForm.dependents["country"].length, 1)
            compare(depForm.dependents["country"][0], "city")
            verify(depForm.dependents["city"] === undefined)
        }

        function test_fieldByNameIndexesByName() {
            compare(depForm.fieldByName["country"].name, "country")
            compare(depForm.fieldByName["city"].name, "city")
        }

        function test_fieldJsonLiteralAndOptionsRequestBody() {
            // Fresh component: country/city both start blank, so the parent
            // is not yet engaged and the dependent field cannot be fetched.
            compare(depForm.fieldJsonLiteral(depForm.fieldByName["country"]), null)
            verify(depForm.optionsRequestBody(depForm.fieldByName["city"]) === null)

            depForm.setFieldValue("country", "1")
            compare(depForm.fieldJsonLiteral(depForm.fieldByName["country"]), "1")
            compare(depForm.optionsRequestBody(depForm.fieldByName["city"]), '{"country":1}')
        }
    }
}
