// SPDX-License-Identifier: Apache-2.0
//
// Chrome slots for CollectionView and WizardView: the same byChrome registry
// DynamicForm reads, with roles of their own, and the registry handed on to
// every DynamicForm they embed -- so a screen built from a host's UI kit shows
// no built-in Label, Button or Dialog anywhere, the editors included.
//
// Each case asserts what the chrome was handed, that the built-in it replaces
// is gone, and that driving the chrome drives the view (the same controller
// calls the built-in buttons make).

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "ViewChrome"
    visible: true
    width: 800
    height: 600

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        property var calls: []
        property var resolvedValues: ({})
        function submitIfValid(actionType, bodyJson) {
            calls.push(actionType + " " + bodyJson)
            if (actionType === "ListRows") {
                replyReceived(actionType, true, JSON.stringify({ rows: [
                    { id: 1, name: "First", amount: { num: 15, den: 10, dp: 1 } },
                    { id: 2, name: "Second", amount: { num: 30, den: 10, dp: 1 } }
                ] }))
                return
            }
            if (actionType === "WizStepOne")
                resolvedValues["WizStepOne.id"] = "1"
            replyReceived(actionType, true, '{"ok":true}')
        }
        function resolvedValue(path) { return resolvedValues[path] !== undefined ? resolvedValues[path] : "" }
    }

    property var testView: ({
        "v-kind": "collection", "v-title": "Rows", "v-query": "ListRows", "v-rowKey": "id",
        "v-columns": [
            { field: "id", label: "ID", "v-hidden": true },
            { field: "name", label: "Name" },
            { field: "amount", label: "Amount", "x-decimalPlaces": 1, ExtUnits: { unitAscii: "kg", unitUnicode: "kg" } }
        ],
        "v-rowAction": { action: "EditRow", bind: { id: "id" } },
        "v-actions": [
            { action: "DeleteRow", label: "Delete", scope: "row", bind: { id: "id" }, confirm: true },
            { action: "CreateRow", label: "New", scope: "collection" }
        ]
    })

    property var testSchemas: ({
        EditRow: { properties: { id: { type: "integer", "x-order": 0 }, name: { type: "string", "x-order": 1 } },
                   required: ["id", "name"] },
        WizStepOne: { properties: { label: { type: "string", "x-order": 0 } }, required: ["label"] },
        WizStepTwo: { properties: { refId: { type: "integer", "x-order": 0 } }, required: ["refId"] }
    })

    property var testWizard: ({
        "w-title": "Test flow",
        "w-steps": [ { action: "WizStepOne", title: "One" },
                     { action: "WizStepTwo", title: "Two", prefill: { refId: "WizStepOne.id" } } ]
    })

    // ── chrome ───────────────────────────────────────────────────────────────

    Component { id: headerChrome; Item { objectName: "headerChrome"; property string title; property var columns; property var actions; property var fire } }
    Component {
        id: rowChrome
        Item {
            objectName: "rowChrome_" + rowKey
            property var row; property var columns; property var cells; property string rowKey
            property var actions; property bool canOpen; property var open; property var fire
        }
    }
    Component { id: confirmChrome; Item { objectName: "confirmChrome"; property string message; property var action; property var row; property var accept; property var reject } }
    Component {
        id: editorChrome
        ColumnLayout {
            objectName: "editorChrome"
            property string title; property bool open; property var close
            property alias contentItem: body
            ColumnLayout { id: body; objectName: "editorBody" }
        }
    }
    Component { id: wizardHeaderChrome; Item { objectName: "wizardHeaderChrome"; property string title; property int stepIndex; property int stepCount; property string stepTitle } }
    Component { id: wizardNavChrome; Item { objectName: "wizardNavChrome"; property bool canBack; property bool canNext; property bool lastStep; property var back; property var next } }
    Component { id: emptyChrome; Item {} }
    Component { id: fieldLabelChrome; Item { objectName: "fieldLabelChrome"; property string text } }

    Component { id: registryComponent; SlotRegistry {} }

    Component {
        id: collectionComponent
        CollectionView { viewId: "V"; view: testCase.testView; schemas: testCase.testSchemas; controller: mockController }
    }

    Component {
        id: wizardComponent
        WizardView { wizardId: "W"; wizardSchema: testCase.testWizard; schemas: testCase.testSchemas; controller: mockController }
    }

    function registryWith(chromes) {
        const registry = createTemporaryObject(registryComponent, testCase)
        for (const role in chromes)
            registry.byChrome(role, chromes[role])
        return registry
    }

    function visibleBuiltins(item, out) {
        if (!item || item.visible === false)
            return out
        if (item instanceof Label || item instanceof Button || item instanceof TabBar)
            out.push(item)
        const kids = item.children || []
        for (let i = 0; i < kids.length; ++i)
            visibleBuiltins(kids[i], out)
        return out
    }

    // ── CollectionView ───────────────────────────────────────────────────────

    function test_collection_header_and_rows_go_through_chrome() {
        mockController.calls = []
        const view = createTemporaryObject(collectionComponent, testCase,
                                           { slotRegistry: registryWith({ collectionHeader: headerChrome, collectionRow: rowChrome }) })
        const header = findChild(view, "headerChrome")
        compare(header.title, "Rows")
        compare(header.columns.length, 2)
        compare(header.actions[0].action, "CreateRow")
        const row = findChild(view, "rowChrome_1")
        verify(row !== null)
        compare(row.cells[1], "1.5 kg")
        compare(row.canOpen, true)
        compare(row.actions[0].action, "DeleteRow")
        // Built-ins are gone: no cell labels, no Open buttons.
        compare(findChild(view, "cell_amount_1"), null)
        compare(findChild(view, "rowOpen_1"), null)
        header.fire(header.actions[0])
        verify(mockController.calls.indexOf("CreateRow {}") !== -1)
    }

    function test_confirm_chrome_holds_the_action_until_accepted() {
        mockController.calls = []
        const view = createTemporaryObject(collectionComponent, testCase,
                                           { slotRegistry: registryWith({ collectionRow: rowChrome, confirmDialog: confirmChrome }) })
        const row = findChild(view, "rowChrome_2")
        row.fire(row.actions[0])
        const confirm = findChild(view, "confirmChrome")
        verify(confirm !== null)
        compare(confirm.message, "Are you sure?")
        compare(confirm.action.action, "DeleteRow")
        verify(mockController.calls.every(function (c) { return c.indexOf("DeleteRow") !== 0 }))
        confirm.accept()
        verify(mockController.calls.indexOf('DeleteRow {"id":2}') !== -1)
        // An unloaded chrome is destroyed on the next event-loop turn.
        tryVerify(function () { return findChild(view, "confirmChrome") === null })
        verify(!findChild(view, "confirmDialog").visible)
    }

    function test_confirm_chrome_reject_fires_nothing() {
        mockController.calls = []
        const view = createTemporaryObject(collectionComponent, testCase,
                                           { slotRegistry: registryWith({ collectionRow: rowChrome, confirmDialog: confirmChrome }) })
        const row = findChild(view, "rowChrome_1")
        row.fire(row.actions[0])
        findChild(view, "confirmChrome").reject()
        verify(mockController.calls.every(function (c) { return c.indexOf("DeleteRow") !== 0 }))
        // An unloaded chrome is destroyed on the next event-loop turn.
        tryVerify(function () { return findChild(view, "confirmChrome") === null })
    }

    function test_editor_chrome_hosts_the_prefilled_editor() {
        const view = createTemporaryObject(collectionComponent, testCase,
                                           { slotRegistry: registryWith({ collectionRow: rowChrome, editorDialog: editorChrome }) })
        const editor = findChild(view, "editorChrome")
        verify(editor !== null)
        compare(editor.open, false)
        findChild(view, "rowChrome_2").open()
        compare(editor.open, true)
        compare(editor.title, "EditRow")
        // The editor form lives inside the chrome, prefilled from the row.
        const idField = findChild(findChild(editor, "editorBody"), "field_id")
        verify(idField !== null)
        compare(idField.text, "2")
        verify(!findChild(view, "editorDialog").visible)
        editor.close()
        compare(editor.open, false)
    }

    function test_the_registry_reaches_the_embedded_editor_forms() {
        const view = createTemporaryObject(collectionComponent, testCase,
                                           { slotRegistry: registryWith({ fieldLabel: fieldLabelChrome, editorDialog: editorChrome }) })
        verify(findChild(findChild(view, "editorBody"), "fieldLabelChrome") !== null)
    }

    function test_a_fully_chromed_collection_shows_no_built_in() {
        const view = createTemporaryObject(collectionComponent, testCase, {
            slotRegistry: registryWith({ collectionHeader: headerChrome, collectionRow: rowChrome,
                                         confirmDialog: confirmChrome, editorDialog: editorChrome,
                                         fieldLabel: fieldLabelChrome, header: emptyChrome, status: emptyChrome,
                                         preview: emptyChrome, result: emptyChrome })
        })
        findChild(view, "rowChrome_1").open()
        const left = visibleBuiltins(view, [])
        compare(left.length, 0, "still built-in: " + left.map(function (i) { return i.toString() }).join(", "))
    }

    // ── WizardView ───────────────────────────────────────────────────────────

    function test_wizard_header_and_nav_go_through_chrome() {
        const wizard = createTemporaryObject(wizardComponent, testCase,
                                             { slotRegistry: registryWith({ wizardHeader: wizardHeaderChrome, wizardNav: wizardNavChrome }) })
        const header = findChild(wizard, "wizardHeaderChrome")
        const nav = findChild(wizard, "wizardNavChrome")
        compare(header.title, "Test flow")
        compare(header.stepCount, 2)
        compare(header.stepTitle, "One")
        compare(nav.canBack, false)
        compare(nav.canNext, false)
        verify(!findChild(wizard, "wizardNext").visible)
        // Next is gated exactly as the built-in button is: nothing happens
        // until the step's action has replied ok.
        nav.next()
        compare(wizard.currentIndex, 0)
        findChild(wizard.currentForm(), "field_label").text = "x"
        tryCompare(nav, "canNext", true)
        nav.next()
        compare(wizard.currentIndex, 1)
        compare(header.stepIndex, 1)
        compare(header.stepTitle, "Two")
        compare(nav.lastStep, true)
        compare(nav.canBack, true)
        nav.back()
        compare(wizard.currentIndex, 0)
    }

    function test_the_registry_reaches_every_wizard_step() {
        const wizard = createTemporaryObject(wizardComponent, testCase,
                                             { slotRegistry: registryWith({ fieldLabel: fieldLabelChrome }) })
        verify(findChild(wizard.currentForm(), "fieldLabelChrome") !== null)
    }

    // ── defaults unchanged ───────────────────────────────────────────────────

    function test_without_a_registry_the_built_ins_are_drawn() {
        const view = createTemporaryObject(collectionComponent, testCase)
        verify(findChild(view, "cell_amount_1") !== null)
        verify(findChild(view, "rowOpen_1") !== null)
        const wizard = createTemporaryObject(wizardComponent, testCase)
        verify(findChild(wizard, "wizardNext").visible)
    }
}
