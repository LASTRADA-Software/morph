// SPDX-License-Identifier: Apache-2.0
//
// Covers CollectionView's list/master-detail behavior against a mock
// controller: populate on completion, derived-column formatting (including a
// Quantity column's ExtUnits/x-decimalPlaces), row-open prefilling the
// editor via bind, a confirm-guarded row action, a collection-scope action,
// and re-populate after any of the three resolves.
//
// mockController.submitIfValid is fully synchronous (it emits replyReceived
// inline), so firing a mutating action (EditRow/DeleteRow/CreateRow)
// synchronously cascades into CollectionView's own re-populate call (a
// second, nested submitIfValid("ListRows", ...)) before control ever
// returns to the test function -- by the time a test resumes, the *last*
// call the mock observed is always the re-populate, not the mutating action
// that triggered it. `callsByAction` (a per-action-type record, never
// overwritten by a different action's call) is what lets a test recover the
// specific call it cares about after that cascade.
//
// `mockController` (like `queryCount`/`callsByAction` on it) is a single
// object shared across every test function in this file, not recreated per
// test (mirrors tst_DynamicFormReactive.qml's mock-controller pattern) --
// every test that reads `queryCount`/`callsByAction` resets it first, since
// QtQuickTest does not run test functions in declaration order (it runs
// them alphabetically), so an earlier-alphabetical test may run after a
// later-declared one.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "CollectionView"

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
                replyReceived(actionType, true,
                    JSON.stringify({ rows: [
                        { id: 1, name: "First", amount: { num: 15, den: 10, dp: 1 } },
                        { id: 2, name: "Second", amount: { num: 30, den: 10, dp: 1 } }
                    ] }))
                return
            }
            replyReceived(actionType, true, JSON.stringify({ ok: true }))
        }

        function fetchOptions(optionsAction, bodyJson) {
            optionsReceived(optionsAction, true, "[]")
        }
    }

    property var testView: ({
        "v-kind": "collection",
        "v-title": "Rows",
        "v-query": "ListRows",
        "v-rowKey": "id",
        "v-columns": [
            { "field": "id", "label": "ID", "v-hidden": true },
            { "field": "name", "label": "Name" },
            { "field": "amount", "label": "Amount", "x-decimalPlaces": 1,
              "ExtUnits": { "unitAscii": "kg", "unitUnicode": "kg" } }
        ],
        "v-rowAction": { "action": "EditRow", "bind": { "id": "id" } },
        "v-actions": [
            { "action": "DeleteRow", "label": "Delete", "scope": "row", "bind": { "id": "id" }, "confirm": true },
            { "action": "CreateRow", "label": "New", "scope": "collection" }
        ]
    })

    property var testSchemas: ({
        "EditRow": {
            properties: {
                id: { type: "integer", "x-order": 0 },
                name: { type: "string", "x-order": 1 }
            },
            required: ["id", "name"]
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

    function test_populatesOnCompletion() {
        mockController.queryCount = 0
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)
        compare(mockController.queryCount, 1)
        compare(view.rows.length, 2)
    }

    function test_derivedColumnsRenderAndFormatQuantity() {
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)
        var cell = findChild(view, "cell_amount_1")
        verify(cell !== null)
        compare(cell.text, "1.5 kg")
    }

    function test_openRowPrefillsEditorFromBoundFieldsOnly() {
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)
        var openButton = findChild(view, "rowOpen_1")
        verify(openButton !== null)
        openButton.clicked()
        compare(view.editorRow.id, 1)
        compare(view.editorRow.name, "First")

        var dialog = findChild(view, "editorDialog")
        verify(dialog !== null)
        // A Dialog's declared content lives under its `contentItem`, not
        // directly among its own QObject children -- findChild must be
        // rooted there to reach into the nested DynamicForm's fields.
        var idField = findChild(dialog.contentItem, "field_id")
        verify(idField !== null)
        compare(idField.text, "1")
        // "name" is NOT in v-rowAction's bind (only "id" is, matching
        // docs/spec/forms/views.md's own example) — it starts blank rather
        // than showing the row's current name.
        var nameField = findChild(dialog.contentItem, "field_name")
        verify(nameField !== null)
        compare(nameField.text, "")
    }

    function test_openingRowAloneDoesNotAutoFireEdit() {
        // Regression guard: if bind covered every required field of the
        // row-opener action, prefill alone would make it "ready" and
        // DynamicForm's no-submit-button auto-fire would submit EditRow the
        // instant the row opens, before the user changes anything. Binding
        // only "id" (the row key) keeps "name" blank so no auto-fire happens
        // until the user actually types a value.
        mockController.queryCount = 0
        mockController.callsByAction = ({})
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)
        mockController.queryCount = 0
        mockController.callsByAction = ({})

        var openButton = findChild(view, "rowOpen_1")
        verify(openButton !== null)
        openButton.clicked()

        compare(mockController.callsByAction["EditRow"], undefined)
        compare(mockController.queryCount, 0)
        verify(view.editorRow !== null)   // the editor is open, not already closed by an auto-fire
    }

    function test_editingAfterOpenFiresAndRepopulates() {
        mockController.queryCount = 0
        mockController.callsByAction = ({})
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)
        mockController.queryCount = 0

        var openButton = findChild(view, "rowOpen_1")
        verify(openButton !== null)
        openButton.clicked()

        var dialog = findChild(view, "editorDialog")
        verify(dialog !== null)
        var nameField = findChild(dialog.contentItem, "field_name")
        verify(nameField !== null)
        nameField.text = "Renamed"   // both required fields now engaged -> auto-fires

        compare(mockController.callsByAction["EditRow"], '{"id":1,"name":"Renamed"}')
        compare(view.editorRow, null)          // closed once the edit resolved
        compare(mockController.queryCount, 1)  // and the table re-populated
    }

    function test_confirmedDeleteFiresBoundActionAndRepopulates() {
        mockController.queryCount = 0
        mockController.callsByAction = ({})
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)
        mockController.queryCount = 0

        var deleteButton = findChild(view, "rowAction_DeleteRow_1")
        verify(deleteButton !== null)
        deleteButton.clicked()

        var dialog = findChild(view, "confirmDialog")
        verify(dialog !== null)
        verify(dialog.visible)
        dialog.accept()

        compare(mockController.callsByAction["DeleteRow"], '{"id":1}')
        compare(mockController.queryCount, 1)   // re-populated after the delete resolved
    }

    function test_collectionActionFiresWithEmptyBody() {
        mockController.callsByAction = ({})
        var view = createTemporaryObject(viewComponent, testCase)
        verify(view !== null)
        var newButton = findChild(view, "collectionAction_CreateRow")
        verify(newButton !== null)
        newButton.clicked()
        compare(mockController.callsByAction["CreateRow"], "{}")
    }
}
