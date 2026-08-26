// SPDX-License-Identifier: Apache-2.0
//
// The renderer half of the per-instance constraints corpus (morph#164).
//
// `schemaJson<A>()` is a pure function of the compiled action type, so a form
// whose *definition* is data -- a versioned analysis catalogue whose version 1
// declares three decimal places and whose version 2 declares one -- could not
// vary any key the framework enforces. `morph::forms::InstanceConstraints` is
// the seam: one declaration both decorates the served schema and checks a
// submitted value, so the served bound and the checked bound cannot drift.
//
// That is only worth anything if the shipped renderer honours what was served.
// It honoured `x-decimalPlaces` already; `x-minimum`/`x-maximum` it ignored
// outright, so an instance's specification range was a second opinion no
// renderer read -- the two-values-for-one-concept outcome the seam exists to
// remove, reintroduced inside the fix for it.
//
// `data/instance_bounds.json` is one file with two readers: this one, and
// `tests/test_forms_instance_constraints.cpp`, which rebuilds the same
// `InstanceConstraints` from the file's `constraints` block, asserts the
// decorated schema below is what that declaration actually emits (byte for
// byte), and asserts `checkValue` reaches the same verdict for every row.
//
// The `compiled/` rows are the control: the same values against the
// *undecorated* schema, where they are accepted. Without them, a renderer that
// rejected everything would pass this file.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormInstanceBounds"
    visible: true
    width: 400
    height: 600

    property var corpusCache: null

    // Bound into the component rather than passed through
    // createTemporaryObject's initial properties, which converts the object via
    // QVariantMap and leaves `Array.isArray(p.type)` false.
    property var pendingSchema: ({})

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)
        property int submitCount: 0
        function submitIfValid(actionType, bodyJson) {
            submitCount += 1
            replyReceived(actionType, true, '{"ok":true}')
        }
        function fetchOptions(optionsAction) { optionsReceived(optionsAction, true, "[]") }
    }

    Component {
        id: formComponent
        DynamicForm {
            actionType: "Test_ICCapture"
            controller: mockController
            schema: testCase.pendingSchema
        }
    }

    // Handed over verbatim by tst_main.cpp; see the comment there.
    function corpus() {
        if (testCase.corpusCache !== null)
            return testCase.corpusCache
        verify(typeof instanceBoundsJson === "string" && instanceBoundsJson.length > 0,
               "data/instance_bounds.json is unreadable or empty")
        testCase.corpusCache = JSON.parse(instanceBoundsJson)
        return testCase.corpusCache
    }

    function test_eachRowGetsTheVerdictTheDeclarationChecks_data() {
        var rows = []
        var cases = corpus().cases
        for (var i = 0; i < cases.length; ++i)
            rows.push({tag: cases[i].id, row: cases[i]})
        return rows
    }

    function test_eachRowGetsTheVerdictTheDeclarationChecks(data) {
        var row = data.row
        var schemaText = corpus().schemas[row.schema]
        verify(schemaText !== undefined, "corpus names an unknown schema: " + row.schema)
        // Parsed from text, exactly as an app parses controller.schemasJson.
        testCase.pendingSchema = JSON.parse(schemaText)

        var form = createTemporaryObject(formComponent, testCase)
        verify(form !== null)
        for (var name in row.state) {
            var control = findChild(form, "field_" + name)
            verify(control !== null, "no control named field_" + name)
            control.text = row.state[name]
        }
        compare(form.ready, row.ready,
                row.id + ": InstanceConstraints::checkValue reaches the opposite verdict")
    }

    // The decorated schema advertises the instance's precision, not the
    // compiled type's. Asserted separately from `ready` because it is the half
    // that already worked -- a regression guard, not a new claim.
    function test_theDecoratedSchemaDrivesTheEntryGranularity() {
        testCase.pendingSchema = JSON.parse(corpus().schemas["decorated"])
        var form = createTemporaryObject(formComponent, testCase)
        compare(form.fieldByName["value"].decimals, 1)

        testCase.pendingSchema = JSON.parse(corpus().schemas["compiled"])
        var plain = createTemporaryObject(formComponent, testCase)
        compare(plain.fieldByName["value"].decimals, 3)
    }

    function test_theCorpusItselfCarriesBothVerdicts() {
        var cases = corpus().cases
        var allowed = 0
        for (var i = 0; i < cases.length; ++i) {
            if (cases[i].ready)
                allowed++
        }
        verify(allowed > 0, "no row expects a submittable form")
        verify(allowed < cases.length, "no row expects a blocked form")
    }
}
