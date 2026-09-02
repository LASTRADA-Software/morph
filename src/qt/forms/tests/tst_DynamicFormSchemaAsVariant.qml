// SPDX-License-Identifier: Apache-2.0
//
// The same schema, reaching DynamicForm two ways, must render the same form
// (morph#388).
//
// Every shipped app *binds* `schema` declaratively, so the value arrives as a
// genuine JS object and every `Array.isArray` in the renderer answers true. A
// schema handed over imperatively does not: an initial property, a
// `setProperty` from C++, or `createTemporaryObject(c, p, {schema: ...})`
// round-trips the value through `QVariant`, and each of its arrays comes back
// as a `QVariantList` -- for which `Array.isArray` is **false**.
//
// That silently mistyped every field the renderer types by asking that
// question:
//
//   - `{"type": ["integer","null"]}` (what schemaJson emits for a rule-3
//     strong id under `$defs`) wrapped instead of unpacking, so `isInteger`
//     came out false and the id went out as a quoted JSON *string* -- the
//     parse_number_failure morph#189 already fixed once, through a new door;
//   - the `anyOf`-over-`$ref` collapse of morph#189 itself went inert, so a
//     nullable `$ref` member regressed to that same pre-#189 encoding;
//   - the closed-set recognition of morph#386 (`oneOf` of `const`, and the
//     bare `enum` keyword) stopped firing, so an enum drew a free-text box.
//
// Nothing warned: the form reported `ready` and produced a body the server
// refuses. Each case below therefore asserts the two forms against **each
// other** rather than against a transcribed expectation -- the bound form is
// the reference, and any future array-shaped schema key is covered by the
// whole-`fields` comparison without this file being edited.
//
// One axis that comparison does *not* cover, because no renderer can:
// JSON key order does not survive the QVariant boundary. See
// test_x_order_is_what_makes_the_two_layouts_agree below.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormSchemaAsVariant"
    visible: true

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)
        function submitIfValid(actionType, bodyJson) {}
        function fetchOptions(optionsAction, body) { optionsReceived(optionsAction, true, "[]") }
    }

    // schemaJson<kanban::SetMemberRole>(), verbatim (the same fixture
    // tst_DynamicFormEnumChoice.qml pins): `projectId` is the array-valued
    // `type` of the report, `role` the morph#386 closed set.
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

    // The remaining array-shaped keys in one schema: the morph#189
    // `anyOf`-over-`$ref` collapse (`optId`), the bare `enum` keyword
    // (`size`), `type: "array"` (`tags`), plus `required` and `x-layout`,
    // which the triage of morph#388 measured as *not* degrading -- kept here
    // so a fix that normalises the schema cannot quietly break them.
    property var mixedSchema: ({
        "$defs": { "int64_t": { "type": "integer", "minimum": -9223372036854775808 } },
        "properties": {
            "optId": { "anyOf": [{ "$ref": "#/$defs/int64_t" }, { "type": "null" }],
                       "x-order": 0, "title": "Opt Id", "x-section": 0 },
            "size": { "enum": [1, 2, 3], "x-order": 1, "title": "Size" },
            "tags": { "type": "array", "items": { "type": "string" }, "x-order": 2, "title": "Tags" },
            "note": { "type": ["string", "null"], "x-order": 3, "title": "Note" }
        },
        "x-layout": { "groups": [{ "title": "Ids", "kind": "flat" }] },
        "required": ["size"]
    })

    // Declaratively bound -- the reference behaviour, and what every shipped
    // app does.
    Component {
        id: boundRoleForm
        DynamicForm { actionType: "SetMemberRole"; schema: testCase.roleSchema; controller: mockController }
    }

    Component {
        id: boundMixedForm
        DynamicForm { actionType: "Mixed"; schema: testCase.mixedSchema; controller: mockController }
    }

    // No `schema` binding at all: the value can only arrive as an initial
    // property, which is the QVariant round trip under test.
    Component {
        id: variantRoleForm
        DynamicForm { actionType: "SetMemberRole"; controller: mockController }
    }

    Component {
        id: variantMixedForm
        DynamicForm { actionType: "Mixed"; controller: mockController }
    }

    function bound(component) {
        var form = createTemporaryObject(component, testCase)
        verify(form !== null)
        return form
    }

    function viaVariant(component, schema) {
        var form = createTemporaryObject(component, testCase, { schema: schema })
        verify(form !== null)
        return form
    }

    // ── the premise: the two really are different values ─────────────────────

    function test_an_assigned_schema_is_not_a_plain_js_object() {
        // Guards the fixture itself. If a future Qt made an initial property
        // arrive as a genuine JS object, every assertion below would pass
        // without exercising anything, and this is the only place that would
        // say so.
        var form = viaVariant(variantRoleForm, testCase.roleSchema)
        verify(!Array.isArray(form.schema["$defs"]["ProjectId"].type))
        compare(Array.isArray(testCase.roleSchema["$defs"]["ProjectId"].type), true)
    }

    // ── the whole field table, both ways ─────────────────────────────────────

    function test_role_schema_yields_identical_fields_either_way() {
        compare(JSON.stringify(viaVariant(variantRoleForm, testCase.roleSchema).fields),
                JSON.stringify(bound(boundRoleForm).fields))
    }

    function test_mixed_schema_yields_identical_fields_either_way() {
        compare(JSON.stringify(viaVariant(variantMixedForm, testCase.mixedSchema).fields),
                JSON.stringify(bound(boundMixedForm).fields))
    }

    // ── and the encoding those fields drive ──────────────────────────────────

    function test_an_array_valued_type_still_encodes_its_id_as_a_number() {
        var form = viaVariant(variantRoleForm, testCase.roleSchema)
        // The report's own case: jsonType came out as the whole
        // ["integer","null"] array, isInteger false, and the id was submitted
        // as "7" rather than 7.
        compare(form.fieldByName["projectId"].jsonType, "integer")
        compare(form.fieldByName["projectId"].isInteger, true)
        put(form, "projectId", "7")
        put(form, "principal", "alice")
        put(form, "role", 1)
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"projectId":7') !== -1)
        verify(form.previewLine.indexOf('"projectId":"7"') === -1)
    }

    // Fills one field through whichever control the form actually drew: a
    // combo box for a closed set (selected the way a user does, since
    // `activated` is what the renderer listens to), a text field otherwise.
    //
    // Dispatching on the control rather than on the schema is deliberate --
    // a form left mistyped by this bug draws a TextField where the fixed one
    // draws a picker, and this way it still fills in and still reaches the
    // assertion. The failure to show is the differing body, not a missing
    // property on a control that should never have been a TextField.
    function put(form, field, textOrIndex) {
        var control = findChild(form, "field_" + field)
        verify(control !== null)
        if (control.currentIndex !== undefined) {
            control.currentIndex = textOrIndex
            control.activated(textOrIndex)
        } else {
            control.text = String(textOrIndex)
        }
    }

    function test_previewLine_is_identical_either_way() {
        var variant = viaVariant(variantRoleForm, testCase.roleSchema)
        var declared = bound(boundRoleForm)
        var forms = [variant, declared]
        for (var i = 0; i < forms.length; ++i) {
            put(forms[i], "projectId", "7")
            put(forms[i], "principal", "alice")
            put(forms[i], "role", 1)
        }
        // Anchored absolutely before being compared pairwise: two forms that
        // both fail a future readiness gate would agree on `false` and `""`
        // and assert nothing at all.
        compare(declared.ready, true)
        compare(declared.previewLine, '{"projectId":7,"principal":"alice","role":"Member"}')
        compare(variant.ready, declared.ready)
        compare(variant.previewLine, declared.previewLine)
    }

    function test_a_nullable_ref_member_keeps_morph189s_numeric_encoding() {
        // morph#189's own fix reads `anyOf` through Array.isArray, so on this
        // path it stopped running: the field lost its type entirely and fell
        // back to the quoted-string encoding #189 was written to remove.
        var form = viaVariant(variantMixedForm, testCase.mixedSchema)
        compare(form.fieldByName["optId"].isInteger, true)
        put(form, "size", 0)
        put(form, "optId", "9223372036854775807")
        compare(form.ready, true)
        verify(form.previewLine.indexOf('"optId":9223372036854775807') !== -1)
        verify(form.previewLine.indexOf('"optId":"9223372036854775807"') === -1)
    }

    function test_a_closed_set_is_still_drawn_as_a_picker() {
        // morph#386's recognition reads `oneOf`/`enum` the same way.
        var form = viaVariant(variantRoleForm, testCase.roleSchema)
        compare(form.fieldByName["role"].isEnum, true)
        compare(form.fieldByName["role"].enumOptions.length, 3)
        var control = findChild(form, "field_role")
        verify(control.currentIndex !== undefined)
        compare(control.count, 3)

        var mixed = viaVariant(variantMixedForm, testCase.mixedSchema)
        compare(mixed.fieldByName["size"].isEnum, true)
        compare(mixed.fieldByName["size"].enumOptions.map(function (r) { return r.valueJson }),
                ["1", "2", "3"])
    }

    // ── what the round trip really does destroy ──────────────────────────────

    function test_x_order_is_what_makes_the_two_layouts_agree() {
        // The QVariant boundary converts through a *sorted* map, so JSON key
        // order is gone before the renderer is reached and no amount of
        // re-reading recovers it. That is the rule `x-order` exists for
        // (docs/spec/forms/forms.md, "Renderer contract": renderers lay
        // fields out in ascending `x-order`, not in JSON key order), and
        // `schemaJson<A>()` emits `x-order` on every property -- so this is
        // an observation about the boundary, not a gap in the fix.
        //
        // Asserted rather than assumed: if the keys did *not* get reordered,
        // the layout agreement below would be proving nothing.
        var form = viaVariant(variantRoleForm, testCase.roleSchema)
        compare(Object.keys(form.schema.properties), ["principal", "projectId", "role"])
        compare(Object.keys(testCase.roleSchema.properties), ["principal", "projectId", "role"])

        // ...and `x-order` overrides that order in both, identically.
        var names = function (f) { return f.fields.map(function (x) { return x.name }) }
        compare(names(form), ["projectId", "principal", "role"])
        compare(names(form), names(bound(boundRoleForm)))
    }

    // ── the keys that already survived the round trip, still surviving ───────

    function test_required_and_layout_still_read_correctly() {
        var form = viaVariant(variantMixedForm, testCase.mixedSchema)
        compare(form.fieldByName["size"].required, true)
        compare(form.fieldByName["optId"].required, false)
        compare(form.sections.length, 2)
        compare(form.sections[0].title, "Ids")
        compare(form.sections[0].fields.map(function (f) { return f.name }), ["optId"])
        compare(form.sections[1].fields.length, 3)
    }
}
