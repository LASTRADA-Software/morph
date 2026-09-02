// SPDX-License-Identifier: Apache-2.0
//
// A C++ `enum class` member is fully described by the schema: glaze emits it
// as a closed `oneOf` of `const` alternatives, each with its own `title`.
// DynamicForm threw all of it away (morph#386) -- resolveProp collapsed the
// `oneOf` to its first non-null branch (the nullable-$ref path morph#189
// added, where the branches differ only in nullability), and no field flag
// ever looked at `const`. The result was a plain TextField for a three-value
// set, and a submit gate that reported `ready` for role = "Emperor".
//
// Every schema below is pasted **verbatim** from
// `morph::forms::schemaJson<A>()` for a shipped action, so a change in what
// glaze emits shows up here as a failure rather than as this file quietly
// testing a shape nothing produces:
//
//   - kanban::SetMemberRole  (Role: Viewer/Member/Manager)
//   - pastebin::CreatePaste  (Visibility, Editability) -- a rung whose GUI
//     binds DynamicForm { actionType: "CreatePaste" } today
//
// The `enum` keyword and the "not a closed set" fallbacks are hand-written:
// glaze emits neither, and the point of both is what a renderer does with a
// schema it did not generate.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormEnumChoice"
    visible: true

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)

        property int submitCount: 0
        property string lastBody: ""
        property var fetched: []

        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            lastBody = bodyJson
        }

        function fetchOptions(optionsAction, body) {
            fetched.push(optionsAction)
            optionsReceived(optionsAction, true, "[]")
        }
    }

    // schemaJson<kanban::SetMemberRole>(), verbatim.
    property var roleSchema: ({
        "type": "object",
        "properties": {
            "principal": { "type": "string", "x-order": 1, "title": "Principal" },
            "projectId": { "$ref": "#/$defs/ProjectId", "x-order": 0, "title": "Project Id" },
            "role": { "type": "string",
                      "oneOf": [{ "title": "Viewer", "const": "Viewer" },
                                { "title": "Member", "const": "Member" },
                                { "title": "Manager", "const": "Manager" }],
                      "x-order": 2, "title": "Role" }
        },
        "additionalProperties": false,
        "$defs": {
            "ProjectId": { "type": ["integer", "null"],
                           "minimum": -9223372036854775808, "maximum": 9223372036854775807,
                           "x-exactMinimum": "-9223372036854775808",
                           "x-exactMaximum": "9223372036854775807" }
        },
        "title": "kanban::SetMemberRole",
        "required": ["projectId", "principal", "role"]
    })

    // The two enum members of schemaJson<pastebin::CreatePaste>(), verbatim,
    // plus its required `content`. The rung binds this action's schema to a
    // DynamicForm today, so these two were free-text boxes in a shipped GUI.
    property var pasteSchema: ({
        "type": "object",
        "properties": {
            "content": { "type": "string", "x-order": 0, "title": "Content" },
            "visibility": { "type": "string",
                            "oneOf": [{ "title": "Public", "const": "Public" },
                                      { "title": "Private", "const": "Private" }],
                            "x-order": 4, "title": "Visibility" },
            "editability": { "type": "string",
                             "oneOf": [{ "title": "Immutable", "const": "Immutable" },
                                       { "title": "Editable", "const": "Editable" }],
                             "x-order": 5, "title": "Editability" }
        },
        "additionalProperties": false,
        "title": "pastebin::CreatePaste",
        "required": ["content"]
    })

    // Hand-written shapes glaze does not emit:
    //   `size`     -- the bare JSON Schema `enum` keyword, over non-strings.
    //   `nullable` -- a closed set with an explicit null branch (optional).
    //   `partial`  -- a `oneOf` in which one branch pins no value. Not a
    //                 closed set; offering a two-of-three list would be worse
    //                 than the text field.
    //   `optI64`   -- the nullable-$ref shape morph#189 added the collapse
    //                 for. It carries no `const` and must keep collapsing.
    property var handWrittenSchema: ({
        "$defs": { "int64_t": { "type": "integer" } },
        "properties": {
            "size": { "enum": [1, 2, 3], "x-order": 0, "title": "Size" },
            "nullable": { "oneOf": [{ "title": "On", "const": "On" },
                                    { "title": "Off", "const": "Off" },
                                    { "type": "null" }],
                          "x-order": 1, "title": "Nullable" },
            "partial": { "type": "string",
                         "oneOf": [{ "title": "A", "const": "A" }, { "type": "string" }],
                         "x-order": 2, "title": "Partial" },
            "optI64": { "anyOf": [{ "$ref": "#/$defs/int64_t" }, { "type": "null" }], "x-order": 3 }
        },
        "required": []
    })

    // A server-fetched Choice, unchanged by any of this: its options are not
    // in the schema, so it must still fetch and must still not be gated on a
    // membership check this renderer cannot make.
    property var choiceSchema: ({
        "properties": {
            "slot": { "type": ["integer", "null"], "x-order": 0, "title": "Slot",
                      "x-optionsAction": "ListSlots", "x-optionValue": "id", "x-optionLabel": "name" }
        },
        "required": []
    })

    Component {
        id: roleForm
        DynamicForm { actionType: "SetMemberRole"; schema: testCase.roleSchema; controller: mockController }
    }

    Component {
        id: pasteForm
        DynamicForm { actionType: "CreatePaste"; schema: testCase.pasteSchema; controller: mockController }
    }

    Component {
        id: handWrittenForm
        DynamicForm { actionType: "HandWritten"; schema: testCase.handWrittenSchema; controller: null }
    }

    Component {
        id: choiceForm
        DynamicForm { actionType: "Choice"; schema: testCase.choiceSchema; controller: mockController }
    }

    function meta(form, name) {
        return form.fieldByName[name]
    }

    // ── the set is read out of the schema ────────────────────────────────────

    function test_a_oneOf_of_consts_is_recognised_as_a_closed_set() {
        var form = createTemporaryObject(roleForm, testCase)
        verify(form !== null)
        var role = meta(form, "role")
        compare(role.isEnum, true)
        // Not a server-fetched Choice: the two are mutually exclusive, and
        // this one needs no options action and no round trip.
        compare(role.isChoice, false)
        compare(role.optionsAction, "")
    }

    function test_the_options_carry_every_alternatives_value_and_its_title() {
        var form = createTemporaryObject(roleForm, testCase)
        var rows = meta(form, "role").enumOptions
        compare(rows.length, 3)
        compare(rows.map(function (r) { return r.label }), ["Viewer", "Member", "Manager"])
        // JSON literals, like a fetched Choice's rows -- a string alternative
        // is therefore quoted.
        compare(rows.map(function (r) { return r.valueJson }), ['"Viewer"', '"Member"', '"Manager"'])
    }

    function test_the_first_branchs_const_does_not_become_the_fields_own_value() {
        var form = createTemporaryObject(roleForm, testCase)
        // Asserted against resolveProp's own output, not the field
        // descriptor: the descriptor never copied `const` across, so
        // checking it there would pass whether or not the collapse still
        // smuggles the first branch's value through.
        var resolved = form.resolveProp(testCase.roleSchema.properties.role)
        compare(resolved["const"], undefined)
        // The outer node's own keys survive the branch merge, as before.
        compare(resolved.title, "Role")
        compare(meta(form, "role").title, "Role")
        compare(meta(form, "role").jsonType, "string")
    }

    function test_the_bare_enum_keyword_is_a_closed_set_too() {
        var form = createTemporaryObject(handWrittenForm, testCase)
        var size = meta(form, "size")
        compare(size.isEnum, true)
        compare(size.enumOptions.map(function (r) { return r.label }), ["1", "2", "3"])
        // Numbers stay bare on the wire; only strings get quoted.
        compare(size.enumOptions.map(function (r) { return r.valueJson }), ["1", "2", "3"])
    }

    function test_a_null_branch_is_not_offered_as_a_choosable_value() {
        var form = createTemporaryObject(handWrittenForm, testCase)
        var nullable = meta(form, "nullable")
        compare(nullable.isEnum, true)
        compare(nullable.enumOptions.map(function (r) { return r.label }), ["On", "Off"])
    }

    // ── what is *not* a closed set keeps its old behaviour ───────────────────

    function test_a_branch_without_a_const_makes_it_not_a_closed_set() {
        var form = createTemporaryObject(handWrittenForm, testCase)
        compare(meta(form, "partial").isEnum, false)
        compare(meta(form, "partial").enumOptions.length, 0)
        verify(findChild(form, "field_partial").placeholderText !== undefined)
    }

    function test_the_nullable_ref_anyOf_still_collapses_to_its_typed_branch() {
        // morph#189's shape. Its branches carry no `const`, so it is not a
        // closed set and must still be typed by T rather than drawn as a
        // picker over nothing.
        var form = createTemporaryObject(handWrittenForm, testCase)
        var optI64 = meta(form, "optI64")
        compare(optI64.isEnum, false)
        compare(optI64.isInteger, true)
        findChild(form, "field_optI64").text = "9223372036854775807"
        verify(form.previewLine.indexOf('"optI64":9223372036854775807') !== -1)
    }

    function test_a_server_fetched_choice_still_fetches_and_is_not_an_enum() {
        var form = createTemporaryObject(choiceForm, testCase)
        var slot = meta(form, "slot")
        compare(slot.isChoice, true)
        compare(slot.isEnum, false)
        verify(mockController.fetched.indexOf("ListSlots") !== -1)
    }

    // ── the control ──────────────────────────────────────────────────────────

    function test_an_enum_renders_a_selection_control_not_a_text_field() {
        var form = createTemporaryObject(roleForm, testCase)
        var control = findChild(form, "field_role")
        verify(control !== null)
        // A ComboBox has `currentIndex` and a `model`; the plain TextField has
        // `placeholderText` and neither. This is what stops "Emperor" from
        // ever being typed in.
        verify(control.currentIndex !== undefined)
        verify(control.model !== undefined)
        verify(control.placeholderText === undefined)
        compare(control.visible, true)
        compare(control.count, 3)
        compare(control.textAt(0), "Viewer")
        compare(control.textAt(2), "Manager")
    }

    function test_both_shipped_pastebin_enums_render_as_selection_controls() {
        var form = createTemporaryObject(pasteForm, testCase)
        var visibility = findChild(form, "field_visibility")
        var editability = findChild(form, "field_editability")
        verify(visibility !== null && editability !== null)
        verify(visibility.currentIndex !== undefined && editability.currentIndex !== undefined)
        compare(visibility.count, 2)
        compare(editability.count, 2)
        compare(visibility.textAt(1), "Private")
        // The non-enum sibling is untouched: still a text field.
        verify(findChild(form, "field_content").placeholderText !== undefined)
    }

    function test_nothing_is_preselected_so_a_required_enum_still_blocks_submit() {
        var form = createTemporaryObject(roleForm, testCase)
        compare(findChild(form, "field_role").currentIndex, -1)
        findChild(form, "field_projectId").text = "1"
        findChild(form, "field_principal").text = "bob"
        // `role` is required and untouched.
        compare(form.ready, false)
    }

    // ── the wire value, and the gate ─────────────────────────────────────────

    function test_choosing_an_option_serialises_the_enum_name_glaze_reads() {
        var form = createTemporaryObject(roleForm, testCase)
        findChild(form, "field_projectId").text = "1"
        findChild(form, "field_principal").text = "bob"
        var combo = findChild(form, "field_role")
        combo.currentIndex = 2
        combo.activated(2)
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"role":"Manager"') !== -1)
    }

    function test_a_value_outside_the_set_leaves_the_form_not_ready() {
        var form = createTemporaryObject(roleForm, testCase)
        findChild(form, "field_projectId").text = "1"
        findChild(form, "field_principal").text = "bob"
        // The exact case morph#386 measured: before the fix `ready` was true
        // and a body was assembled for it, so the client gate said the
        // opposite of what it exists to say.
        form.setFieldValue("role", '"Emperor"')
        compare(form.ready, false)
        compare(form.previewLine, "")
        compare(form.previewLine.indexOf("Emperor"), -1)
    }

    function test_an_out_of_set_value_is_refused_even_unquoted() {
        var form = createTemporaryObject(roleForm, testCase)
        findChild(form, "field_projectId").text = "1"
        findChild(form, "field_principal").text = "bob"
        // The stored text is the JSON literal, so a bare `Manager` is not a
        // member of the set either -- it would have gone out as a nonsense
        // body fragment (`"role":Manager`) if it were passed through.
        form.setFieldValue("role", "Manager")
        compare(form.ready, false)
    }

    function test_an_out_of_set_value_is_never_auto_submitted() {
        var form = createTemporaryObject(roleForm, testCase)
        mockController.submitCount = 0
        findChild(form, "field_projectId").text = "1"
        findChild(form, "field_principal").text = "bob"
        form.setFieldValue("role", '"Emperor"')
        compare(mockController.submitCount, 0)
    }

    function test_resetFields_clears_an_enum_selection() {
        var form = createTemporaryObject(roleForm, testCase)
        var combo = findChild(form, "field_role")
        combo.currentIndex = 1
        combo.activated(1)
        compare(form.fieldValues["role"], '"Member"')
        // resetFields() drives every field through its `field_` control, and
        // a ComboBox has no writable `text` -- clearing one the way a
        // TextField is cleared throws, taking the whole reset with it.
        form.resetFields()
        compare(form.fieldValues["role"], undefined)
        compare(combo.currentIndex, -1)
        // The reset ran to completion: the sibling text fields were cleared
        // too, so nothing threw part-way through the loop.
        compare(findChild(form, "field_principal").text, "")
    }

    function test_an_optional_enum_left_blank_is_omitted_rather_than_invalid() {
        var form = createTemporaryObject(handWrittenForm, testCase)
        // `nullable` is not in `required`; blank means "not answered".
        compare(form.ready, true)
        compare(form.previewLine.indexOf("nullable"), -1)
    }
}
