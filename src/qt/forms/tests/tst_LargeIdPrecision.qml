// SPDX-License-Identifier: Apache-2.0
//
// Ids above 2^53 must survive the renderer's JSON round trip exactly
// (morph#190, morph#191).
//
// JavaScript numbers are IEEE-754 doubles, so `JSON.parse` cannot hold an
// integer above 2^53, and re-serialising the rounded value emits a *different*
// id than the app sent. Doubles round to even in that range, so the two ids
// used throughout this file -- 9007199254740993 and 9007199254740992 -- both
// round to 9007199254740992: neighbouring rows become indistinguishable, and
// acting on one acts on the other.
//
// Every payload below is written as a raw string, never built with
// JSON.stringify: stringifying a JS object would round the ids before the
// component under test ever saw them, and the test would pass against the very
// bug it is meant to catch.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "LargeIdPrecision"
    when: windowShown
    width: 640
    height: 480

    readonly property string oddId: "9007199254740993"
    readonly property string evenId: "9007199254740992"

    // Two rows whose ids differ by one, both beyond the exactly-representable
    // range. Raw text, so the digits reach the component intact.
    readonly property string rowsPayload:
        '{"rows":[{"id":9007199254740993,"name":"Alpha"},'
        + '{"id":9007199254740992,"name":"Beta"}]}'

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)

        property int queryCount: 0
        property var callsByAction: ({})

        function submitIfValid(actionType, bodyJson) {
            callsByAction[actionType] = bodyJson
            if (actionType === "ListRows") {
                queryCount += 1
                replyReceived(actionType, true, testCase.rowsPayload)
                return
            }
            replyReceived(actionType, true, '{"ok":true}')
        }

        function fetchOptions(optionsAction, bodyJson) {
            optionsReceived(optionsAction, true, testCase.rowsPayload)
        }
    }

    property var testView: ({
        "v-kind": "collection",
        "v-title": "Rows",
        "v-query": "ListRows",
        "v-rowKey": "id",
        "v-columns": [
            { "field": "id", "label": "ID" },
            { "field": "name", "label": "Name" }
        ],
        "v-rowAction": { "action": "EditRow", "bind": { "id": "id" } },
        "v-actions": [
            { "action": "DeleteRow", "label": "Delete", "scope": "row", "bind": { "id": "id" }, "confirm": true }
        ]
    })

    property var testSchemas: ({
        "EditRow": {
            properties: {
                id: { type: "integer", "x-order": 0 },
                name: { type: "string", "x-order": 1 }
            },
            required: ["id"]
        }
    })

    Component {
        id: viewComponent
        CollectionView {
            viewId: "TestView"
            view: testCase.testView
            schemas: testCase.testSchemas
            controller: mockController
        }
    }

    Component {
        id: idLabelledChoiceComponent
        DynamicForm {
            actionType: "Probe"
            controller: mockController
            schema: ({
                "properties": {
                    "slot": { "type": ["integer", "null"], "x-order": 0, "title": "Slot",
                              "x-optionsAction": "ListRows",
                              "x-optionValue": "id", "x-optionLabel": "id" }
                },
                "required": ["slot"]
            })
        }
    }

    Component {
        id: choiceFormComponent
        DynamicForm {
            actionType: "Probe"
            controller: mockController
            schema: ({
                "properties": {
                    "slot": { "type": ["integer", "null"], "x-order": 0, "title": "Slot",
                              "x-optionsAction": "ListRows",
                              "x-optionValue": "id", "x-optionLabel": "name" }
                },
                "required": ["slot"]
            })
        }
    }

    // ── morph#191: CollectionView row ids ────────────────────────────────────

    // The two rows must remain addressable as distinct objects. Before the fix
    // both objectNames ended in the same rounded id, so findChild could not
    // name one row rather than the other.
    function test_neighbouringRowIdsStayDistinct() {
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)
        var oddButton = findChild(view, "rowOpen_" + testCase.oddId)
        var evenButton = findChild(view, "rowOpen_" + testCase.evenId)
        verify(oddButton !== null)
        verify(evenButton !== null)
        verify(oddButton !== evenButton)
    }

    // The id a row action sends must be the row's own, exactly. This is the
    // reported defect: deleting Beta deleted Alpha.
    function test_deleteSendsTheClickedRowsExactId() {
        mockController.callsByAction = ({})
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)

        var deleteBeta = findChild(view, "rowAction_DeleteRow_" + testCase.evenId)
        verify(deleteBeta !== null)
        deleteBeta.clicked()
        var dialog = findChild(view, "confirmDialog")
        verify(dialog !== null)
        dialog.accept()
        compare(mockController.callsByAction["DeleteRow"], '{"id":' + testCase.evenId + '}')

        mockController.callsByAction = ({})
        var deleteAlpha = findChild(view, "rowAction_DeleteRow_" + testCase.oddId)
        verify(deleteAlpha !== null)
        deleteAlpha.clicked()
        dialog.accept()
        compare(mockController.callsByAction["DeleteRow"], '{"id":' + testCase.oddId + '}')
    }

    // The visible cell must show the id the app sent, not a rounded one.
    function test_cellRendersTheExactId() {
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)
        var cell = findChild(view, "cell_id_" + testCase.oddId)
        verify(cell !== null)
        compare(cell.text, testCase.oddId)
    }

    // ── morph#190: choice option ids ─────────────────────────────────────────

    // Each option's valueJson is the literal the form submits for that choice,
    // so two options must not share one. Before the fix both were the rounded
    // even id, and the combo held two entries the UI could not tell apart.
    function test_choiceOptionValuesStayDistinct() {
        var form = createTemporaryObject(choiceFormComponent, testCase)
        verify(form !== null)
        mockController.fetchOptions("ListRows", "{}")
        var options = form.fieldOptions["slot"]
        verify(options !== undefined)
        compare(options.length, 2)
        compare(options[0].valueJson, testCase.oddId)
        compare(options[1].valueJson, testCase.evenId)
        verify(options[0].valueJson !== options[1].valueJson)
    }

    // An id used as the visible label must read exactly. Labelling by `name`
    // would not discriminate -- a rounded id leaves "Alpha"/"Beta" untouched --
    // so this form labels by `id` on purpose.
    function test_choiceOptionLabelsAreExact() {
        var form = createTemporaryObject(idLabelledChoiceComponent, testCase)
        verify(form !== null)
        mockController.fetchOptions("ListRows", "{}")
        var options = form.fieldOptions["slot"]
        verify(options !== undefined)
        compare(options.length, 2)
        compare(options[0].label, testCase.oddId)
        compare(options[1].label, testCase.evenId)
        verify(options[0].label !== options[1].label)
    }

    // ── Values a double *does* hold must be untouched ────────────────────────
    //
    // Without this the fix could "pass" by turning every number into a string,
    // which would change the wire shape of every ordinary id in the tree.

    function test_ordinaryIdsAreUnchanged() {
        mockController.callsByAction = ({})
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)
        // Re-answer the query with small ids and confirm the body is identical
        // to what a plain JSON round trip would have produced.
        mockController.replyReceived("ListRows", true, '{"rows":[{"id":7,"name":"Small"}]}')
        var deleteSmall = findChild(view, "rowAction_DeleteRow_7")
        verify(deleteSmall !== null)
        deleteSmall.clicked()
        var dialog = findChild(view, "confirmDialog")
        dialog.accept()
        compare(mockController.callsByAction["DeleteRow"], '{"id":7}')
    }
}
