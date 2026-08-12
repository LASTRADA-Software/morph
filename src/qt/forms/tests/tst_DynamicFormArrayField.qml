// SPDX-License-Identifier: Apache-2.0
//
// Covers DynamicForm's control for a JSON "array"-typed field (docs/spec/
// forms/forms.md, "Array fields"): a comma-separated-with-validation text
// control that emits a genuine JSON array literal, not a stringified one.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormArrayField"
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
            replyReceived(actionType, true, JSON.stringify({ok: true}))
        }

        function fetchOptions(optionsAction) {
            optionsReceived(optionsAction, true, "[]")
        }
    }

    property var arraySchema: ({
        properties: {
            name: { type: "string", "x-order": 0 },
            tags: { type: "array", items: { type: "string" }, "x-order": 1 }
        },
        required: ["name"]
    })

    Component {
        id: formComponent
        DynamicForm {
            actionType: "CFR_TagRoom"
            schema: testCase.arraySchema
            controller: mockController
        }
    }

    function test_array_field_renders_its_own_control_not_the_plain_text_field() {
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)
        var arrayControl = findChild(form, "field_tags")
        verify(arrayControl !== null)
    }

    function test_empty_array_field_is_not_required_by_default() {
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)
        findChild(form, "field_name").text = "Alice"
        compare(form.ready, true)  // tags is optional (not in `required`) and blank
    }

    function test_comma_separated_entry_encodes_as_a_json_array_of_strings() {
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)
        findChild(form, "field_name").text = "Alice"
        findChild(form, "field_tags").text = "red, green, blue"
        compare(form.ready, true)

        var parsed = JSON.parse(form.previewLine)
        verify(Array.isArray(parsed.tags))
        compare(parsed.tags.length, 3)
        compare(parsed.tags[0], "red")
        compare(parsed.tags[1], "green")
        compare(parsed.tags[2], "blue")
    }

    function test_blank_entries_and_surrounding_whitespace_are_dropped() {
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)
        findChild(form, "field_name").text = "Alice"
        findChild(form, "field_tags").text = "  red ,, green ,   "
        compare(form.ready, true)

        var parsed = JSON.parse(form.previewLine)
        compare(parsed.tags.length, 2)
        compare(parsed.tags[0], "red")
        compare(parsed.tags[1], "green")
    }

    function test_single_item_still_encodes_as_an_array() {
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)
        findChild(form, "field_name").text = "Alice"
        findChild(form, "field_tags").text = "solo"
        compare(form.ready, true)

        var parsed = JSON.parse(form.previewLine)
        verify(Array.isArray(parsed.tags))
        compare(parsed.tags.length, 1)
        compare(parsed.tags[0], "solo")
    }

    function test_fully_blank_array_field_is_omitted_from_the_preview_when_optional() {
        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)
        findChild(form, "field_name").text = "Alice"
        compare(form.ready, true)
        var parsed = JSON.parse(form.previewLine)
        verify(parsed.tags === undefined)
    }

    // --- required array field ------------------------------------------

    property var requiredArraySchema: ({
        properties: {
            name: { type: "string", "x-order": 0 },
            tags: { type: "array", items: { type: "string" }, "x-order": 1 }
        },
        required: ["name", "tags"]
    })

    Component {
        id: requiredFormComponent
        DynamicForm {
            actionType: "CFR_TagRoom"
            schema: testCase.requiredArraySchema
            controller: mockController
        }
    }

    function test_required_array_field_blocks_submit_until_engaged() {
        var form = createTemporaryObject(requiredFormComponent, testCase)
        verify(form !== null)
        findChild(form, "field_name").text = "Alice"
        compare(form.ready, false)  // tags required, still blank

        findChild(form, "field_tags").text = "red"
        compare(form.ready, true)
    }

    // A comma-only entry is non-blank text (the field is "engaged" by the
    // same trim-then-check-empty rule every other field type uses) even
    // though every individual entry is dropped -- it encodes to a genuine
    // empty array [], not null, and therefore satisfies a `required` array
    // field (docs/spec/forms/forms.md, "Array fields").
    function test_comma_only_entry_is_engaged_and_encodes_as_an_empty_array() {
        var form = createTemporaryObject(requiredFormComponent, testCase)
        verify(form !== null)
        findChild(form, "field_name").text = "Alice"
        findChild(form, "field_tags").text = " , , "
        compare(form.ready, true)

        var parsed = JSON.parse(form.previewLine)
        verify(Array.isArray(parsed.tags))
        compare(parsed.tags.length, 0)
    }
}
