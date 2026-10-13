// SPDX-License-Identifier: Apache-2.0
//
// The body corpus (tests/data/body_corpus.json): for every case, the exact
// body a real DynamicForm assembles from the typed text. The forms engine's
// test (tests/test_forms_engine_corpora.cpp) requires the same bytes, so the
// expected bodies are this renderer's output, not hand-written strings.
//
// With MORPH_FORMS_WRITE_BODY_CORPUS set, every case's body is recorded and
// the file is rewritten at the end; without it, each case must produce the
// body the file holds. tst_main.cpp hands the text over and writes it back.

import QtQuick
import QtTest
import MorphForms

TestCase {
    id: testCase
    name: "BodyCorpus"

    property var corpusCache: null
    property var pendingSchema: ({})
    property var recorded: ({})

    QtObject {
        id: mockController
        signal replyReceived(string actionType, bool ok, string payload)
        signal optionsReceived(string optionsAction, bool ok, string payload)
        function submitIfValid(actionType, bodyJson) {}
        function fetchOptions(optionsAction, bodyJson) {}
    }

    Component {
        id: formComponent
        DynamicForm {
            actionType: "BodyCorpus"
            controller: mockController
            schema: testCase.pendingSchema
        }
    }

    function corpus() {
        if (testCase.corpusCache !== null)
            return testCase.corpusCache
        verify(typeof bodyCorpusJson === "string" && bodyCorpusJson.length > 0,
               "tests/data/body_corpus.json is unreadable or empty")
        testCase.corpusCache = JSON.parse(bodyCorpusJson)
        return testCase.corpusCache
    }

    function test_bodies_data() {
        var rows = []
        var cases = corpus().cases
        for (var i = 0; i < cases.length; ++i)
            rows.push({tag: cases[i].id, row: cases[i]})
        verify(rows.length > 0, "the body corpus has no cases")
        return rows
    }

    function test_bodies(data) {
        var row = data.row
        // Parsed from text, as an application parses the schemas it is served.
        testCase.pendingSchema = JSON.parse(row.schema)
        var form = createTemporaryObject(formComponent, testCase,
                                         {displayLocale: row.locale, displayOffsetMinutes: row.offset})
        verify(form !== null)
        if (row.prefill !== undefined)
            verify(form.prefillFromJson(row.prefill), row.id + ": the prefill does not load")
        for (var i = 0; i < row.steps.length; ++i) {
            var step = row.steps[i]
            if (step.unit !== undefined)
                form.fieldUnits[step.field] = step.unit
            form.setFieldValue(step.field, step.text)
        }
        var body = form.ready ? form.previewLine : null
        if (bodyCorpusRegenerate) {
            testCase.recorded[row.id] = body
            return
        }
        compare(body, row.body, row.id)
    }

    function cleanupTestCase() {
        if (!bodyCorpusRegenerate)
            return
        var file = corpus()
        for (var i = 0; i < file.cases.length; ++i) {
            var id = file.cases[i].id
            verify(id in testCase.recorded, id + ": no body was recorded")
            file.cases[i].body = testCase.recorded[id]
        }
        verify(bodyCorpusFile.write(JSON.stringify(file, null, 2) + "\n"), "the body corpus could not be written")
    }
}
