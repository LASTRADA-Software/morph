// SPDX-License-Identifier: Apache-2.0
//
// Conformance kit -- functional assertions against fixture schemas mirroring
// the C++ corpus in tests/test_forms_conformance_corpus.cpp (kept in sync by
// hand; each fixture below is named after its C++ counterpart). Proves the
// shipped MorphForms renderer honors the forms.md "Renderer contract" for
// every corpus feature: field order, the required-gate, exact Quantity
// payloads + unit conversion, Choice's descriptor (the empty-body options
// fetch itself is asserted in src/qt/forms/tests/test_forms_controller_core.cpp,
// since DynamicForm never calls the options action directly -- its
// controller does), Timestamp, and the $ref dual-read for two properties
// sharing one $def.
//
// Known limitation, noted explicitly (not hidden): the C++ corpus and this
// QML mirror are two independently-maintained representations of the same
// five fixtures, synchronized by hand and by the cross-referencing comments
// in both files, not by a shared loaded file.
//
// Collection/wizard/app (v-*/w-*/app-*) fixtures are deferred: no emitter
// exists yet for those Tier-2 view-schema keys (gui_collections_views.md /
// gui_workflows_navigation.md are still "Status: planned"), so there is
// nothing here yet to generate or pin a corpus fixture against.

pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import MorphForms

Item {
    width: 480
    height: 480

    // Mirrors CFScalarsAndRequired: count (int64, required), label (string,
    // required), note (optional).
    property var scalarsAndRequiredSchema: ({
        properties: {
            count: { type: "integer", "x-order": 0 },
            label: { type: "string", "x-order": 1 },
            note: { type: ["string", "null"], "x-order": 2 }
        },
        required: ["count", "label"]
    })

    // Mirrors CFQuantityAlternatives: mass in kg (3 decimals), convertible to
    // g (1 decimal) and t (4 decimals). ExtUnits lives in the shared $def,
    // matching the real forms.hpp output shape (not on the property).
    property var quantityAlternativesSchema: ({
        properties: {
            mass: { "$ref": "#/$defs/q", "x-order": 0, "x-decimalPlaces": 3,
                    "x-unitAlternatives": [
                        { id: "g", display: "g", decimals: 1, num: 1, den: 1000 },
                        { id: "t", display: "t", decimals: 4, num: 1000, den: 1 }
                    ] }
        },
        "$defs": { q: { type: ["object", "null"], ExtUnits: { unitAscii: "kg", unitUnicode: "kg" } } },
        required: ["mass"]
    })

    // Mirrors CFChoiceField: widgetId picked from CFListWidgets.
    property var choiceFieldSchema: ({
        properties: {
            widgetId: { type: ["integer", "null"], "x-order": 0,
                        "x-optionsAction": "CFListWidgets", "x-optionValue": "id", "x-optionLabel": "name" }
        },
        required: ["widgetId"]
    })

    // Mirrors CFTimestampField.
    property var timestampFieldSchema: ({
        properties: {
            when: { type: ["string", "null"], format: "date-time", "x-order": 0 }
        },
        required: ["when"]
    })

    // Mirrors CFSharedDefFields: massA/massB both reference the SAME $def --
    // the mandatory dual-read (property x-order differs; ExtUnits/def shared).
    property var sharedDefFieldsSchema: ({
        properties: {
            massA: { "$ref": "#/$defs/q", "x-order": 0, "x-decimalPlaces": 3 },
            massB: { "$ref": "#/$defs/q", "x-order": 1, "x-decimalPlaces": 3 }
        },
        "$defs": { q: { type: ["object", "null"], ExtUnits: { unitAscii: "kg", unitUnicode: "kg" } } },
        required: ["massA", "massB"]
    })

    DynamicForm { id: scalarsForm; actionType: "CFScalarsAndRequired"; controller: null; schema: scalarsAndRequiredSchema }
    DynamicForm { id: quantityForm; actionType: "CFQuantityAlternatives"; controller: null; schema: quantityAlternativesSchema }
    DynamicForm { id: choiceForm; actionType: "CFChoiceField"; controller: null; schema: choiceFieldSchema }
    DynamicForm { id: timestampForm; actionType: "CFTimestampField"; controller: null; schema: timestampFieldSchema }
    DynamicForm { id: sharedDefForm; actionType: "CFSharedDefFields"; controller: null; schema: sharedDefFieldsSchema }

    TestCase {
        name: "ConformanceCorpus"

        function test_fieldsRenderInDeclarationOrderNotJsonKeyOrder() {
            compare(scalarsForm.fields.map(f => f.name).join(","), "count,label,note")
        }

        function test_requiredGateBlocksSubmitUntilEveryRequiredFieldIsEngaged() {
            verify(!scalarsForm.ready)
            scalarsForm.setFieldValue("count", "3")
            verify(!scalarsForm.ready)   // label still empty
            scalarsForm.setFieldValue("label", "widget-7")
            verify(scalarsForm.ready)    // note is optional
            compare(scalarsForm.previewLine, '{"count":3,"label":"widget-7"}')
        }

        function test_quantityPayloadIsExactAndUnitSwitchRecomputesExactly() {
            quantityForm.setFieldValue("mass", "2650.5")
            verify(quantityForm.ready)
            compare(quantityForm.previewLine, '{"mass":{"num":2650500,"den":1000,"dp":3}}')

            // Switch to grams (unitOptions[1]): the same physical value,
            // recomputed exactly via convertText, then re-submitted.
            const kg = quantityForm.fields[0].unitOptions[0]
            const g = quantityForm.fields[0].unitOptions[1]
            quantityForm.fieldUnits["mass"] = 1
            quantityForm.setFieldValue("mass", quantityForm.convertText("2650.5", kg, g))
            verify(quantityForm.ready)
            compare(quantityForm.previewLine, '{"mass":{"num":26505000,"den":10000,"dp":3}}')
            quantityForm.fieldUnits["mass"] = 0
        }

        function test_choiceFieldDescriptorCarriesTheDeclaredOptionsContract() {
            const widgetId = choiceForm.fields[0]
            verify(widgetId.isChoice)
            compare(widgetId.optionsAction, "CFListWidgets")
            compare(widgetId.valueField, "id")
            compare(widgetId.labelField, "name")
        }

        function test_timestampFieldRendersAsDateTimeAndGatesOnIso8601() {
            verify(timestampForm.fields[0].isDateTime)
            verify(!timestampForm.ready)
            timestampForm.setFieldValue("when", "2026-07-20T09:00")
            verify(timestampForm.ready)
            compare(timestampForm.previewLine, '{"when":"2026-07-20T09:00:00Z"}')
        }

        function test_sharedDefDualReadPropertyOrderDiffersDefExtUnitsShared() {
            compare(sharedDefForm.fields.map(f => f.name + ":" + f.canonDp).join(","), "massA:3,massB:3")
            compare(sharedDefForm.fields[0].unit, "kg")
            compare(sharedDefForm.fields[1].unit, "kg")
            compare(sharedDefForm.fields[0].name, "massA")   // x-order 0 before massB's 1
        }
    }
}
