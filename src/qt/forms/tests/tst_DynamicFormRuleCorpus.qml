// SPDX-License-Identifier: Apache-2.0
//
// The renderer half of the shared `x-rules` corpus (morph#176).
//
// `x-rules` is evaluated twice -- compiled by `morph::forms::allRulesSatisfied`
// and again in JavaScript by DynamicForm.qml -- and nothing structural pinned
// the two to each other. A hand-mirrored pair of test files does not pin them:
// adding a rule kind to one side leaves the other silently untested, which is
// how `atLeastOneOf` and `mutuallyExclusive` could both be disabled
// client-side with the whole renderer suite still green.
//
// The pin is `data/rule_corpus.json`: ONE file, TWO readers. This file drives
// every row through a real DynamicForm; `tests/test_forms_rule_corpus.cpp`
// drives the same rows through the compiled evaluator and additionally asserts
// that (1) each corpus schema is the current `schemaJson<A>()` byte-for-byte,
// (2) every one of the 16 `detail::RuleKind` values appears somewhere in the
// corpus, and (3) each kind carries both verdicts. Neither file owns the
// cases, so the two evaluators cannot be pinned to different case lists.
//
// The corpus stores each schema as JSON *text*, and this file feeds that text
// through JSON.parse exactly as a shipped app does with
// `controller.schemasJson`. That is not incidental: parsing is what rounds an
// int64 literal past 2^53, so a fixture built from a QML object literal (or
// re-serialised through JSON.stringify on the way in) could not express the
// `equals-int64` rows at all -- it would pass against the very bug it exists
// to catch.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "DynamicFormRuleCorpus"
    visible: true
    width: 400
    height: 600

    property var corpusCache: null

    // The schema the next createTemporaryObject() picks up. A `property var`
    // assigned a JS value, then *bound* into the component below rather than
    // handed to createTemporaryObject as an initial-properties entry. That
    // distinction used to matter -- the initial-properties path converts the
    // object through QVariantMap, and `DynamicForm` typed each field by asking
    // `Array.isArray` about the result -- but morph#388 made the renderer
    // re-read the schema as JSON at the property, so both paths now render the
    // same form. Binding stays because it is what every shipped app does, and
    // because re-assigning `pendingSchema` per corpus row re-renders on its own.
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
            controller: mockController
            schema: testCase.pendingSchema
        }
    }

    // The corpus text, handed over verbatim by tst_main.cpp (see the comment
    // there for why the file is not fetched from QML). Parsed once.
    function corpus() {
        if (testCase.corpusCache !== null)
            return testCase.corpusCache
        verify(typeof ruleCorpusJson === "string" && ruleCorpusJson.length > 0,
               "data/rule_corpus.json is unreadable or empty")
        testCase.corpusCache = JSON.parse(ruleCorpusJson)
        return testCase.corpusCache
    }

    // Applies one corpus row's field state to a live form, using the control
    // each field's declared JSON type actually renders as. A field absent from
    // `state` is left untouched, which is what "unengaged" means here.
    function applyState(form, parsedSchema, state) {
        for (var name in state) {
            var control = findChild(form, "field_" + name)
            verify(control !== null, "no control named field_" + name)
            var property = parsedSchema.properties[name]
            var types = Array.isArray(property.type) ? property.type
                        : (property.type === undefined ? [] : [property.type])
            if (types.indexOf("boolean") !== -1) {
                // The CheckBox writes "true"/"false" through onToggled; setting
                // `checked` alone would not reach the draft.
                control.checked = (state[name] === "true")
                control.toggled()
            } else {
                control.text = state[name]
            }
        }
    }

    function test_corpusRowsAgreeWithTheCompiledEvaluator_data() {
        var rows = []
        var cases = corpus().cases
        for (var i = 0; i < cases.length; ++i) {
            var row = cases[i]
            rows.push({tag: row.id, row: row})
        }
        return rows
    }

    function test_corpusRowsAgreeWithTheCompiledEvaluator(data) {
        var row = data.row
        var schemaText = corpus().schemas[row.action]
        verify(schemaText !== undefined, "corpus names an unknown action: " + row.action)
        // Parsed from text, exactly as an app parses controller.schemasJson.
        var parsedSchema = JSON.parse(schemaText)

        testCase.pendingSchema = parsedSchema
        var form = createTemporaryObject(formComponent, testCase, {actionType: "Test_" + row.action})
        verify(form !== null)
        applyState(form, parsedSchema, row.state)

        compare(form.ready, row.ready,
                row.id + ": the compiled evaluator says allRulesSatisfied == " + row.ready)

        // Presentation kinds never gate, so `ready` alone cannot tell whether
        // visibleWhen/readonlyWhen did anything. These are the rows that make
        // those two kinds observable at all.
        if (row.visible !== undefined) {
            for (var vname in row.visible) {
                var vcolumn = findChild(form, "column_" + vname)
                verify(vcolumn !== null, "no column named column_" + vname)
                compare(vcolumn.visible, row.visible[vname], row.id + ": visibility of " + vname)
            }
        }
        if (row.readonly !== undefined) {
            for (var rname in row.readonly) {
                var rcolumn = findChild(form, "column_" + rname)
                verify(rcolumn !== null, "no column named column_" + rname)
                compare(rcolumn.enabled, !row.readonly[rname], row.id + ": editability of " + rname)
            }
        }
    }

    // A corpus every one of whose rows expects `ready: true` would pass against
    // a renderer that never blocks anything, and a corpus of all-false rows
    // would pass against one that blocks everything. The C++ half asserts the
    // stronger per-kind version of this; here it is the cheap whole-corpus
    // guard, so neither reader can be handed a degenerate file unnoticed.
    function test_theCorpusItselfCarriesBothVerdicts() {
        var cases = corpus().cases
        var allowed = 0
        for (var i = 0; i < cases.length; ++i) {
            if (cases[i].ready)
                allowed++
        }
        verify(cases.length > 0)
        verify(allowed > 0, "no corpus row expects a submittable form")
        verify(allowed < cases.length, "no corpus row expects a blocked form")
    }
}
