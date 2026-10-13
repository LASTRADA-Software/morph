// SPDX-License-Identifier: Apache-2.0
//
// The `form` node: a document's schema form, lowered through the forms engine (spec 5 §9, spec 2).

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>
#include <string_view>

#include "ui_interpreter_support.hpp"

namespace {

using morph::ui::testing::Harness;
using morph::ui::testing::readJson;

constexpr std::string_view kCapture = R"({
  "vocab": "ui/1", "id": "capture",
  "params": { "sampleKey": "int?" },
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "saved": { "init": 0 }, "lastError": { "init": null } },
  "view": { "kind": "column", "children": [
    { "kind": "form", "id": "cap", "testId": "capture", "model": "sample", "action": "Lims_CaptureConcentration",
      "prefill": { "if": [ { "fn": "engaged", "args": [ { "ref": "sampleKey" } ] },
                           { "obj": { "sampleId": { "ref": "sampleKey" } } }, null ] },
      "fields": { "rowIndex": { "omit": true },
                  "note": { "label": { "t": "capture.note", "fallback": "Remarks" } } },
      "onSuccess": [ { "set": "saved", "value": { "fn": "add", "args": [ { "ref": "saved" }, 1 ] } } ],
      "onError": [ { "set": "lastError", "value": { "ref": "event" } } ] },
    { "kind": "text", "testId": "ready", "text": { "if": [ { "ref": "cap.ready" }, "ready", "not ready" ] } },
    { "kind": "text", "testId": "draft", "text": { "ref": "cap.sampleId.draft" } },
    { "kind": "text", "testId": "saved", "text": { "fn": "format", "args": [ "{0}", { "ref": "saved" } ] } } ] }
})";

}  // namespace

TEST_CASE("form node: the form is drawn from the action's schema, with the document's overlay",
          "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(kCapture);
    INFO((h.diagnostics.empty() ? std::string{} : h.diagnostics.front().message()));
    CHECK(h.diagnostics.empty());
    CHECK(h.backend.find("TextInput", "testId", "field_sampleId").has_value());
    CHECK(h.backend.find("TextInput", "testId", "field_concentration").has_value());
    CHECK_FALSE(h.backend.find("TextInput", "testId", "field_rowIndex").has_value());  // omitted
    CHECK(h.text("label_note") == "Remarks");                                          // the overlay's label
    CHECK(h.backend.find("Column", "testId", "capture").has_value());                  // the node's own testId
    CHECK(h.text("ready") == "not ready");
}

TEST_CASE("form node: typing submits through the model alias, and the form's values are elements",
          "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(kCapture);
    h.backend.edit(h.byTestId("TextInput", "field_sampleId"), "9007199254740993");
    h.settle();
    CHECK(h.text("draft") == "9007199254740993");
    CHECK(h.dispatcher.count("Lims_CaptureConcentration") == 0);  // concentration is still missing
    h.backend.edit(h.byTestId("TextInput", "field_concentration"), "12.5");
    h.settle();
    CHECK(h.text("ready") == "ready");
    REQUIRE(h.dispatcher.count("Lims_CaptureConcentration") == 1);
    auto const& call = h.dispatcher.calls.at(h.dispatcher.last("Lims_CaptureConcentration"));
    CHECK(call.typeId == "Lims_SampleModel");
    CHECK(call.body == R"({"sampleId":9007199254740993,"concentration":{"num":1250,"den":100,"dp":2}})");
    CHECK(readJson(*screen, R"({"ref":"cap.concentration.value"})") == R"({"num":25,"den":2,"dp":2})");
    CHECK(readJson(*screen, R"({"ref":"cap.submitted"})") == "true");
    CHECK(readJson(*screen, R"({"ref":"cap.body.sampleId"})") == "9007199254740993");
    CHECK(h.text("saved") == "0");
    h.reply(h.dispatcher.last("Lims_CaptureConcentration"), R"({"ok":true})");
    CHECK(h.text("saved") == "1");  // onSuccess ran once
}

TEST_CASE("form node: a failure runs onError with its text", "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(kCapture);
    h.backend.edit(h.byTestId("TextInput", "field_sampleId"), "4");
    h.backend.edit(h.byTestId("TextInput", "field_concentration"), "1");
    h.settle();
    h.fail(h.dispatcher.last("Lims_CaptureConcentration"));
    CHECK(readJson(*screen, R"({"ref":"lastError"})") == R"("it failed")");
    CHECK(readJson(*screen, R"({"ref":"saved"})") == "0");
}

TEST_CASE("form node: the prefill follows its expression and never submits", "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(kCapture, {{"sampleKey", morph::ui::Value{std::int64_t{42}}}});
    CHECK(h.backend.prop(h.byTestId("TextInput", "field_sampleId"), "text") == "42");
    CHECK(h.dispatcher.count("Lims_CaptureConcentration") == 0);
}

TEST_CASE("form node: an explicit form submits only from its button", "[ui][interpreter][forms]") {
    Harness h;
    std::string document{kCapture};
    document.replace(document.find(R"("prefill")"), 0, R"("submit": "explicit", )");
    auto const screen = h.mount(document);
    h.backend.edit(h.byTestId("TextInput", "field_sampleId"), "4");
    h.backend.edit(h.byTestId("TextInput", "field_concentration"), "1");
    h.settle();
    CHECK(h.dispatcher.count("Lims_CaptureConcentration") == 0);
    h.backend.click(h.byTestId("Button", "form_submit"));
    h.settle();
    CHECK(h.dispatcher.count("Lims_CaptureConcentration") == 1);
}

TEST_CASE("form node: an action the catalog has no schema for is reported and shown muted",
          "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "noschema",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "form", "model": "sample", "action": "Lims_Unknown" } })");
    REQUIRE_FALSE(h.diagnostics.empty());
    CHECK(h.diagnostics.front().reason.find("no schema") != std::string::npos);
    CHECK(h.backend.find("Text", "text", "Lims_Unknown").has_value());
}
