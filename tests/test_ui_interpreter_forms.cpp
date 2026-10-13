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
    CHECK(h.diagnostics.front().reason.contains("no schema"));
    CHECK(h.backend.find("Text", "text", "Lims_Unknown").has_value());
}

namespace {

constexpr std::string_view kSamples = R"({
  "vocab": "ui/1", "id": "samples",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "view": { "kind": "collection", "testId": "samples", "model": "sample", "action": "Lims_ListSamples" } })";

constexpr std::string_view kCaptureFlow = R"({
  "vocab": "ui/1", "id": "flow",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "view": { "kind": "wizard", "testId": "flow", "model": "sample", "action": "Lims_CaptureConcentration" } })";

}  // namespace

TEST_CASE("collection node: the catalog's view document lists rows through the model alias",
          "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(kSamples);
    INFO((h.diagnostics.empty() ? std::string{} : h.diagnostics.front().message()));
    CHECK(h.diagnostics.empty());
    CHECK(h.backend.find("Column", "testId", "samples").has_value());  // the node's own testId
    REQUIRE(h.dispatcher.count("Lims_ListSamples") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_ListSamples")).typeId == "Lims_SampleModel");
    h.reply(h.dispatcher.last("Lims_ListSamples"), R"({"rows":[{"id":9007199254740993,"name":"Water"}]})");
    CHECK(h.text("collection_title") == "Samples");
    CHECK(h.text("cell_name_9007199254740993") == "Water");

    // A confirmed delete goes through the alias with the row's exact key, then the list reloads.
    h.backend.click(h.byTestId("Button", "rowAction_Lims_DeleteSample_9007199254740993"));
    h.settle();
    CHECK(h.dispatcher.count("Lims_DeleteSample") == 0);
    CHECK(h.backend.prop(h.byTestId("Dialog", "confirmDialog"), "open") == "true");
    h.backend.click(h.byTestId("Button", "confirm_accept"));
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_DeleteSample") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_DeleteSample")).body == R"({"id":9007199254740993})");
    h.reply(h.dispatcher.last("Lims_DeleteSample"), "{}");
    CHECK(h.dispatcher.count("Lims_ListSamples") == 2);
    h.reply(h.dispatcher.last("Lims_ListSamples"), R"({"rows":[{"id":9007199254740993,"name":"Water"}]})");

    // The row editor is the capture form, prefilled from the row and submitted through the alias.
    h.backend.click(h.byTestId("Button", "rowOpen_9007199254740993"));
    h.settle();
    CHECK(h.backend.prop(h.byTestId("Dialog", "editorDialog"), "open") == "true");
    CHECK(h.backend.prop(h.byTestId("TextInput", "field_sampleId"), "text") == "9007199254740993");
    h.backend.edit(h.byTestId("TextInput", "field_concentration"), "2");
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_CaptureConcentration") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_CaptureConcentration")).body ==
          R"({"sampleId":9007199254740993,"concentration":{"num":200,"den":100,"dp":2}})");
    h.reply(h.dispatcher.last("Lims_CaptureConcentration"), R"({"ok":true})");
    CHECK(h.backend.prop(h.byTestId("Dialog", "editorDialog"), "open") == "false");
    CHECK(h.dispatcher.count("Lims_ListSamples") == 3);
}

TEST_CASE("wizard node: the catalog's wizard document steps through the model alias", "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(kCaptureFlow);
    INFO((h.diagnostics.empty() ? std::string{} : h.diagnostics.front().message()));
    CHECK(h.diagnostics.empty());
    CHECK(h.backend.find("Column", "testId", "flow").has_value());
    CHECK(h.text("wizard_title") == "Capture flow  (1 / 2)");
    CHECK(h.backend.prop(h.byTestId("Button", "wizardNext"), "enabled") == "false");
    h.backend.edit(h.byTestId("TextInput", "field_sampleId"), "9007199254740993");
    h.backend.edit(h.byTestId("TextInput", "field_concentration"), "1");
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_CaptureConcentration") == 1);
    h.reply(h.dispatcher.last("Lims_CaptureConcentration"), R"({"ok":true})");
    CHECK(h.backend.prop(h.byTestId("Button", "wizardNext"), "enabled") == "true");
    h.backend.click(h.byTestId("Button", "wizardNext"));
    h.settle();
    CHECK(h.text("wizard_title") == "Capture flow  (2 / 2)");
    // The second step's prefill is the first step's body, exact; ready as prefilled, it submits once.
    CHECK(h.backend.prop(h.byTestId("TextInput", "field_sampleId"), "text") == "9007199254740993");
    REQUIRE(h.dispatcher.count("Lims_ConfirmSample") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_ConfirmSample")).body == R"({"sampleId":9007199254740993})");
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_ConfirmSample")).typeId == "Lims_SampleModel");
}

TEST_CASE("collection and wizard nodes: a document the catalog lacks is reported and shown muted",
          "[ui][interpreter][forms]") {
    for (std::string_view const document : {
             R"({"vocab": "ui/1", "id": "a", "models": { "sample": { "type": "Lims_SampleModel" } },
                 "view": { "kind": "collection", "model": "sample", "action": "Lims_ListResults" } })",
             R"({"vocab": "ui/1", "id": "b", "models": { "sample": { "type": "Lims_SampleModel" } },
                 "view": { "kind": "wizard", "model": "sample", "action": "Lims_ConfirmSample" } })",
         }) {
        Harness h;
        auto const screen = h.mount(document);
        REQUIRE(h.diagnostics.size() == 1);
        CHECK(h.diagnostics.front().reason.find("document for") != std::string::npos);
        CHECK(h.backend.all("Table").empty());
        CHECK(h.backend.all("Button").empty());
    }
}

TEST_CASE("collection node: a view document listing another action is reported", "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(R"({"vocab": "ui/1", "id": "stale",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "collection", "model": "sample", "action": "Lims_ListStale" } })");
    REQUIRE(h.diagnostics.size() == 1);
    CHECK(h.diagnostics.front().reason == "the view document of `Lims_ListStale` lists `Lims_ListSamples`");
    CHECK(h.dispatcher.calls.empty());
    CHECK(h.backend.find("Text", "text", "Lims_ListStale").has_value());
}
