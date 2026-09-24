// SPDX-License-Identifier: Apache-2.0
//
// What DynamicForm does with a nested-aggregate member, cyclic or not
// (docs/spec/forms/forms.md, "What DynamicForm does with a nested aggregate").
//
// `schemaJson<A>()` emits a finite, correctly annotated schema for a
// self-referential domain type, by way of a `$ref` back into `$defs`.
// That says nothing about what the shipped renderer draws for one, and the
// spec says only that morph "does not promise to render the form". This suite is
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
//   4. The form does not claim a payload it cannot assemble. Such a member is
//      unrepresentable -- the control that was drawn collects text, and text
//      encodes as a JSON *string* where the schema asks for an object -- so
//      the form stays short of `ready`, submits nothing, and names the member
//      in `unrepresentableReason`.
//
// (4) is the readiness claim, and it is about the payload rather than about
// which controls happen to be filled: whether the renderer should eventually
// draw the sub-form, decline the schema or go on flattening it, the answer to
// "does the body satisfy the schema" is the same. The last two cases here are
// the boundary in the other direction -- a member the payload legitimately
// omits, and a flat form -- because the way to break this is to make an
// ordinary form unready.

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

    // (4) `root` must be an object; the only thing the drawn control collects
    // is text. So the form never reports ready, never assembles a body, and
    // says which member it is stuck on. Filling the control is not a remedy,
    // which is exactly why the reason has to be reachable: "fill the required
    // fields" is advice no input can act on here.
    function test_the_cyclic_member_leaves_the_form_unready_and_says_why() {
        var form = createTemporaryObject(cyclicComponent, testCase)
        verify(form !== null)
        var submitsBefore = mockController.submitCount
        compare(form.ready, false)  // both members are required, both blank

        // The reason is a property of the schema, so it is readable before
        // anything is typed -- a caller can refuse the form up front.
        compare(form.fields[0].unrepresentable, "")
        verify(form.fields[1].unrepresentable !== "")
        verify(form.unrepresentableReason.indexOf("root: ") === 0)

        findChild(form, "field_id").text = "7"
        compare(form.ready, false)  // `root` still blank

        findChild(form, "field_root").text = "anything"
        compare(form.ready, false)
        verify(form.unrepresentableReason.indexOf("root: ") === 0)
        // Nothing to preview, because there is no body to send.
        compare(form.previewLine, "")
        compare(mockController.submitCount, submitsBefore)
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
    // sub-object, none for its members -- and the same refusal to call the
    // result ready.
    function test_an_acyclic_nested_aggregate_is_flattened_the_same_way() {
        var form = createTemporaryObject(acyclicComponent, testCase)
        verify(form !== null)
        var submitsBefore = mockController.submitCount
        compare(form.fields.length, 2)
        verify(findChild(form, "field_address") !== null)
        compare(findChild(form, "field_street"), null)
        compare(findChild(form, "field_city"), null)

        findChild(form, "field_id").text = "7"
        findChild(form, "field_address").text = "somewhere"
        compare(form.ready, false)
        verify(form.unrepresentableReason.indexOf("address: ") === 0)
        compare(form.previewLine, "")
        compare(mockController.submitCount, submitsBefore)
    }

    // --- A recursive collection at the root -------------------------------
    //
    // `TreeNode` used as the action type itself: `children` is an array whose
    // items `$ref` back to `TreeNode`. The array control is chosen by
    // `type: "array"` alone and encodes every comma-separated entry as a JSON
    // string, so it cannot produce the array of *objects* the schema asks for.
    // A control was drawn, and the member is unrepresentable anyway -- which is
    // why "a control the renderer drew is filled" is not what `ready` claims.
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

    function test_a_recursive_collection_gets_the_array_control_but_no_payload() {
        var form = createTemporaryObject(rootCyclicComponent, testCase)
        verify(form !== null)
        var submitsBefore = mockController.submitCount
        compare(form.fields.length, 2)
        // Declaration order, not JSON key order: `name` carries x-order 0.
        compare(form.fields[0].name, "name")
        compare(form.fields[1].name, "children")
        // The array control is still the one drawn: what changed is the claim
        // about the payload, not the rendering.
        compare(form.fields[1].isArray, true)
        compare(form.fields[0].unrepresentable, "")
        verify(form.fields[1].unrepresentable !== "")

        findChild(form, "field_name").text = "top"
        findChild(form, "field_children").text = "a, b"
        compare(form.ready, false)
        verify(form.unrepresentableReason.indexOf("children: ") === 0)
        compare(form.previewLine, "")
        compare(mockController.submitCount, submitsBefore)
    }

    // --- The boundary: what is still ready --------------------------------
    //
    // An *optional* nested member is one the payload may legitimately leave
    // out, so leaving it out is a body the schema accepts and the form says so.
    // That the member is undrawable is still reachable on its descriptor --
    // it is not dropped silently, it is declined. Typing into its control
    // anyway is the case the gate must catch: there is no encoding for that
    // text, so the form goes unready rather than quoting it into the body.
    property var optionalNestedSchema: ({
        type: "object",
        properties: {
            id: { type: "integer", "x-order": 0, title: "Id" },
            note: { type: "string", "x-order": 1, title: "Note" },
            address: {
                type: "object",
                properties: { street: { type: "string", "x-order": 0, title: "Street" } },
                required: ["street"],
                "x-order": 2,
                title: "Address"
            }
        },
        required: ["id"]
    })

    Component {
        id: optionalNestedComponent
        DynamicForm {
            actionType: "OptionalNestedAction"
            schema: testCase.optionalNestedSchema
            controller: mockController
        }
    }

    function test_an_optional_unrepresentable_member_may_be_omitted() {
        var form = createTemporaryObject(optionalNestedComponent, testCase)
        verify(form !== null)
        verify(form.fields[2].unrepresentable !== "")

        findChild(form, "field_id").text = "7"
        compare(form.ready, true)
        compare(form.unrepresentableReason, "")
        var parsed = JSON.parse(form.previewLine)
        compare(parsed.id, 7)
        compare(parsed.address, undefined)

        // ... and the moment something is typed into it, there is no literal
        // for it and the form stops claiming the body is acceptable.
        findChild(form, "field_address").text = "somewhere"
        compare(form.ready, false)
        verify(form.unrepresentableReason.indexOf("address: ") === 0)
    }

    // The regression this contract most easily causes: an ordinary flat form
    // judged unready because a member's schema was misread as a nested
    // aggregate. A Quantity is `"type": "object"` in the schema and must not
    // be caught by it, and neither must an array of strings.
    property var flatSchema: ({
        type: "object",
        properties: {
            id: { type: "integer", "x-order": 0, title: "Id" },
            note: { type: "string", "x-order": 1, title: "Note" },
            tags: { type: "array", items: { type: "string" }, "x-order": 2, title: "Tags" },
            weight: { type: "object", "x-order": 3, "x-decimalPlaces": 2, title: "Weight",
                      ExtUnits: { unitAscii: "kg", unitUnicode: "kg" } }
        },
        required: ["id", "note", "tags", "weight"]
    })

    Component {
        id: flatComponent
        DynamicForm {
            actionType: "FlatAction"
            schema: testCase.flatSchema
            controller: mockController
        }
    }

    function test_a_flat_form_names_nothing_unrepresentable_and_reaches_ready() {
        var form = createTemporaryObject(flatComponent, testCase)
        verify(form !== null)
        compare(form.fields.length, 4)
        for (var i = 0; i < form.fields.length; ++i)
            compare(form.fields[i].unrepresentable, "")

        findChild(form, "field_id").text = "7"
        findChild(form, "field_note").text = "hello"
        findChild(form, "field_tags").text = "a, b"
        findChild(form, "field_weight").text = "1.25"
        compare(form.ready, true)
        compare(form.unrepresentableReason, "")

        var parsed = JSON.parse(form.previewLine)
        compare(parsed.id, 7)
        compare(parsed.note, "hello")
        verify(Array.isArray(parsed.tags))
        compare(parsed.tags[0], "a")
        compare(parsed.weight.num, 125)
    }
}
