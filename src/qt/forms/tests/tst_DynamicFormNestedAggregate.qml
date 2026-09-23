// SPDX-License-Identifier: Apache-2.0
//
// What DynamicForm does with a nested-aggregate member, cyclic or not
// (morph#727; docs/spec/forms/forms.md, "What DynamicForm does with a nested
// aggregate").
//
// morph#703 made `schemaJson<A>()` emit a finite, correctly annotated schema
// for a self-referential domain type, by way of a `$ref` back into `$defs`.
// It did not say what the shipped renderer should draw for one, and the spec
// said only that morph "does not promise to render the form". This suite is
// the measurement that replaced that non-promise with a stated contract, and
// it pins every part of it:
//
//   1. A `$ref` cycle does not loop, hang or crash the renderer. `resolveRef`
//      follows a `$ref` exactly one level and merges; it never recurses, so a
//      cycle is not a loop.
//   2. A nested aggregate is NOT drawn as a sub-form. It becomes one scalar
//      control at the parent level, and its own members get no control at all.
//   3. That is true of an acyclic nested aggregate too, which is the point:
//      the cycle is not what stops the renderer, nesting is. The acyclic case
//      is in here as the control -- without it, (2) reads as a cycle-specific
//      defect rather than the general limit it is.
//   4. The payload is wrong and the form does not say so: an object-typed
//      member submits as a JSON *string*, an array-of-objects member as an
//      array of strings, and `ready` is true for both.
//
// (4) is the part worth arguing about, and the argument is morph#759, not
// this file. This suite states today's behaviour so that a change to it is
// visible as a failing test rather than as a silent difference.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormNestedAggregate"
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

    // --- The cyclic case -------------------------------------------------
    //
    // Exactly the document `schemaJson<SelfAction>()` emits for
    //
    //     struct TreeNode   { std::string name; std::vector<TreeNode> children; };
    //     struct SelfAction { std::int64_t id{}; TreeNode root{}; };
    //
    // as quoted in docs/spec/forms/forms.md, "Nested aggregates (recursive,
    // cycle-safe)": one `$defs` entry that `$ref`s itself through `children`.
    property var cyclicSchema: ({
        type: "object",
        properties: {
            id: { type: "integer", "x-order": 0, title: "Id" },
            root: { "$ref": "#/$defs/probe::TreeNode", "x-order": 1, title: "Root" }
        },
        required: ["id", "root"],
        "$defs": {
            "probe::TreeNode": {
                type: "object",
                properties: {
                    children: {
                        type: "array",
                        items: { "$ref": "#/$defs/probe::TreeNode" },
                        "x-order": 1,
                        title: "Children"
                    },
                    name: { type: "string", "x-order": 0, title: "Name" }
                },
                additionalProperties: false,
                required: ["name", "children"]
            }
        }
    })

    Component {
        id: cyclicComponent
        DynamicForm {
            actionType: "SelfAction"
            schema: testCase.cyclicSchema
            controller: mockController
        }
    }

    // (1) The form is built at all. A renderer that followed the cycle would
    // not get here, so this case is the no-hang claim -- previously a reading
    // of nine lines of QML, now a run.
    function test_a_cyclic_ref_builds_a_form_instead_of_looping() {
        var form = createTemporaryObject(cyclicComponent, testCase)
        verify(form !== null)
        compare(form.fields.length, 2)
        compare(form.fields[0].name, "id")
        compare(form.fields[1].name, "root")
    }

    // (2) The cyclic member is one scalar control, and `TreeNode`'s own
    // members reach no control at all -- `name`/`children` exist only inside
    // the `$defs` entry the renderer merged and then treated as a leaf.
    function test_the_cyclic_member_is_one_plain_text_field_not_a_sub_form() {
        var form = createTemporaryObject(cyclicComponent, testCase)
        verify(form !== null)
        verify(findChild(form, "field_root") !== null)
        // The sub-form that is NOT drawn. Both spellings, because a future
        // sub-form could name its controls either way.
        compare(findChild(form, "field_name"), null)
        compare(findChild(form, "field_children"), null)
        compare(findChild(form, "field_root_name"), null)
        compare(findChild(form, "field_root_children"), null)
    }

    // (4) The wrong payload, stated exactly. `root` must be an object; what
    // the form offers is a string, and it reports itself ready to send it.
    function test_the_cyclic_member_submits_as_a_json_string_and_the_form_says_ready() {
        var form = createTemporaryObject(cyclicComponent, testCase)
        verify(form !== null)
        compare(form.ready, false)  // both members are required, both blank

        findChild(form, "field_id").text = "7"
        compare(form.ready, false)  // `root` still blank

        findChild(form, "field_root").text = "anything"
        compare(form.ready, true)

        var parsed = JSON.parse(form.previewLine)
        compare(parsed.id, 7)
        compare(typeof parsed.root, "string")
        compare(parsed.root, "anything")
    }

    // --- The acyclic control ---------------------------------------------
    //
    // glaze inlines a nested type used exactly once, so this is the shape an
    // ordinary, non-recursive nested struct arrives in. It matters that this
    // behaves identically: it is what makes the cycle a non-event rather than
    // the cause.
    property var acyclicInlineSchema: ({
        type: "object",
        properties: {
            id: { type: "integer", "x-order": 0, title: "Id" },
            address: {
                type: "object",
                properties: {
                    street: { type: "string", "x-order": 0, title: "Street" },
                    city: { type: "string", "x-order": 1, title: "City" }
                },
                required: ["street", "city"],
                "x-order": 1,
                title: "Address"
            }
        },
        required: ["id", "address"]
    })

    Component {
        id: acyclicComponent
        DynamicForm {
            actionType: "AcyclicAction"
            schema: testCase.acyclicInlineSchema
            controller: mockController
        }
    }

    // (3) Same outcome with no cycle anywhere: one control for the whole
    // sub-object, none for its members, and a string payload.
    function test_an_acyclic_nested_aggregate_is_flattened_the_same_way() {
        var form = createTemporaryObject(acyclicComponent, testCase)
        verify(form !== null)
        compare(form.fields.length, 2)
        verify(findChild(form, "field_address") !== null)
        compare(findChild(form, "field_street"), null)
        compare(findChild(form, "field_city"), null)

        findChild(form, "field_id").text = "7"
        findChild(form, "field_address").text = "somewhere"
        compare(form.ready, true)

        var parsed = JSON.parse(form.previewLine)
        compare(typeof parsed.address, "string")
        compare(parsed.address, "somewhere")
    }

    // --- A recursive collection at the root -------------------------------
    //
    // `TreeNode` used as the action type itself: `children` is an array whose
    // items `$ref` back to `TreeNode`. The array control is chosen by
    // `type: "array"` alone and encodes every comma-separated entry as a JSON
    // string, so an array of objects arrives as an array of strings.
    property var rootIsCyclicSchema: ({
        type: "object",
        properties: {
            children: {
                type: "array",
                items: { "$ref": "#/$defs/probe::TreeNode" },
                "x-order": 1,
                title: "Children"
            },
            name: { type: "string", "x-order": 0, title: "Name" }
        },
        required: ["name", "children"],
        "$defs": {
            "probe::TreeNode": {
                type: "object",
                properties: {
                    children: {
                        type: "array",
                        items: { "$ref": "#/$defs/probe::TreeNode" },
                        "x-order": 1,
                        title: "Children"
                    },
                    name: { type: "string", "x-order": 0, title: "Name" }
                },
                additionalProperties: false,
                required: ["name", "children"]
            }
        }
    })

    Component {
        id: rootCyclicComponent
        DynamicForm {
            actionType: "TreeNodeAction"
            schema: testCase.rootIsCyclicSchema
            controller: mockController
        }
    }

    function test_a_recursive_collection_gets_the_array_control_and_string_items() {
        var form = createTemporaryObject(rootCyclicComponent, testCase)
        verify(form !== null)
        compare(form.fields.length, 2)
        // Declaration order, not JSON key order: `name` carries x-order 0.
        compare(form.fields[0].name, "name")
        compare(form.fields[1].name, "children")
        compare(form.fields[1].isArray, true)

        findChild(form, "field_name").text = "top"
        findChild(form, "field_children").text = "a, b"
        compare(form.ready, true)

        var parsed = JSON.parse(form.previewLine)
        compare(parsed.name, "top")
        verify(Array.isArray(parsed.children))
        compare(parsed.children.length, 2)
        compare(typeof parsed.children[0], "string")
        compare(parsed.children[0], "a")
        compare(parsed.children[1], "b")
    }
}
