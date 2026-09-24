// SPDX-License-Identifier: Apache-2.0
//
// A collection of objects (`std::vector<Row>`) drawn by a host slot.
//
// Without a slot the renderer has no encoding for such a member and reports it
// unrepresentable (tst_DynamicFormNestedAggregate.qml pins that, unchanged).
// With one, the slot is handed the element's member descriptors -- the columns
// of a grid -- writes the rows as `{member: cellText}` objects, and the form
// encodes every cell with the encoder the same member would get at the top
// level. Every case below asserts the submitted body, because "the slot wrote
// something" is not the claim; "the payload satisfies the schema" is.

pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormObjectArraySlot"
    visible: true

    // `struct Row { double sieve; Quantity<pct, 1> passing; std::string note;
    //   std::int64_t order; }` held as `std::vector<Row> rows`, beside a plain
    // `std::vector<std::string> tags`, the shape schemaJson<A>() emits: the
    // row type under `$defs`, reached through `items.$ref`.
    property var gradingSchema: ({
        "$defs": {
            "double": { type: "number", minimum: -1.7976931348623157e+308, maximum: 1.7976931348623157e+308 },
            "int64_t": { type: "integer", minimum: -9223372036854775808, maximum: 9223372036854775807 },
            "Row": {
                type: "object",
                properties: {
                    note: { type: ["string", "null"], "x-order": 2, title: "Note" },
                    sieve: {
                        "$ref": "#/$defs/double", "x-order": 0, title: "Sieve",
                        ExtUnits: { unitAscii: "mm", unitUnicode: "mm" }
                    },
                    passing: {
                        type: ["object", "null"],
                        properties: { num: { type: "integer" }, den: { type: "integer" }, dp: { type: "integer" } },
                        ExtUnits: { unitAscii: "pct", unitUnicode: "%" },
                        "x-decimalPlaces": 1, "x-order": 1, title: "Passing"
                    },
                    order: { "$ref": "#/$defs/int64_t", "x-order": 3, title: "Order", "x-readonly": true }
                },
                required: ["sieve", "passing"]
            }
        },
        properties: {
            rows: { type: "array", items: { "$ref": "#/$defs/Row" }, "x-order": 0, title: "Rows" },
            tags: { type: "array", items: { type: "string" }, "x-order": 1, title: "Tags" }
        },
        required: ["rows"]
    })

    // A grid slot: it declares the whole optional contract, and records what
    // it was handed.
    Component {
        id: gridSlot
        Item {
            objectName: "gridSlot"
            property var field
            property var setValue
            property var rows: []
            property var setRows
            property string fieldText
            property var form
        }
    }

    // A slot declaring only the two mandatory members: the optional ones must
    // not be assigned (assigning an undeclared property is an error).
    Component {
        id: minimalSlot
        Item {
            objectName: "minimalSlot"
            property var field
            property var setValue
        }
    }

    Component {
        id: registryComponent
        SlotRegistry {}
    }

    // A scalar slot declaring `fieldText`: the optional member is not specific
    // to collections.
    property var labelSchema: ({
        properties: { label: { type: "string", "x-order": 0, title: "Label" } },
        required: ["label"]
    })

    Component {
        id: textSlot
        Item {
            objectName: "textSlot"
            property var field
            property var setValue
            property string fieldText
        }
    }

    Component {
        id: labelForm
        DynamicForm { actionType: "T_Label"; schema: testCase.labelSchema; controller: null }
    }

    Component {
        id: gradingForm
        DynamicForm { actionType: "T_Grading"; schema: testCase.gradingSchema; controller: null }
    }

    function formWithSlot(component) {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byField("T_Grading", "rows", component)
        return createTemporaryObject(gradingForm, testCase, { slotRegistry: registry })
    }

    // ── without a slot: unchanged ────────────────────────────────────────────

    function test_without_a_slot_the_collection_stays_unrepresentable() {
        const form = createTemporaryObject(gradingForm, testCase)
        const rows = form.fieldByName["rows"]
        compare(rows.isObjectArray, true)
        compare(rows.claimedBySlot, false)
        verify(rows.unrepresentable !== "")
        compare(form.ready, false)
    }

    function test_a_collection_of_strings_is_not_an_object_array() {
        const form = createTemporaryObject(gradingForm, testCase)
        const tags = form.fieldByName["tags"]
        compare(tags.isObjectArray, false)
        compare(tags.itemFields.length, 0)
        compare(tags.unrepresentable, "")
    }

    // ── the element schema a grid draws its columns from ─────────────────────

    function test_the_slot_receives_the_element_members_in_order() {
        const form = formWithSlot(gridSlot)
        const slot = findChild(form, "gridSlot")
        verify(slot !== null)
        compare(slot.field.name, "rows")
        compare(slot.field.claimedBySlot, true)
        compare(slot.field.unrepresentable, "")
        const columns = slot.field.itemFields
        compare(columns.length, 4)
        compare(columns[0].name, "sieve")
        compare(columns[1].name, "passing")
        compare(columns[2].name, "note")
        compare(columns[3].name, "order")
    }

    function test_each_column_carries_its_label_unit_decimals_and_flags() {
        const form = formWithSlot(gridSlot)
        const columns = findChild(form, "gridSlot").field.itemFields
        compare(columns[0].label, "Sieve")
        compare(columns[0].unit, "mm")
        compare(columns[0].isNumber, true)
        compare(columns[0].required, true)
        compare(columns[1].unit, "%")
        compare(columns[1].isQuantity, true)
        compare(columns[1].decimals, 1)
        compare(columns[2].required, false)
        compare(columns[3].readOnly, true)
        compare(columns[3].isInteger, true)
    }

    // ── writing the rows ─────────────────────────────────────────────────────

    function test_rows_encode_with_each_members_own_encoder() {
        const form = formWithSlot(gridSlot)
        const slot = findChild(form, "gridSlot")
        slot.setRows([{ sieve: "31.5", passing: "100.0" }, { sieve: "0.063", passing: "4.2", note: "fines" }])
        compare(form.ready, true)
        compare(form.previewLine,
                '{"rows":[{"sieve":31.5,"passing":{"num":1000,"den":10,"dp":1}},'
                + '{"sieve":0.063,"passing":{"num":42,"den":10,"dp":1},"note":"fines"}]}')
    }

    function test_an_empty_collection_is_a_valid_value() {
        const form = formWithSlot(gridSlot)
        findChild(form, "gridSlot").setRows([])
        compare(form.ready, true)
        compare(form.previewLine, '{"rows":[]}')
    }

    function test_a_blank_required_cell_leaves_the_form_unready() {
        const form = formWithSlot(gridSlot)
        findChild(form, "gridSlot").setRows([{ sieve: "31.5" }])
        compare(form.ready, false)
        compare(form.previewLine, "")
    }

    function test_a_cell_that_does_not_encode_leaves_the_form_unready() {
        const form = formWithSlot(gridSlot)
        const slot = findChild(form, "gridSlot")
        slot.setRows([{ sieve: "abc", passing: "1.0" }])
        compare(form.ready, false)
        // An over-precise Quantity cell is refused, not rounded, as at the top.
        slot.setRows([{ sieve: "1", passing: "1.25" }])
        compare(form.ready, false)
        slot.setRows([{ sieve: "1", passing: "1.2" }])
        compare(form.ready, true)
    }

    function test_a_value_that_is_not_an_array_of_objects_has_no_literal() {
        const form = formWithSlot(gridSlot)
        const slot = findChild(form, "gridSlot")
        slot.setValue('{"sieve":"1"}')
        compare(form.ready, false)
        slot.setValue('[1, 2]')
        compare(form.ready, false)
        slot.setValue('not json')
        compare(form.ready, false)
    }

    function test_non_string_cells_are_read_as_their_text() {
        const form = formWithSlot(gridSlot)
        findChild(form, "gridSlot").setRows([{ sieve: 2.5, passing: "3.0", order: 7 }])
        compare(form.ready, true)
        compare(form.previewLine, '{"rows":[{"sieve":2.5,"passing":{"num":30,"den":10,"dp":1},"order":7}]}')
    }

    // ── reading the rows back ────────────────────────────────────────────────

    function test_the_slot_reads_back_what_it_wrote() {
        const form = formWithSlot(gridSlot)
        const slot = findChild(form, "gridSlot")
        const written = [{ sieve: "8", passing: "55.0" }]
        slot.setRows(written)
        compare(slot.rows.length, 1)
        compare(slot.rows[0].sieve, "8")
        compare(slot.fieldText, JSON.stringify(written))
        verify(slot.form === form)
    }

    function test_a_reset_reaches_the_slot() {
        const form = formWithSlot(gridSlot)
        const slot = findChild(form, "gridSlot")
        slot.setRows([{ sieve: "8", passing: "55.0" }])
        form.resetFields()
        compare(slot.rows.length, 0)
        compare(slot.fieldText, "")
        compare(form.ready, false)
    }

    function test_a_programmatic_write_reaches_the_slot() {
        const form = formWithSlot(gridSlot)
        const slot = findChild(form, "gridSlot")
        form.setFieldValue("rows", JSON.stringify([{ sieve: "1", passing: "2.0" }, { sieve: "2", passing: "3.0" }]))
        compare(slot.rows.length, 2)
        compare(slot.rows[1].sieve, "2")
    }

    function test_a_prefill_through_the_default_control_reaches_a_scalar_slot() {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byField("T_Label", "label", textSlot)
        const form = createTemporaryObject(labelForm, testCase, { slotRegistry: registry })
        const slot = findChild(form, "textSlot")
        // What CollectionView's row-open prefill does: write the hidden
        // default control's text.
        findChild(form, "field_label").text = "Sample 7"
        compare(slot.fieldText, "Sample 7")
        compare(form.previewLine, '{"label":"Sample 7"}')
    }

    function test_a_slot_declaring_only_the_mandatory_contract_still_works() {
        const form = formWithSlot(minimalSlot)
        const slot = findChild(form, "minimalSlot")
        verify(slot !== null)
        slot.setValue(JSON.stringify([{ sieve: "1", passing: "2.0" }]))
        compare(form.ready, true)
    }

    function test_a_slot_registered_after_the_form_is_built_is_picked_up() {
        const registry = createTemporaryObject(registryComponent, testCase)
        const form = createTemporaryObject(gradingForm, testCase, { slotRegistry: registry })
        verify(form.fieldByName["rows"].unrepresentable !== "")
        registry.byField("T_Grading", "rows", gridSlot)
        compare(form.fieldByName["rows"].unrepresentable, "")
        tryVerify(function () { return findChild(form, "gridSlot") !== null })
    }
}
