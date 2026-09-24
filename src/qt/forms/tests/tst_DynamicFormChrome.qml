// SPDX-License-Identifier: Apache-2.0
//
// Chrome slots: a host replaces the form's own chrome -- field label, help
// text, section / accordion / tab-set containers, header, status line, submit
// button, preview and result -- through SlotRegistry.byChrome, so a form built
// from a host's UI kit shows no built-in Label or Button of its own.
//
// Each case asserts both halves: the chrome is loaded and handed its values,
// *and* the built-in it replaces is gone -- a chrome drawn beside a still-
// visible built-in is exactly the half-restyled form this seam exists to end.
// The fields a container chrome hosts are asserted to be created once, inside
// it: a second, hidden copy would claim the same objectName.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormChrome"
    visible: true
    width: 600
    height: 800

    QtObject {
        id: recordingController
        property int submissions: 0
        property string lastBody: ""
        signal replyReceived(string actionType, bool ok, string payload)
        function submitIfValid(actionType, bodyJson) {
            submissions++
            lastBody = bodyJson
            replyReceived(actionType, true, '{"ok":true}')
        }
    }

    property var flatSchema: ({
        properties: {
            count: { type: "integer", "x-order": 0, title: "Count", description: "How many" },
            note: { type: ["string", "null"], "x-order": 1, title: "Note" }
        },
        required: ["count"]
    })

    property var explicitSchema: ({
        properties: { count: { type: "integer", "x-order": 0, title: "Count" } },
        required: ["count"],
        "x-submitMode": "explicit"
    })

    property var layoutSchema: ({
        properties: {
            a: { type: ["integer", "null"], "x-order": 0, "x-section": 0 },
            b: { type: ["integer", "null"], "x-order": 1, "x-section": 1 },
            c: { type: ["integer", "null"], "x-order": 2, "x-section": 2 },
            d: { type: ["integer", "null"], "x-order": 3, "x-section": 3 }
        },
        required: [],
        "x-layout": {
            groups: [
                { title: "Identity", kind: "section", fields: ["a"] },
                { title: "More", kind: "accordion", fields: ["b"] },
                { title: "One", kind: "tab", fields: ["c"] },
                { title: "Two", kind: "tab", fields: ["d"] }
            ]
        }
    })

    // ── chrome components, each recording what it was handed ─────────────────

    Component {
        id: labelChrome
        Item {
            objectName: "labelChrome"
            property var field
            property string text
            property bool required
            property bool invalid
            property var form
        }
    }

    Component {
        id: helpChrome
        Item {
            objectName: "helpChrome"
            property string text
        }
    }

    Component {
        id: sectionChrome
        ColumnLayout {
            objectName: "sectionChrome"
            property string title
            property string kind
            property var section
            property alias contentItem: body
            ColumnLayout { id: body; objectName: "sectionBody"; Layout.fillWidth: true }
        }
    }

    Component {
        id: accordionChrome
        ColumnLayout {
            objectName: "accordionChrome"
            property string title
            property alias contentItem: body
            ColumnLayout { id: body; Layout.fillWidth: true }
        }
    }

    Component {
        id: tabsetChrome
        ColumnLayout {
            objectName: "tabsetChrome"
            property var tabs: []
            property int currentIndex: 0
            property alias contentItem: body
            ColumnLayout { id: body; objectName: "tabsetBody"; Layout.fillWidth: true }
        }
    }

    Component {
        id: headerChrome
        Item { objectName: "headerChrome"; property string text }
    }

    Component {
        id: statusChrome
        Item {
            objectName: "statusChrome"
            property string text
            property bool ready
            property string reason
            property bool explicitSubmit
        }
    }

    Component {
        id: submitChrome
        Item {
            objectName: "submitChrome"
            property bool ready
            property var submit
        }
    }

    Component {
        id: previewChrome
        Item { objectName: "previewChrome"; property string text }
    }

    Component {
        id: resultChrome
        Item { objectName: "resultChrome"; property string text; property bool ok }
    }

    Component {
        id: registryComponent
        SlotRegistry {}
    }

    Component {
        id: formComponent
        DynamicForm { actionType: "T_Chrome"; controller: null }
    }

    function makeForm(schema, chromes, controller) {
        const registry = createTemporaryObject(registryComponent, testCase)
        for (const role in chromes)
            registry.byChrome(role, chromes[role])
        return createTemporaryObject(formComponent, testCase,
                                     { schema: schema, slotRegistry: registry, controller: controller || null })
    }

    // Every visible Label or Button under `item` -- what a fully restyled form
    // must not contain.
    function visibleBuiltins(item, out) {
        if (!item)
            return out
        if (item.visible === false)
            return out
        if (item instanceof Label || item instanceof Button || item instanceof TabBar)
            out.push(item)
        const kids = item.children || []
        for (let i = 0; i < kids.length; ++i)
            visibleBuiltins(kids[i], out)
        return out
    }

    function countNamed(item, name) {
        if (!item)
            return 0
        let n = item.objectName === name ? 1 : 0
        const kids = item.children || []
        for (let i = 0; i < kids.length; ++i)
            n += countNamed(kids[i], name)
        return n
    }

    // ── registry ─────────────────────────────────────────────────────────────

    function test_resolve_chrome_misses_to_null_and_accordion_falls_back() {
        const registry = createTemporaryObject(registryComponent, testCase)
        compare(registry.resolveChrome("section"), null)
        registry.byChrome("section", sectionChrome)
        compare(registry.resolveChrome("section"), sectionChrome)
        compare(registry.resolveChrome("accordion"), sectionChrome)
        registry.byChrome("accordion", accordionChrome)
        compare(registry.resolveChrome("accordion"), accordionChrome)
        compare(registry.resolveChrome("tabset"), null)
    }

    // ── defaults unchanged ───────────────────────────────────────────────────

    function test_without_chrome_the_built_ins_are_drawn() {
        const form = createTemporaryObject(formComponent, testCase, { schema: testCase.flatSchema })
        verify(visibleBuiltins(form, []).length > 0)
        compare(findChild(form, "labelChrome"), null)
    }

    // ── field chrome ─────────────────────────────────────────────────────────

    function test_label_chrome_gets_the_caption_and_a_live_required_and_invalid_state() {
        const form = makeForm(testCase.flatSchema, { fieldLabel: labelChrome })
        const first = findChild(form, "labelChrome")
        verify(first !== null)
        compare(first.field.name, "count")
        compare(first.text, "Count")
        compare(first.required, true)
        compare(first.invalid, false)
        verify(first.form === form)
        findChild(form, "field_count").text = "abc"
        compare(first.invalid, true)
        findChild(form, "field_count").text = "3"
        compare(first.invalid, false)
        compare(countNamed(form, "labelChrome"), 2)
    }

    function test_help_chrome_is_loaded_only_where_there_is_help() {
        const form = makeForm(testCase.flatSchema, { fieldHelp: helpChrome })
        compare(countNamed(form, "helpChrome"), 1)
        compare(findChild(form, "helpChrome").text, "How many")
    }

    // ── container chrome ─────────────────────────────────────────────────────

    function test_section_chrome_hosts_its_fields_once() {
        const form = makeForm(testCase.layoutSchema, { section: sectionChrome })
        const first = findChild(form, "sectionChrome")
        verify(first !== null)
        compare(first.title, "Identity")
        compare(first.kind, "section")
        // The field sits inside the chrome, and nowhere else.
        verify(findChild(first, "field_a") !== null)
        compare(countNamed(form, "field_a"), 1)
        // The accordion falls back to the section chrome.
        compare(countNamed(form, "sectionChrome"), 2)
        compare(countNamed(form, "field_b"), 1)
    }

    function test_an_accordion_chrome_of_its_own_wins() {
        const form = makeForm(testCase.layoutSchema, { section: sectionChrome, accordion: accordionChrome })
        const accordion = findChild(form, "accordionChrome")
        verify(accordion !== null)
        compare(accordion.title, "More")
        verify(findChild(accordion, "field_b") !== null)
        compare(countNamed(form, "sectionChrome"), 1)
    }

    function test_a_field_inside_a_chrome_still_drives_the_form() {
        const form = makeForm(testCase.layoutSchema, { section: sectionChrome })
        findChild(form, "field_a").text = "5"
        compare(form.ready, true)
        compare(form.previewLine, '{"a":5}')
    }

    function test_tabset_chrome_owns_the_tabs_and_the_selection() {
        const form = makeForm(testCase.layoutSchema, { tabset: tabsetChrome })
        const tabs = findChild(form, "tabsetChrome")
        verify(tabs !== null)
        compare(tabs.tabs.length, 2)
        compare(tabs.tabs[0].title, "One")
        compare(tabs.tabs[1].title, "Two")
        compare(countNamed(form, "tabBar"), 1)          // the built-in, hidden
        verify(!findChild(form, "tabBar").visible)
        verify(findChild(tabs, "field_c") !== null)
        compare(countNamed(form, "field_d"), 0)
        tabs.currentIndex = 1
        tryVerify(function () { return findChild(tabs, "field_d") !== null })
        compare(countNamed(form, "field_c"), 0)
    }

    // ── form chrome ──────────────────────────────────────────────────────────

    function test_header_status_preview_and_result_chrome() {
        const form = makeForm(testCase.flatSchema,
                              { header: headerChrome, status: statusChrome, preview: previewChrome,
                                result: resultChrome },
                              recordingController)
        compare(findChild(form, "headerChrome").text, "T_Chrome")
        const status = findChild(form, "statusChrome")
        compare(status.ready, false)
        compare(status.text, "fill the required (*) fields")
        findChild(form, "field_count").text = "4"
        compare(status.ready, true)
        compare(status.text, form.statusText)
        compare(findChild(form, "previewChrome").text, '{"count":4}')
        // Auto-submit fired; the reply reaches the result chrome.
        compare(findChild(form, "resultChrome").ok, true)
        compare(findChild(form, "resultChrome").text, '{"ok":true}')
    }

    function test_submit_chrome_fires_through_the_forms_own_gate() {
        recordingController.submissions = 0
        const form = makeForm(testCase.explicitSchema, { submitButton: submitChrome }, recordingController)
        const submit = findChild(form, "submitChrome")
        verify(submit !== null)
        compare(findChild(form, "submitButton"), null)
        compare(submit.ready, false)
        submit.submit()
        compare(recordingController.submissions, 0)     // not ready: the gate holds
        findChild(form, "field_count").text = "2"
        compare(submit.ready, true)
        submit.submit()
        compare(recordingController.submissions, 1)
        compare(recordingController.lastBody, '{"count":2}')
    }

    // ── all of it: nothing built-in left ─────────────────────────────────────

    function test_a_fully_chromed_form_shows_no_built_in_label_or_button() {
        const form = makeForm(testCase.layoutSchema,
                              { fieldLabel: labelChrome, fieldHelp: helpChrome, section: sectionChrome,
                                tabset: tabsetChrome, header: headerChrome, status: statusChrome,
                                preview: previewChrome, result: resultChrome })
        const left = visibleBuiltins(form, [])
        compare(left.length, 0, "still built-in: " + left.map(function (i) { return i.toString() }).join(", "))
    }
}
