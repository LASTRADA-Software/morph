// SPDX-License-Identifier: Apache-2.0
//
// A nested object member (a plain or `std::optional` struct member) drawn by
// a host slot.
//
// Without a slot the renderer has no encoding for such a member and reports it
// unrepresentable (tst_DynamicFormNestedAggregate.qml pins that, unchanged).
// With one, the slot is handed the object's member descriptors -- recursively,
// so a member that is itself an object carries its own -- writes the value as
// a `{member: cellText | nestedValue}` object, and the form encodes every leaf
// cell with the encoder the same member would get at the top level. The cases
// assert the body the controller receives, because "the slot wrote something"
// is not the claim; "the payload satisfies the schema" is.

pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormObjectSlot"
    visible: true

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)

        property int submitCount: 0
        property string lastBody: ""

        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            lastBody = bodyJson
            replyReceived(actionType, true, JSON.stringify({ ok: true }))
        }

        function fetchOptions(optionsAction) {
            optionsReceived(optionsAction, true, "[]")
        }
    }

    function init() {
        mockController.submitCount = 0
        mockController.lastBody = ""
    }

    // The document schemaJson<A>() emits for
    //
    //   struct IgnitionSpecimen { double massOfContainer = 0.0;
    //       double massOfContainerAndSampleBeforeIgnition = 0.0;
    //       std::optional<double> readoutBinderContent; int testTemperature = 0; };
    //   struct BinderIgnitionSection { IgnitionMethod method;
    //       IgnitionSpecimen specimen = {}; double calibrationFactor = 0.0;
    //       bool driedSample = false; std::optional<int> testedByNr; };
    //
    // with FieldMeta units and display decimals on the specimen's masses. The
    // specimen type is used once, so glaze inlines it into the property.
    property var ignitionSchema: ({
        type: "object",
        "$defs": {
            "double": { type: "number", minimum: -1.7976931348623157e+308, maximum: 1.7976931348623157e+308 },
            "int32_t": { type: "integer", minimum: -2147483648, maximum: 2147483647 }
        },
        properties: {
            method: {
                type: "string",
                oneOf: [{ title: "Furnace", "const": "Furnace" }, { title: "Infrared", "const": "Infrared" }],
                "x-order": 0, title: "Method"
            },
            specimen: {
                type: "object",
                properties: {
                    massOfContainer: {
                        "$ref": "#/$defs/double", "x-order": 0, title: "Container",
                        ExtUnits: { unitAscii: "g", unitUnicode: "g" }, "x-displayDecimals": 1
                    },
                    massOfContainerAndSampleBeforeIgnition: {
                        "$ref": "#/$defs/double", "x-order": 1, title: "Container + sample",
                        ExtUnits: { unitAscii: "g", unitUnicode: "g" }, "x-displayDecimals": 1
                    },
                    readoutBinderContent: {
                        anyOf: [{ "$ref": "#/$defs/double" }, { type: "null" }],
                        "x-order": 2, title: "Readout binder content",
                        ExtUnits: { unitAscii: "pct", unitUnicode: "%" }, "x-displayDecimals": 2
                    },
                    testTemperature: { "$ref": "#/$defs/int32_t", "x-order": 3, title: "Test Temperature" }
                },
                additionalProperties: false,
                required: ["massOfContainer", "massOfContainerAndSampleBeforeIgnition", "testTemperature"],
                "x-order": 1, title: "Specimen"
            },
            calibrationFactor: { "$ref": "#/$defs/double", "x-order": 2, title: "Calibration Factor" },
            driedSample: { type: "boolean", "x-order": 3, title: "Dried Sample" },
            testedByNr: { anyOf: [{ "$ref": "#/$defs/int32_t" }, { type: "null" }], "x-order": 4, title: "Tested By Nr" }
        },
        additionalProperties: false,
        required: ["method", "specimen", "calibrationFactor", "driedSample"]
    })

    // The document schemaJson<A>() emits for
    //
    //   struct PycnometerDetermination { std::optional<double> massPycnometerEmpty;
    //       double massPycnometerAndSample = 0.0; bool excluded = false;
    //       std::optional<double> computedDensity; };
    //   struct PycnometerTestData { bool useSpecificGravity = false;
    //       std::optional<double> testLiquidTemperature;
    //       std::optional<std::string> testLiquidName;
    //       std::optional<PycnometerDetermination> determination1;
    //       std::optional<PycnometerDetermination> determination2; };
    //   struct MaxDensitySection { PycnometerTestData data; };
    //
    // PycnometerDetermination is used twice, so it sits under `$defs` and each
    // optional member is the nullable `anyOf` over its `$ref`.
    property var densitySchema: ({
        type: "object",
        "$defs": {
            "double": { type: "number", minimum: -1.7976931348623157e+308, maximum: 1.7976931348623157e+308 },
            "lab::PycnometerDetermination": {
                type: "object",
                properties: {
                    massPycnometerEmpty: {
                        anyOf: [{ "$ref": "#/$defs/double" }, { type: "null" }],
                        "x-order": 0, title: "Pycnometer empty",
                        ExtUnits: { unitAscii: "g", unitUnicode: "g" }, "x-displayDecimals": 2
                    },
                    massPycnometerAndSample: {
                        "$ref": "#/$defs/double", "x-order": 1, title: "Pycnometer + sample",
                        ExtUnits: { unitAscii: "g", unitUnicode: "g" }, "x-displayDecimals": 2
                    },
                    excluded: { type: "boolean", "x-order": 2, title: "Excluded" },
                    computedDensity: {
                        anyOf: [{ "$ref": "#/$defs/double" }, { type: "null" }],
                        "x-order": 3, title: "Computed Density", "x-readonly": true
                    }
                },
                additionalProperties: false,
                required: ["massPycnometerAndSample", "excluded"]
            }
        },
        properties: {
            data: {
                type: "object",
                properties: {
                    useSpecificGravity: { type: "boolean", "x-order": 0, title: "Use Specific Gravity" },
                    testLiquidTemperature: {
                        anyOf: [{ "$ref": "#/$defs/double" }, { type: "null" }],
                        "x-order": 1, title: "Test Liquid Temperature",
                        ExtUnits: { unitAscii: "degC", unitUnicode: "°C" }, "x-displayDecimals": 1
                    },
                    testLiquidName: { type: ["string", "null"], "x-order": 2, title: "Test Liquid Name" },
                    determination1: {
                        anyOf: [{ "$ref": "#/$defs/lab::PycnometerDetermination" }, { type: "null" }],
                        "x-order": 3, title: "Determination 1"
                    },
                    determination2: {
                        anyOf: [{ "$ref": "#/$defs/lab::PycnometerDetermination" }, { type: "null" }],
                        "x-order": 4, title: "Determination 2"
                    }
                },
                additionalProperties: false,
                required: ["useSpecificGravity"],
                "x-order": 0, title: "Data"
            }
        },
        additionalProperties: false,
        required: ["data"]
    })

    // A collection of objects inside a nested object: the row encoder is
    // reused for it. `struct Row { double sieve; Quantity<pct, 1> passing; };
    // struct Grading { std::string label; std::vector<Row> rows; };
    // struct GradingSection { Grading grading; };`
    property var gradingSchema: ({
        type: "object",
        "$defs": {
            "Row": {
                type: "object",
                properties: {
                    sieve: { type: "number", "x-order": 0, title: "Sieve" },
                    passing: {
                        type: ["object", "null"],
                        properties: { num: { type: "integer" }, den: { type: "integer" }, dp: { type: "integer" } },
                        ExtUnits: { unitAscii: "pct", unitUnicode: "%" }, "x-decimalPlaces": 1,
                        "x-order": 1, title: "Passing"
                    }
                },
                required: ["sieve", "passing"]
            }
        },
        properties: {
            grading: {
                type: "object",
                properties: {
                    label: { type: "string", "x-order": 0, title: "Label" },
                    rows: { type: "array", items: { "$ref": "#/$defs/Row" }, "x-order": 1, title: "Rows" }
                },
                required: ["label", "rows"],
                "x-order": 0, title: "Grading"
            }
        },
        required: ["grading"]
    })

    // A self-referential type below a claimed object: its description stops
    // at the first repetition of the type instead of looping.
    property var treeSchema: ({
        type: "object",
        "$defs": {
            "TreeNode": {
                type: "object",
                properties: {
                    name: { type: "string", "x-order": 0, title: "Name" },
                    child: { anyOf: [{ "$ref": "#/$defs/TreeNode" }, { type: "null" }], "x-order": 1, title: "Child" }
                },
                required: ["name"]
            }
        },
        properties: { root: { "$ref": "#/$defs/TreeNode", "x-order": 0, title: "Root" } },
        required: ["root"]
    })

    // A sub-form slot: it declares the whole optional contract.
    Component {
        id: objectSlot
        Item {
            objectName: "objectSlot"
            property var field
            property var setValue
            property var objectValue: ({})
            property var setObject
            property string fieldText
            property var form
        }
    }

    // A slot declaring only the two mandatory members.
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

    Component {
        id: ignitionForm
        DynamicForm { actionType: "T_Ignition"; schema: testCase.ignitionSchema; controller: mockController }
    }

    Component {
        id: densityForm
        DynamicForm { actionType: "T_Density"; schema: testCase.densitySchema; controller: mockController }
    }

    Component {
        id: gradingForm
        DynamicForm { actionType: "T_Grading"; schema: testCase.gradingSchema; controller: mockController }
    }

    Component {
        id: treeForm
        DynamicForm { actionType: "T_Tree"; schema: testCase.treeSchema; controller: mockController }
    }

    function formWithSlot(formComponent, action, member, slotComponent, properties) {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byField(action, member, slotComponent)
        return createTemporaryObject(formComponent, testCase,
                                     Object.assign({ slotRegistry: registry }, properties || {}))
    }

    function ignitionWithSlot(properties) {
        return formWithSlot(ignitionForm, "T_Ignition", "specimen", objectSlot, properties)
    }

    function densityWithSlot(properties) {
        return formWithSlot(densityForm, "T_Density", "data", objectSlot, properties)
    }

    // The top-level scalars of the ignition section, through their controls.
    function fillIgnitionScalars(form) {
        form.setFieldValue("method", '"Furnace"')
        form.setFieldValue("calibrationFactor", "0.25")
        form.setFieldValue("driedSample", "true")
    }

    // ── without a slot: unchanged ────────────────────────────────────────────

    function test_without_a_slot_the_object_stays_unrepresentable() {
        const form = createTemporaryObject(ignitionForm, testCase)
        const specimen = form.fieldByName["specimen"]
        compare(specimen.isObject, true)
        compare(specimen.kind, "object")
        compare(specimen.claimedBySlot, false)
        verify(specimen.unrepresentable !== "")
        fillIgnitionScalars(form)
        findChild(form, "field_specimen").text = '{"massOfContainer":"1"}'
        compare(form.ready, false)
        verify(form.unrepresentableReason.indexOf("specimen: ") === 0)
        compare(mockController.submitCount, 0)
    }

    // A Quantity is an object with properties too; its own control claims it.
    function test_a_quantity_is_not_a_nested_object() {
        const form = createTemporaryObject(gradingForm, testCase)
        const passing = form.fieldByName["grading"].objectFields[1].itemFields[1]
        compare(passing.isQuantity, true)
        compare(passing.isObject, false)
        compare(passing.objectFields.length, 0)
    }

    // ── the claim ────────────────────────────────────────────────────────────

    function test_a_slot_claims_the_object_and_makes_it_representable() {
        const form = ignitionWithSlot()
        const slot = findChild(form, "objectSlot")
        verify(slot !== null)
        compare(slot.field.name, "specimen")
        compare(slot.field.isObject, true)
        compare(slot.field.claimedBySlot, true)
        compare(slot.field.unrepresentable, "")
        verify(slot.form === form)
    }

    function test_a_slot_registered_by_kind_claims_the_object() {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byKind("object", objectSlot)
        const form = createTemporaryObject(densityForm, testCase, { slotRegistry: registry })
        const slot = findChild(form, "objectSlot")
        verify(slot !== null)
        compare(slot.field.name, "data")
        compare(slot.field.claimedBySlot, true)
        compare(slot.field.unrepresentable, "")
    }

    function test_a_slot_registered_after_the_form_is_built_is_picked_up() {
        const registry = createTemporaryObject(registryComponent, testCase)
        const form = createTemporaryObject(ignitionForm, testCase, { slotRegistry: registry })
        verify(form.fieldByName["specimen"].unrepresentable !== "")
        registry.byField("T_Ignition", "specimen", objectSlot)
        compare(form.fieldByName["specimen"].unrepresentable, "")
        tryVerify(function () { return findChild(form, "objectSlot") !== null })
    }

    // ── the descriptors a sub-form draws from ────────────────────────────────

    function test_object_fields_carry_order_labels_units_and_decimals() {
        const form = ignitionWithSlot()
        const members = findChild(form, "objectSlot").field.objectFields
        compare(members.length, 4)
        compare(members[0].name, "massOfContainer")
        compare(members[0].label, "Container")
        compare(members[0].unit, "g")
        compare(members[0].isNumber, true)
        compare(members[0].decimals, 1)
        compare(members[0].decimalsDeclared, true)
        compare(members[0].required, true)
        compare(members[1].name, "massOfContainerAndSampleBeforeIgnition")
        compare(members[2].name, "readoutBinderContent")
        compare(members[2].unit, "%")
        compare(members[2].decimals, 2)
        compare(members[2].required, false)
        compare(members[3].isInteger, true)
        compare(members[3].required, true)
    }

    function test_object_fields_recurse_into_optional_nested_objects() {
        const form = densityWithSlot()
        const data = findChild(form, "objectSlot").field
        compare(data.objectFields.map(function (m) { return m.name }).join(","),
                "useSpecificGravity,testLiquidTemperature,testLiquidName,determination1,determination2")
        const determination = data.objectFields[3]
        compare(determination.isObject, true)
        compare(determination.required, false)
        compare(determination.unrepresentable, "")
        compare(determination.label, "Determination 1")
        compare(determination.objectFields.length, 4)
        const sample = determination.objectFields[1]
        compare(sample.name, "massPycnometerAndSample")
        compare(sample.label, "Pycnometer + sample")
        compare(sample.unit, "g")
        compare(sample.decimals, 2)
        compare(sample.required, true)
        compare(determination.objectFields[3].readOnly, true)
        // Both uses of the shared $defs entry are described alike.
        compare(data.objectFields[4].objectFields.length, 4)
    }

    function test_a_self_referential_type_is_described_once_and_stops() {
        const form = formWithSlot(treeForm, "T_Tree", "root", objectSlot)
        const root = form.fieldByName["root"]
        compare(root.objectFields.length, 2)
        const child = root.objectFields[1]
        compare(child.isObject, true)
        compare(child.objectFields.length, 0)
        verify(child.unrepresentable !== "")
        // The repeated type has no encoding, but an absent optional one is fine.
        findChild(form, "objectSlot").setObject({ name: "top" })
        compare(mockController.lastBody, '{"root":{"name":"top"}}')
        findChild(form, "objectSlot").setObject({ name: "top", child: { name: "leaf" } })
        compare(form.ready, false)
    }

    // ── encoding ─────────────────────────────────────────────────────────────

    function test_the_binder_ignition_section_encodes_member_by_member() {
        const form = ignitionWithSlot()
        fillIgnitionScalars(form)
        compare(form.ready, false)  // the required specimen is still blank
        findChild(form, "objectSlot").setObject({
            massOfContainer: "512.3", massOfContainerAndSampleBeforeIgnition: "2012.8",
            readoutBinderContent: "5.25", testTemperature: "538"
        })
        compare(form.ready, true)
        compare(mockController.lastBody,
                '{"method":"Furnace","specimen":{"massOfContainer":512.3,'
                + '"massOfContainerAndSampleBeforeIgnition":2012.8,"readoutBinderContent":5.25,'
                + '"testTemperature":538},"calibrationFactor":0.25,"driedSample":true}')
    }

    function test_the_max_density_section_encodes_two_levels_deep() {
        const form = densityWithSlot()
        findChild(form, "objectSlot").setObject({
            useSpecificGravity: "false", testLiquidTemperature: "25.0", testLiquidName: "water",
            determination1: { massPycnometerEmpty: "1450.10", massPycnometerAndSample: "3450.25", excluded: "false" }
        })
        compare(form.ready, true)
        compare(mockController.lastBody,
                '{"data":{"useSpecificGravity":false,"testLiquidTemperature":25.0,"testLiquidName":"water",'
                + '"determination1":{"massPycnometerEmpty":1450.10,"massPycnometerAndSample":3450.25,'
                + '"excluded":false}}}')
    }

    function test_blank_optional_leaves_and_objects_are_omitted() {
        const form = densityWithSlot()
        findChild(form, "objectSlot").setObject({
            useSpecificGravity: "true", testLiquidTemperature: "", testLiquidName: "  ",
            determination1: {}, determination2: { massPycnometerEmpty: "", excluded: "" }
        })
        compare(form.ready, true)
        compare(mockController.lastBody, '{"data":{"useSpecificGravity":true}}')
    }

    function test_a_blank_required_leaf_leaves_the_form_unready() {
        const form = ignitionWithSlot()
        fillIgnitionScalars(form)
        const slot = findChild(form, "objectSlot")
        slot.setObject({ massOfContainer: "1.0", massOfContainerAndSampleBeforeIgnition: "2.0" })
        compare(form.ready, false)
        compare(form.previewLine, "")
        compare(mockController.submitCount, 0)
    }

    function test_a_partly_filled_optional_object_needs_its_required_members() {
        const form = densityWithSlot()
        const slot = findChild(form, "objectSlot")
        slot.setObject({ useSpecificGravity: "false", determination2: { massPycnometerEmpty: "10.00" } })
        compare(form.ready, false)
        slot.setObject({ useSpecificGravity: "false",
                         determination2: { massPycnometerEmpty: "10.00", massPycnometerAndSample: "20.00",
                                           excluded: "true" } })
        compare(form.ready, true)
        compare(mockController.lastBody,
                '{"data":{"useSpecificGravity":false,"determination2":{"massPycnometerEmpty":10.00,'
                + '"massPycnometerAndSample":20.00,"excluded":true}}}')
    }

    function test_a_blank_required_object_leaves_the_form_unready() {
        const form = densityWithSlot()
        const slot = findChild(form, "objectSlot")
        slot.setObject({})
        compare(form.ready, false)
        slot.setObject({ testLiquidName: "" })
        compare(form.ready, false)
        compare(mockController.submitCount, 0)
    }

    function test_a_cell_that_does_not_encode_leaves_the_form_unready() {
        const form = ignitionWithSlot()
        fillIgnitionScalars(form)
        const slot = findChild(form, "objectSlot")
        const specimen = { massOfContainerAndSampleBeforeIgnition: "2.0", testTemperature: "538" }
        // More fraction digits than x-displayDecimals is refused, not rounded.
        slot.setObject(Object.assign({ massOfContainer: "1.25" }, specimen))
        compare(form.ready, false)
        slot.setObject(Object.assign({ massOfContainer: "abc" }, specimen))
        compare(form.ready, false)
        slot.setObject(Object.assign({ massOfContainer: "1.2" }, specimen, { testTemperature: "1.5" }))
        compare(form.ready, false)
        slot.setObject(Object.assign({ massOfContainer: "1.2" }, specimen))
        compare(form.ready, true)
    }

    function test_cells_are_read_in_the_display_locale() {
        const form = ignitionWithSlot({ displayLocale: "de_DE" })
        form.setFieldValue("method", '"Furnace"')
        form.setFieldValue("calibrationFactor", "0,25")
        form.setFieldValue("driedSample", "true")
        findChild(form, "objectSlot").setObject({
            massOfContainer: "1.512,3", massOfContainerAndSampleBeforeIgnition: "2012,8",
            readoutBinderContent: "5,25", testTemperature: "538"
        })
        compare(form.ready, true)
        compare(mockController.lastBody,
                '{"method":"Furnace","specimen":{"massOfContainer":1512.3,'
                + '"massOfContainerAndSampleBeforeIgnition":2012.8,"readoutBinderContent":5.25,'
                + '"testTemperature":538},"calibrationFactor":0.25,"driedSample":true}')
    }

    function test_a_value_that_is_not_an_object_has_no_literal() {
        const form = ignitionWithSlot()
        fillIgnitionScalars(form)
        const slot = findChild(form, "objectSlot")
        slot.setValue("[1, 2]")
        compare(form.ready, false)
        slot.setValue("not json")
        compare(form.ready, false)
        slot.setValue('"text"')
        compare(form.ready, false)
        compare(mockController.submitCount, 0)
    }

    function test_rows_inside_a_claimed_object_reuse_the_row_encoder() {
        const form = formWithSlot(gradingForm, "T_Grading", "grading", objectSlot)
        const grading = form.fieldByName["grading"]
        compare(grading.objectFields[1].isObjectArray, true)
        compare(grading.objectFields[1].itemFields.length, 2)
        compare(grading.objectFields[1].unrepresentable, "")
        const slot = findChild(form, "objectSlot")
        slot.setObject({ label: "A", rows: [{ sieve: "8", passing: "55.0" }, { sieve: "2" }] })
        compare(form.ready, false)  // the second row's required cell is blank
        slot.setObject({ label: "A", rows: [{ sieve: "8", passing: "55.0" }] })
        compare(mockController.lastBody,
                '{"grading":{"label":"A","rows":[{"sieve":8,"passing":{"num":550,"den":10,"dp":1}}]}}')
        slot.setObject({ label: "A", rows: [] })
        compare(mockController.lastBody, '{"grading":{"label":"A","rows":[]}}')
    }

    function test_a_slot_declaring_only_the_mandatory_contract_still_works() {
        const form = formWithSlot(densityForm, "T_Density", "data", minimalSlot)
        const slot = findChild(form, "minimalSlot")
        verify(slot !== null)
        slot.setValue(JSON.stringify({ useSpecificGravity: "true" }))
        compare(mockController.lastBody, '{"data":{"useSpecificGravity":true}}')
    }

    // ── reading the value back ───────────────────────────────────────────────

    function test_the_slot_reads_back_what_it_wrote_and_a_reset_clears_it() {
        const form = densityWithSlot()
        const slot = findChild(form, "objectSlot")
        slot.setObject({ useSpecificGravity: "true", determination1: { excluded: "true" } })
        compare(slot.objectValue.useSpecificGravity, "true")
        compare(slot.objectValue.determination1.excluded, "true")
        form.resetFields()
        compare(Object.keys(slot.objectValue).length, 0)
        compare(slot.fieldText, "")
        compare(form.ready, false)
    }

    function test_a_prefill_decodes_into_the_object_value_and_round_trips() {
        const form = densityWithSlot()
        const stored = '{"data":{"useSpecificGravity":false,"testLiquidTemperature":25.5,'
                + '"determination1":{"massPycnometerEmpty":1450.1,"massPycnometerAndSample":3450.25,'
                + '"excluded":true,"computedDensity":2.412}}}'
        verify(form.prefillFromJson(stored))
        compare(mockController.submitCount, 0)  // a prefill never submits
        const slot = findChild(form, "objectSlot")
        compare(slot.objectValue.useSpecificGravity, "false")
        compare(slot.objectValue.testLiquidTemperature, "25.5")
        // Padded to the declared display decimals, as the built-in control is.
        compare(slot.objectValue.determination1.massPycnometerEmpty, "1450.10")
        compare(slot.objectValue.determination1.excluded, "true")
        compare(slot.objectValue.determination2, undefined)
        compare(form.ready, true)
        form.submit()
        compare(mockController.lastBody,
                '{"data":{"useSpecificGravity":false,"testLiquidTemperature":25.5,'
                + '"determination1":{"massPycnometerEmpty":1450.10,"massPycnometerAndSample":3450.25,'
                + '"excluded":true,"computedDensity":2.412}}}')
    }

    function test_a_prefill_uses_the_display_locale_for_cells() {
        const form = ignitionWithSlot({ displayLocale: "de_DE" })
        verify(form.prefill({
            method: "Infrared", calibrationFactor: 0.5, driedSample: false,
            specimen: { massOfContainer: 512.3, massOfContainerAndSampleBeforeIgnition: 2012.8, testTemperature: 538 }
        }))
        const slot = findChild(form, "objectSlot")
        compare(slot.objectValue.massOfContainer, "512,3")
        compare(slot.objectValue.readoutBinderContent, undefined)
        compare(form.ready, true)
        form.submit()
        compare(mockController.lastBody,
                '{"method":"Infrared","specimen":{"massOfContainer":512.3,'
                + '"massOfContainerAndSampleBeforeIgnition":2012.8,"testTemperature":538},'
                + '"calibrationFactor":0.5,"driedSample":false}')
    }
}
