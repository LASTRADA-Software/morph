// SPDX-License-Identifier: Apache-2.0
//
// Proves the conformance kit's assertions are specific: a renderer that
// violates exactly one rule fails exactly the corresponding assertion and no
// others (docs/spec/forms/forms.md, "Renderer conformance kit").
// BrokenOrderForm.qml / BrokenQuantityForm.qml (this directory) are
// deliberately-wrong test doubles -- never shipped in the MorphForms module.

pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import MorphForms

Item {
    width: 480
    height: 480

    // Deliberately: JSON key order ("label" before "count") differs from
    // x-order (count=0, label=1), so an order bug is actually observable.
    property var reorderedSchema: ({
        properties: {
            label: { type: "string", "x-order": 1 },
            count: { type: "integer", "x-order": 0 }
        },
        required: ["count", "label"]
    })

    DynamicForm { id: conformantOrderForm; actionType: "X"; controller: null; schema: reorderedSchema }
    BrokenOrderForm { id: brokenOrderForm; schema: reorderedSchema }

    property var quantitySchema: ({
        properties: {
            mass: { "$ref": "#/$defs/q", "x-order": 0, "x-decimalPlaces": 3 }
        },
        "$defs": { q: { type: ["object", "null"], ExtUnits: { unitAscii: "kg", unitUnicode: "kg" } } },
        required: ["mass"]
    })

    DynamicForm { id: conformantQuantityForm; actionType: "Y"; controller: null; schema: quantitySchema }
    BrokenQuantityForm { id: brokenQuantityForm }

    TestCase {
        name: "ConformanceNegative"

        function test_conformantRendererRestoresDeclarationOrderFromXOrder() {
            compare(conformantOrderForm.fields.map(f => f.name).join(","), "count,label")
        }

        function test_brokenOrderRendererFailsOnlyTheOrderAssertionNotRequiredness() {
            // Order assertion: FAILS for the broken double (raw JSON key
            // order "label,count", not x-order's "count,label").
            compare(brokenOrderForm.fields.map(f => f.name).join(","), "label,count")
            verify(brokenOrderForm.fields.map(f => f.name).join(",")
                   !== conformantOrderForm.fields.map(f => f.name).join(","))

            // Required-ness assertion: still PASSES for the same broken
            // double -- proving the failure is isolated to ordering.
            const requiredFlags = brokenOrderForm.fields.map(f => f.name + ":" + f.required).sort().join(",")
            compare(requiredFlags, "count:true,label:true")
        }

        function test_conformantRendererRejectsOverPreciseQuantityInput() {
            conformantQuantityForm.setFieldValue("mass", "2650.5001")
            verify(!conformantQuantityForm.ready)
        }

        function test_brokenQuantityRendererFailsOnlyTheNoSilentRoundingAssertion() {
            brokenQuantityForm.text = "2650.5001"
            brokenQuantityForm.canonDp = 3

            // Exact-payload assertion: FAILS for the broken double -- it
            // accepts and silently rounds instead of rejecting.
            verify(brokenQuantityForm.ready())
            compare(brokenQuantityForm.payload(), '{"num":2650500,"den":1000,"dp":3}')

            // Every other assertion this fixture supports is unaffected: a
            // syntactically malformed entry is still rejected (the bug is
            // isolated to over-precision, not general input validation).
            brokenQuantityForm.text = "not-a-number"
            verify(!brokenQuantityForm.ready())
        }
    }
}
