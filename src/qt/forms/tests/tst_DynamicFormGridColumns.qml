// SPDX-License-Identifier: Apache-2.0
//
// DynamicForm.gridColumns: the number of columns of every field grid the form
// builds, so a host can lay fields out on a grid of its own (12 columns, say)
// with x-colspan. The default keeps the renderer's own layout -- two columns
// per section and tab set, one for the implicit flat bucket -- and a span is
// clamped to the grid it sits in. Asserted on the grids' `columns`, the
// delegates' spans, and where the fields actually land.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormGridColumns"
    visible: true
    width: 800
    height: 900
    when: windowShown

    // No x-layout: one implicit flat bucket.
    property var flatSchema: ({
        properties: {
            a: { type: ["integer", "null"], "x-order": 0, title: "A", "x-colspan": 6 },
            b: { type: ["integer", "null"], "x-order": 1, title: "B", "x-colspan": 6 },
            c: { type: ["integer", "null"], "x-order": 2, title: "C", "x-colspan": 20 },
            d: { type: ["integer", "null"], "x-order": 3, title: "D" }
        },
        required: []
    })

    property var layoutSchema: ({
        properties: {
            a: { type: ["integer", "null"], "x-order": 0, "x-section": 0, "x-colspan": 4 },
            b: { type: ["integer", "null"], "x-order": 1, "x-section": 0, "x-colspan": 8 },
            c: { type: ["integer", "null"], "x-order": 2, "x-section": 1, "x-colspan": 3 },
            d: { type: ["integer", "null"], "x-order": 3, "x-section": 2, "x-colspan": 2 },
            e: { type: ["integer", "null"], "x-order": 4 }
        },
        required: [],
        "x-layout": {
            groups: [
                { title: "Identity", kind: "section", fields: ["a", "b"] },
                { title: "One", kind: "tab", fields: ["c"] },
                { title: "Two", kind: "tab", fields: ["d"] }
            ]
        }
    })

    Component {
        id: flatForm
        DynamicForm { width: 780; actionType: "T_Flat"; schema: testCase.flatSchema; controller: null }
    }

    Component {
        id: layoutForm
        DynamicForm { width: 780; actionType: "T_Layout"; schema: testCase.layoutSchema; controller: null }
    }

    Component {
        id: registryComponent
        SlotRegistry {}
    }

    Component {
        id: sectionChrome
        ColumnLayout {
            objectName: "sectionChrome"
            property string title
            property string kind
            property var section
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
            ColumnLayout { id: body; Layout.fillWidth: true }
        }
    }

    function column(form, name) {
        const item = findChild(form, "column_" + name)
        verify(item !== null, "no delegate for " + name)
        return item
    }

    function gridOf(form, name) {
        return column(form, name).parent
    }

    // ── the default: unchanged ───────────────────────────────────────────────

    function test_the_default_keeps_one_flat_column_and_two_per_section() {
        const flat = createTemporaryObject(flatForm, testCase)
        compare(flat.gridColumns, 2)
        compare(flat.flatGridColumns, 1)
        compare(gridOf(flat, "a").columns, 1)

        const sectioned = createTemporaryObject(layoutForm, testCase)
        compare(gridOf(sectioned, "a").objectName, "sectionGrid")
        compare(gridOf(sectioned, "a").columns, 2)
        compare(gridOf(sectioned, "c").objectName, "tabGrid")
        compare(gridOf(sectioned, "c").columns, 2)
        // The trailing group of unnamed fields is the flat bucket.
        compare(gridOf(sectioned, "e").columns, 1)
    }

    function test_a_span_is_clamped_to_the_grid() {
        const sectioned = createTemporaryObject(layoutForm, testCase)
        compare(column(sectioned, "a").Layout.columnSpan, 2)
        compare(column(sectioned, "b").Layout.columnSpan, 2)
        const flat = createTemporaryObject(flatForm, testCase)
        compare(column(flat, "a").Layout.columnSpan, 1)
        compare(column(flat, "d").Layout.columnSpan, 1)
    }

    // ── a host's grid ────────────────────────────────────────────────────────

    function test_a_host_grid_applies_to_the_flat_form() {
        const form = createTemporaryObject(flatForm, testCase, { gridColumns: 12 })
        compare(form.flatGridColumns, 12)
        compare(gridOf(form, "a").columns, 12)
        compare(column(form, "a").Layout.columnSpan, 6)
        compare(column(form, "c").Layout.columnSpan, 12)  // 20, clamped
        compare(column(form, "d").Layout.columnSpan, 1)
        // Two half-width fields share a row; the clamped one takes the next.
        tryVerify(function () { return column(form, "b").x > column(form, "a").x })
        compare(column(form, "a").y, column(form, "b").y)
        verify(column(form, "c").y > column(form, "a").y)
        verify(column(form, "c").width > column(form, "a").width)
    }

    function test_a_host_grid_applies_to_sections_and_tab_sets() {
        const form = createTemporaryObject(layoutForm, testCase, { gridColumns: 12 })
        compare(gridOf(form, "a").columns, 12)
        compare(column(form, "a").Layout.columnSpan, 4)
        compare(column(form, "b").Layout.columnSpan, 8)
        compare(gridOf(form, "c").columns, 12)
        compare(column(form, "c").Layout.columnSpan, 3)
        compare(gridOf(form, "e").columns, 12)
        tryVerify(function () { return column(form, "b").x > column(form, "a").x })
        compare(column(form, "a").y, column(form, "b").y)
    }

    function test_the_flat_bucket_can_be_set_on_its_own() {
        const form = createTemporaryObject(flatForm, testCase, { flatGridColumns: 2 })
        compare(form.gridColumns, 2)
        compare(gridOf(form, "a").columns, 2)
        compare(column(form, "a").Layout.columnSpan, 2)
    }

    function test_changing_the_grid_relays_the_form() {
        const form = createTemporaryObject(layoutForm, testCase)
        compare(column(form, "b").Layout.columnSpan, 2)
        form.gridColumns = 12
        compare(gridOf(form, "a").columns, 12)
        compare(column(form, "b").Layout.columnSpan, 8)
    }

    // ── grids created inside a host's chrome ─────────────────────────────────

    function test_a_host_grid_applies_inside_section_and_tabset_chrome() {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byChrome("section", sectionChrome)
        registry.byChrome("tabset", tabsetChrome)
        const form = createTemporaryObject(layoutForm, testCase, { slotRegistry: registry, gridColumns: 12 })
        verify(findChild(form, "sectionChrome") !== null)
        compare(gridOf(form, "a").objectName, "chromeFieldGrid")
        compare(gridOf(form, "a").columns, 12)
        compare(column(form, "b").Layout.columnSpan, 8)
        compare(gridOf(form, "c").objectName, "chromeFieldGrid")
        compare(gridOf(form, "c").columns, 12)
        compare(column(form, "c").Layout.columnSpan, 3)
    }

    function test_the_default_grid_inside_chrome_is_two_columns() {
        const registry = createTemporaryObject(registryComponent, testCase)
        registry.byChrome("section", sectionChrome)
        const form = createTemporaryObject(layoutForm, testCase, { slotRegistry: registry })
        compare(gridOf(form, "a").objectName, "chromeFieldGrid")
        compare(gridOf(form, "a").columns, 2)
        compare(column(form, "b").Layout.columnSpan, 2)
    }
}
