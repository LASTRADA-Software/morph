// SPDX-License-Identifier: Apache-2.0
//
// The `form` node: a document's schema form, lowered through the forms engine (spec 5 §9, spec 2).

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
        CHECK(h.diagnostics.front().reason.contains("document for"));
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

namespace {

// A query holding `T_Row` rows beside a form, a collection or a wizard: their submits apply the action's effects, so
// `Lims_CaptureConcentration` (`updates T_Row`) patches the row it replies, and `Lims_DeleteSample` (`changes T_Row`)
// refetches the query.
std::string refreshedBy(std::string_view node) {
    return std::string{R"({
  "vocab": "ui/1", "id": "refreshed",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "rows": { "model": "sample", "action": "Push_ListRows" } },
  "view": { "kind": "column", "children": [ { "kind": "text", "text": { "fn": "format", "args": [ "{0}", { "query": "rows" } ] } }, )"} +
           std::string{node} + " ] } }";
}

constexpr char const* kOneRow = R"([{"code":"a","name":"x"}])";
constexpr char const* kCaptured = R"({"code":"a","name":"captured"})";

}  // namespace

// Mutation: leave the submits out of `actionSucceeded` — no row is patched and no delete refetches.
TEST_CASE("form, collection and wizard nodes: a submit that succeeds applies its action's effects",
          "[ui][interpreter][forms]") {
    Harness h;
    SECTION("a form") {
        auto const screen = h.mount(refreshedBy(
            R"({ "kind": "form", "model": "sample", "action": "Lims_CaptureConcentration", "submit": "explicit" })"));
        h.reply(h.dispatcher.last("Push_ListRows"), kOneRow);
        h.backend.edit(h.byTestId("TextInput", "field_sampleId"), "4");
        h.backend.edit(h.byTestId("TextInput", "field_concentration"), "1");
        h.settle();
        h.backend.click(h.byTestId("Button", "form_submit"));
        h.settle();
        h.fail(h.dispatcher.last("Lims_CaptureConcentration"));
        CHECK(readJson(*screen, R"({"query": "rows"})") == kOneRow);  // a failure is not a success
        h.backend.click(h.byTestId("Button", "form_submit"));
        h.settle();
        h.reply(h.dispatcher.last("Lims_CaptureConcentration"), kCaptured);
        CHECK(readJson(*screen, R"({"query": "rows"})") == R"([{"code":"a","name":"captured"}])");
        CHECK(h.dispatcher.count("Push_ListRows") == 1);
    }
    SECTION("a collection, whose own list query is a read, not a submit") {
        auto const screen =
            h.mount(refreshedBy(R"({ "kind": "collection", "model": "sample", "action": "Lims_ListSamples" })"));
        h.reply(h.dispatcher.last("Push_ListRows"), kOneRow);
        h.reply(h.dispatcher.last("Lims_ListSamples"), R"({"rows":[{"id":7,"name":"Water"}]})");
        CHECK(h.dispatcher.count("Push_ListRows") == 1);
        h.backend.click(h.byTestId("Button", "rowAction_Lims_DeleteSample_7"));
        h.settle();
        h.backend.click(h.byTestId("Button", "confirm_accept"));
        h.settle();
        h.reply(h.dispatcher.last("Lims_DeleteSample"), "{}");
        CHECK(h.dispatcher.count("Push_ListRows") == 2);
        // The collection reloads its list after the delete; that read refetches nothing.
        h.reply(h.dispatcher.last("Lims_ListSamples"), R"({"rows":[]})");
        CHECK(h.dispatcher.count("Push_ListRows") == 2);
    }
    SECTION("a wizard's step") {
        auto const screen =
            h.mount(refreshedBy(R"({ "kind": "wizard", "model": "sample", "action": "Lims_CaptureConcentration" })"));
        h.reply(h.dispatcher.last("Push_ListRows"), kOneRow);
        h.backend.edit(h.byTestId("TextInput", "field_sampleId"), "4");
        h.backend.edit(h.byTestId("TextInput", "field_concentration"), "1");
        h.settle();
        h.reply(h.dispatcher.last("Lims_CaptureConcentration"), kCaptured);
        CHECK(readJson(*screen, R"({"query": "rows"})") == R"([{"code":"a","name":"captured"}])");
    }
}

namespace {

// A form prefilled from a query of the record chosen by state `id`; KEY is the form's `key`, or nothing.
std::string followed(std::string_view key) {
    return std::string{R"({
  "vocab": "ui/1", "id": "followed",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "id": { "init": 1 } },
  "queries": { "rec": { "model": "sample", "action": "Lims_ListResults", "body": { "obj": { "id": { "ref": "id" } } } } },
  "view": { "kind": "column", "children": [
    { "kind": "textInput", "testId": "pick", "onChange": [ { "set": "id", "value": { "ref": "event" } } ] },
    { "kind": "form", "model": "sample", "action": "Lims_CaptureConcentration", "submit": "explicit",
      "prefill": { "query": "rec" })"} +
           std::string{key} + " } ] } }";
}

}  // namespace

// Mutation: make the prefill effect always call FormSession::prefill -> the late first value discards "typed".
TEST_CASE("form node: a value of the record being edited keeps what the user typed; another record loads every field",
          "[ui][interpreter][forms]") {
    Harness h;
    auto const note = [&h] { return h.backend.prop(h.byTestId("TextInput", "field_note"), "text"); };
    auto const row = [&h] { return h.backend.prop(h.byTestId("TextInput", "field_rowIndex"), "text"); };
    SECTION("with a key: the same key keeps the edit, a new key reloads") {
        auto const screen = h.mount(followed(R"(, "key": { "ref": "id" })"));
        h.backend.edit(h.byTestId("TextInput", "field_note"), "typed");
        h.settle();
        h.reply(h.dispatcher.last("Lims_ListResults"), R"({"sampleId":1,"note":"stored","rowIndex":1})");
        CHECK(note() == "typed");
        CHECK(row() == "1");
        h.session.backendChanged("primary");  // a refetch of the same record
        h.settle();
        h.reply(h.dispatcher.last("Lims_ListResults"), R"({"sampleId":1,"note":"stored again","rowIndex":2})");
        CHECK(note() == "typed");
        CHECK(row() == "2");
        h.backend.edit(h.byTestId("TextInput", "pick"), "2");
        h.settle();
        h.reply(h.dispatcher.last("Lims_ListResults"), R"({"sampleId":2,"note":"other","rowIndex":3})");
        CHECK(note() == "other");
        CHECK(row() == "3");
    }
    SECTION("without a key: only the first value keeps the edit") {
        auto const screen = h.mount(followed(""));
        h.backend.edit(h.byTestId("TextInput", "field_note"), "typed");
        h.settle();
        h.reply(h.dispatcher.last("Lims_ListResults"), R"({"sampleId":1,"note":"stored","rowIndex":1})");
        CHECK(note() == "typed");
        h.session.backendChanged("primary");
        h.settle();
        h.reply(h.dispatcher.last("Lims_ListResults"), R"({"sampleId":1,"note":"stored again","rowIndex":2})");
        CHECK(note() == "stored again");
    }
}

// Mutation: build a form, collection or wizard from a catalog document that does not build (it throws, or shows an
// empty engine instead of the report).
TEST_CASE("form, collection and wizard nodes: a catalog document that does not build is reported",
          "[ui][interpreter][forms]") {
    Harness h;
    morph::ui::Catalog catalog = morph::ui::testing::corpusCatalog();
    auto const corpusSchema = catalog.schema;
    catalog.schema = [corpusSchema](std::string_view action) -> std::optional<std::string> {
        if (action == "Lims_CaptureConcentration") {
            return std::string{"{not a schema"};
        }
        return corpusSchema(action);
    };
    catalog.view = [](std::string_view) -> std::optional<std::string> { return std::string{R"({"v-kind":"nope"})"}; };
    catalog.wizard = [](std::string_view action) -> std::optional<std::string> {
        if (action == "Lims_ConfirmSample") {
            return std::string{R"({"w-title":"Flow","w-steps":[{"action":"Lims_CaptureConcentration","title":"A"}]})"};
        }
        return std::string{R"({"w-steps":"none"})"};
    };
    morph::ui::Session session{h.runtime, morph::ui::SessionOptions{.scheduler = &h.scheduler,
                                                                    .clock = [&h] { return h.scheduler.now(); },
                                                                    .keepUnused = std::chrono::minutes{5},
                                                                    .catalog = catalog,
                                                                    .classify = {}}};
    std::vector<std::string> reasons;
    morph::ui::Interpreter interpreter{
        session, h.dispatcher,
        morph::ui::InterpreterOptions{.host = &h.host, .diagnostics = [&](morph::ui::Refusal const& refusal) {
                                          reasons.push_back(refusal.reason);
                                      }}};
    auto const screen = interpreter.mount(morph::ui::testing::loadOrFail(R"({"vocab": "ui/1", "id": "broken",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "column", "children": [
        { "kind": "form", "model": "sample", "action": "Lims_CaptureConcentration" },
        { "kind": "collection", "model": "sample", "action": "Lims_ListSamples" },
        { "kind": "wizard", "model": "sample", "action": "Lims_CaptureConcentration" },
        { "kind": "wizard", "model": "sample", "action": "Lims_ConfirmSample" } ] } })",
                                                                         morph::ui::testing::corpusCatalog()),
                                          {}, h.backend);
    REQUIRE(screen.has_value());
    h.settle();
    REQUIRE(reasons.size() == 4);
    CHECK(reasons.at(0).starts_with("the form of `Lims_CaptureConcentration` does not build: "));
    CHECK(reasons.at(1).starts_with("the collection of `Lims_ListSamples` does not build: "));
    CHECK(reasons.at(2).starts_with("the wizard of `Lims_CaptureConcentration` does not build: "));
    CHECK(reasons.at(3) == "the wizard document of `Lims_ConfirmSample` starts with `Lims_CaptureConcentration`");
    CHECK(h.dispatcher.calls.empty());
}

// Mutation: tell the inputs their label is never shown (the standalone inputs report `captionShown=false`).
TEST_CASE("an input shows its label as a caption only when the document asks; a form's cell draws its own",
          "[ui][interpreter][forms]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "captions",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "column", "children": [
        { "kind": "textInput", "testId": "shownText", "label": "Name", "showLabel": true },
        { "kind": "select", "testId": "shownSelect", "label": "Grade", "showLabel": true },
        { "kind": "dateTimeInput", "testId": "shownWhen", "label": "When", "showLabel": true },
        { "kind": "textInput", "testId": "named", "label": "Hidden caption" },
        { "kind": "textInput", "testId": "bare" },
        { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_CaptureConcentration" } ] } })");
    auto const caption = [&h](std::string_view kind, std::string_view testId) {
        int const widget = h.byTestId(kind, testId);
        return h.backend.prop(widget, "caption") + "|" + h.backend.prop(widget, "captionShown");
    };
    CHECK(caption("TextInput", "shownText") == "Name|true");
    CHECK(caption("Select", "shownSelect") == "Grade|true");
    CHECK(caption("DateTimeInput", "shownWhen") == "When|true");
    CHECK(caption("TextInput", "named") == "Hidden caption|false");  // the accessible name, not drawn
    CHECK_FALSE(h.backend.hasProp(h.byTestId("TextInput", "bare"), "caption"));
    // In a form the cell draws the caption, and the input is told not to.
    CHECK(caption("TextInput", "field_sampleId") == "Sample|false");
    CHECK(h.text("label_sampleId") == "Sample");
}

namespace {

/// A form whose list field is drawn by the host component `ReadingsGrid`, given a prop of the document's beside the
/// field's own.
constexpr std::string_view kReadings = R"({
  "vocab": "ui/1", "id": "readings",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "components": { "ReadingsGrid": { "host": "ReadingsGrid", "version": 1,
    "props": { "value": "json", "label": "string", "required": "bool", "errors": "json", "columns": "int" },
    "events": { "change": "any" }, "fallback": { "kind": "text", "text": "no grid" } } },
  "view": { "kind": "column", "children": [
    { "kind": "form", "id": "rec", "model": "sample", "action": "Lims_RecordReadings", "submit": "explicit",
      "prefill": { "lit": { "sampleId": 7, "readings": [ { "at": "a", "value": 1 } ] } },
      "fields": { "readings": { "component": "ReadingsGrid", "props": { "columns": 2 } } } },
    { "kind": "text", "testId": "rows", "text": { "fn": "count", "args": [ { "ref": "rec.readings.value" } ] } } ] }
})";

}  // namespace

// Mutation: skip the override (the field keeps its built-in rows, and no component is asked for).
TEST_CASE("form node: a field drawn by a host component gets its value and writes it back as a user edit",
          "[ui][interpreter][forms]") {
    Harness h;
    h.backend.loadsComponents({"ReadingsGrid"});
    auto const screen = h.mount(kReadings);
    h.settle();
    INFO((h.diagnostics.empty() ? std::string{} : h.diagnostics.front().message()));
    std::optional<int> const grid = h.backend.find("Custom", "component", "ReadingsGrid");
    REQUIRE(grid.has_value());
    CHECK(h.backend.prop(*grid, "prop.value") == R"([{"at":"a","value":1}])");
    CHECK(h.backend.prop(*grid, "prop.label") == "Readings");
    CHECK(h.backend.prop(*grid, "prop.columns") == "2");
    CHECK(h.text("rows") == "1");

    h.backend.raiseEvent(*grid, "change", R"([{"at":"a","value":1},{"at":"b","value":2}])");
    h.settle();
    CHECK(h.text("rows") == "2");
    CHECK(h.backend.prop(*grid, "prop.value") == R"([{"at":"a","value":1},{"at":"b","value":2}])");

    std::size_t const reported = h.diagnostics.size();
    h.backend.raiseEvent(*grid, "change", R"({"at":"c"})");
    h.backend.raiseEvent(*grid, "change", "not json");
    h.settle();
    CHECK(h.text("rows") == "2");
    REQUIRE(h.diagnostics.size() == reported + 2);
    CHECK(h.diagnostics.at(reported).reason ==
          "field readings: the component's change does not decode: a list field takes a JSON array");
    CHECK(h.diagnostics.at(reported + 1).reason ==
          "field readings: the component's change does not decode: it is "
          "not JSON");

    h.backend.click(h.byTestId("Button", "form_submit"));
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_RecordReadings") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_RecordReadings")).body ==
          R"({"sampleId":7,"readings":[{"at":"a","value":1},{"at":"b","value":2}]})");
}

TEST_CASE("form node: without the component loaded, the field's built-in control is drawn",
          "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(kReadings);
    h.settle();
    CHECK_FALSE(h.backend.find("Custom", "component", "ReadingsGrid").has_value());
    CHECK(h.backend.find("Button", "testId", "add_readings").has_value());
}
