// SPDX-License-Identifier: Apache-2.0
//
// `"x-blankAs": "empty"` (FieldMeta::blankAs): a string field cleared after a
// prefill or an edit submits "" instead of being omitted.
//
// The motivating flow is an edit form over a stored record whose model reads
// an absent `std::optional<std::string>` as "leave unchanged" and "" as
// "clear". Without the key, deleting a prefilled remark omits the member and
// the stored text survives. Every case asserts the body the controller gets.

pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormBlankAs"
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

    // `struct EditSample { std::int64_t id; std::optional<std::string> remark;
    //   std::optional<std::string> operatorName; std::optional<std::string> plain;
    //   std::optional<double> weight; std::string title; }` with
    // blankAs = Empty on remark, operatorName, weight (ignored: not a string)
    // and title (required, so the ordinary gate applies).
    property var editSchema: ({
        type: "object",
        properties: {
            id: { type: "integer", "x-order": 0, title: "Id" },
            remark: { type: ["string", "null"], "x-order": 1, title: "Remark", "x-blankAs": "empty" },
            operatorName: { anyOf: [{ type: "string" }, { type: "null" }], "x-order": 2, title: "Operator",
                            "x-blankAs": "empty" },
            plain: { type: ["string", "null"], "x-order": 3, title: "Plain" },
            weight: { type: ["number", "null"], "x-order": 4, title: "Weight", "x-blankAs": "empty" },
            title: { type: "string", "x-order": 5, title: "Title", "x-blankAs": "empty" }
        },
        required: ["id", "title"]
    })

    Component {
        id: editForm
        DynamicForm { actionType: "T_EditSample"; schema: testCase.editSchema; controller: mockController }
    }

    Component {
        id: textSlot
        Item {
            objectName: "remarkSlot"
            property var field
            property var setValue
        }
    }

    Component {
        id: registryComponent
        SlotRegistry {}
    }

    function stored() {
        return { id: 7, remark: "abc", operatorName: "Ann", plain: "keep", weight: 1.5, title: "T" }
    }

    function test_the_descriptor_flags_only_string_fields() {
        const form = createTemporaryObject(editForm, testCase)
        compare(form.fieldByName["remark"].blankAsEmpty, true)
        compare(form.fieldByName["operatorName"].blankAsEmpty, true)
        compare(form.fieldByName["plain"].blankAsEmpty, false)
        compare(form.fieldByName["weight"].blankAsEmpty, false)
        compare(form.fieldByName["title"].blankAsEmpty, true)
    }

    function test_a_prefilled_field_the_user_clears_submits_an_empty_string() {
        const form = createTemporaryObject(editForm, testCase)
        verify(form.prefill(stored()))
        compare(mockController.submitCount, 0)
        findChild(form, "field_remark").text = ""
        compare(mockController.lastBody,
                '{"id":7,"remark":"","operatorName":"Ann","plain":"keep","weight":1.5,"title":"T"}')
    }

    function test_a_field_without_the_key_is_still_omitted_when_cleared() {
        const form = createTemporaryObject(editForm, testCase)
        verify(form.prefill(stored()))
        findChild(form, "field_plain").text = ""
        compare(mockController.lastBody, '{"id":7,"remark":"abc","operatorName":"Ann","weight":1.5,"title":"T"}')
    }

    function test_an_untouched_never_set_field_is_omitted() {
        const form = createTemporaryObject(editForm, testCase)
        findChild(form, "field_id").text = "7"
        findChild(form, "field_title").text = "T"
        compare(mockController.lastBody, '{"id":7,"title":"T"}')
    }

    function test_a_field_typed_into_and_cleared_submits_an_empty_string() {
        const form = createTemporaryObject(editForm, testCase)
        findChild(form, "field_id").text = "7"
        findChild(form, "field_title").text = "T"
        findChild(form, "field_operatorName").text = "Bo"
        findChild(form, "field_operatorName").text = ""
        compare(mockController.lastBody, '{"id":7,"operatorName":"","title":"T"}')
    }

    function test_a_stored_empty_string_round_trips() {
        const form = createTemporaryObject(editForm, testCase)
        verify(form.prefillFromJson('{"id":7,"remark":"","title":"T"}'))
        compare(form.ready, true)
        form.submit()
        compare(mockController.lastBody, '{"id":7,"remark":"","title":"T"}')
    }

    function test_a_stored_null_is_not_engaged() {
        const form = createTemporaryObject(editForm, testCase)
        verify(form.prefillFromJson('{"id":7,"remark":null,"title":"T"}'))
        form.submit()
        compare(mockController.lastBody, '{"id":7,"title":"T"}')
    }

    function test_a_non_string_field_ignores_the_key() {
        const form = createTemporaryObject(editForm, testCase)
        verify(form.prefill(stored()))
        findChild(form, "field_weight").text = ""
        compare(mockController.lastBody,
                '{"id":7,"remark":"abc","operatorName":"Ann","plain":"keep","title":"T"}')
    }

    function test_a_required_field_left_blank_is_still_unfilled() {
        const form = createTemporaryObject(editForm, testCase)
        verify(form.prefill(stored()))
        const before = mockController.submitCount
        findChild(form, "field_title").text = ""
        compare(form.ready, false)
        compare(form.previewLine, "")
        compare(mockController.submitCount, before)
    }

    function test_a_reset_disengages_the_field() {
        const form = createTemporaryObject(editForm, testCase)
        verify(form.prefill(stored()))
        form.resetFields()
        findChild(form, "field_id").text = "8"
        findChild(form, "field_title").text = "U"
        compare(mockController.lastBody, '{"id":8,"title":"U"}')
    }

    function test_a_new_prefill_disengages_what_the_previous_one_engaged() {
        const form = createTemporaryObject(editForm, testCase)
        verify(form.prefill(stored()))
        verify(form.prefill({ id: 9, title: "V" }))
        form.submit()
        compare(mockController.lastBody, '{"id":9,"title":"V"}')
    }

    function test_a_slot_clearing_the_field_behaves_the_same() {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byField("T_EditSample", "remark", textSlot)
        const form = createTemporaryObject(editForm, testCase, { slotRegistry: registry })
        verify(form.prefill(stored()))
        findChild(form, "remarkSlot").setValue("")
        compare(mockController.lastBody,
                '{"id":7,"remark":"","operatorName":"Ann","plain":"keep","weight":1.5,"title":"T"}')
    }
}
