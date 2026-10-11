// SPDX-License-Identifier: Apache-2.0

// The relations corpus of spec 5 §14: cascading watches with `source: "user"` against a server write, check
// targets and `ready`, the `showErrors` policies, dependent options with `onStale`, derived values, and the cycle
// refusal.

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;
using ui::testing::readJson;

namespace {

constexpr char const* kWatch = R"({
  "vocab": "ui/1", "id": "watch",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "pick": { "init": "a" }, "unit": { "init": "mg" }, "dilution": { "init": 2 }, "anyRuns": { "init": 0 } },
  "mutations": { "load": { "model": "sample", "action": "Lims_Save",
                           "onSuccess": [ { "set": "pick", "value": { "result": "load" } } ] } },
  "watch": [
    { "on": { "ref": "pick" }, "source": "user", "do": [ { "set": "unit", "value": null } ] },
    { "on": { "ref": "unit" }, "source": "user", "do": [ { "set": "dilution", "value": null } ] },
    { "on": { "ref": "pick" }, "do": [ { "set": "anyRuns", "value": { "fn": "add", "args": [ { "ref": "anyRuns" }, 1 ] } } ] } ],
  "view": { "kind": "column", "children": [
    { "kind": "select", "testId": "pick", "options": { "lit": [ "a", "b", "c" ] }, "selected": { "ref": "pick" },
      "onChoose": [ { "set": "pick", "value": { "ref": "event" } } ] },
    { "kind": "button", "testId": "load", "label": "load", "onClick": { "run": "load" } } ] } })";

}  // namespace

TEST_CASE("interpreter relations: a user watch cascades on the user's edit and not on a server write",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kWatch);
    CHECK(readJson(*screen, R"({"ref": "anyRuns"})") == "0");

    SECTION("a server write runs only the `any` watch") {
        h.backend.click(h.byTestId("Button", "load"));
        h.settle();
        h.reply(h.dispatcher.last("Lims_Save"), R"("b")");
        CHECK(readJson(*screen, R"({"ref": "pick"})") == R"("b")");
        CHECK(readJson(*screen, R"({"ref": "unit"})") == R"("mg")");
        CHECK(readJson(*screen, R"({"ref": "dilution"})") == "2");
        CHECK(readJson(*screen, R"({"ref": "anyRuns"})") == "1");
    }
    SECTION("the user's choice cascades through both user watches") {
        h.backend.choose(h.byTestId("Select", "pick"), ui::Key{std::string{"c"}});
        h.settle();
        CHECK(readJson(*screen, R"({"ref": "pick"})") == R"("c")");
        CHECK(readJson(*screen, R"({"ref": "unit"})") == "null");
        CHECK(readJson(*screen, R"({"ref": "dilution"})") == "null");
        CHECK(readJson(*screen, R"({"ref": "anyRuns"})") == "1");
    }
}

TEST_CASE("interpreter relations: a watch cycle is refused at load", "[ui][interpreter]") {
    auto refused = ui::loadDocumentText(R"({
      "vocab": "ui/1", "id": "cycle", "state": { "a": { "init": 0 }, "b": { "init": 0 } },
      "watch": [ { "on": { "ref": "a" }, "do": [ { "set": "b", "value": 1 } ] },
                 { "on": { "ref": "b" }, "do": [ { "set": "a", "value": 1 } ] } ],
      "view": { "kind": "spacer" } })");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().reason.find("cycle") != std::string::npos);
}

namespace {

constexpr char const* kDialog = R"({
  "vocab": "ui/1", "id": "dialog",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "limit": { "init": 5 } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "open", "label": "open", "onClick": { "open": "dlg" } },
    { "kind": "text", "testId": "ready", "text": { "fn": "format", "args": [ "{0}", { "ref": "dlg.ready" } ] } },
    { "kind": "text", "testId": "submitted", "text": { "fn": "format", "args": [ "{0}", { "ref": "dlg.submitted" } ] } },
    { "kind": "dialog", "id": "dlg", "title": "Capture", "showErrors": "POLICY",
      "scope": { "mutations": { "save": { "model": "sample", "action": "Lims_Save", "body": { "obj": { "dilution": { "ref": "dilution.value" } } } } },
                 "checks": [ { "id": "range", "assert": { "fn": "le", "args": [ { "ref": "dilution.value" }, { "ref": "limit" } ] },
                               "message": "above the limit", "targets": [ "dilution" ], "blocking": true } ] },
      "children": [
        { "kind": "slider", "id": "dilution", "testId": "dilution", "required": true, "min": 0, "max": 10 },
        { "kind": "text", "testId": "valid", "text": { "fn": "format", "args": [ "{0}", { "ref": "dilution.valid" } ] } },
        { "kind": "textInput", "id": "note", "testId": "note", "required": true },
        { "kind": "button", "testId": "save", "label": "save", "onClick": { "run": "save" } } ] } ] } })";

std::string dialog(std::string const& policy) {
    std::string text = kDialog;
    text.replace(text.find("POLICY"), 6, policy);
    return text;
}

}  // namespace

TEST_CASE("interpreter relations: checks target elements and decide a dialog's ready", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(dialog("always"));
    CHECK(h.text("ready") == "false");
    h.backend.click(h.byTestId("Button", "open"));
    h.settle();
    int const slider = h.byTestId("Slider", "dilution");
    CHECK(h.backend.prop(slider, "errors") == "[Required]");
    h.backend.slide(slider, 7);
    h.settle();
    CHECK(h.backend.prop(slider, "errors") == "[above the limit]");
    CHECK(h.text("valid") == "false");
    h.backend.slide(slider, 3);
    h.settle();
    CHECK(h.backend.prop(slider, "errors") == "[]");
    CHECK(h.text("valid") == "true");
    CHECK(h.text("ready") == "false");
    h.backend.edit(h.byTestId("TextInput", "note"), "ok");
    h.settle();
    CHECK(h.text("ready") == "true");
    h.backend.click(h.byTestId("Button", "save"));
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_Save") == 1);
    CHECK(h.dispatcher.calls.at(0).body == R"({"dilution":3})");
}

TEST_CASE("interpreter relations: a run from a dialog that is not ready is refused, and marks it submitted",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(dialog("submit"));
    h.backend.click(h.byTestId("Button", "open"));
    h.settle();
    int const note = h.byTestId("TextInput", "note");
    CHECK(h.backend.prop(note, "errors") == "[]");
    h.backend.click(h.byTestId("Button", "save"));
    h.settle();
    CHECK(h.dispatcher.calls.empty());
    CHECK(h.text("submitted") == "true");
    CHECK(h.backend.prop(note, "errors") == "[Required]");
}

TEST_CASE("interpreter relations: showErrors `touched` shows an element's errors once it is committed",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(dialog("touched"));
    h.backend.click(h.byTestId("Button", "open"));
    h.settle();
    int const note = h.byTestId("TextInput", "note");
    CHECK(h.backend.prop(note, "errors") == "[]");
    h.backend.edit(note, "x");
    h.settle();
    h.backend.edit(note, "");
    h.settle();
    CHECK(h.backend.prop(note, "errors") == "[]");
    h.backend.commit(note);
    h.settle();
    CHECK(h.backend.prop(note, "errors") == "[Required]");
    CHECK(readJson(*screen, R"({"ref": "dlg.open"})") == "true");
}

TEST_CASE("interpreter relations: closing a dialog releases its scope; reopening starts afresh", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(dialog("always"));
    h.backend.click(h.byTestId("Button", "open"));
    h.settle();
    h.backend.edit(h.byTestId("TextInput", "note"), "kept?");
    h.settle();
    h.backend.dismiss(h.backend.all("Dialog").front());
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "dlg.open"})") == "false");
    CHECK(h.text("ready") == "false");
    h.backend.click(h.byTestId("Button", "open"));
    h.settle();
    CHECK(h.backend.prop(h.byTestId("TextInput", "note"), "text").empty());
}

namespace {

constexpr char const* kOptions = R"({
  "vocab": "ui/1", "id": "options",
  "models": { "catalog": { "type": "Lims_CatalogModel" } },
  "queries": { "analyses": { "model": "catalog", "action": "Lims_CatalogAnalyses",
                             "body": { "obj": { "matrix": { "ref": "matrix.selected" } } } } },
  "let": { "total": { "fn": "sum", "args": [ { "map": { "query": "analyses" }, "as": "a", "to": { "ref": "a.cost" } } ] } },
  "view": { "kind": "column", "children": [
    { "kind": "select", "id": "matrix", "testId": "matrix", "options": { "lit": [ "water", "soil" ] } },
    { "kind": "select", "id": "analysis", "testId": "analysis", "options": { "query": "analyses" }, "onStale": "STALE" },
    { "kind": "text", "testId": "chosen", "text": { "fn": "format", "args": [ "{0}", { "fn": "coalesce", "args": [ { "ref": "analysis.selectedOption.label" }, "none" ] } ] } },
    { "kind": "text", "testId": "total", "text": { "fn": "format", "args": [ "{0}", { "ref": "total" } ] } } ] } })";

std::string options(std::string const& onStale) {
    std::string text = kOptions;
    text.replace(text.find("STALE"), 5, onStale);
    return text;
}

}  // namespace

TEST_CASE("interpreter relations: options that depend on another element, with onStale", "[ui][interpreter]") {
    for (std::string const policy : {"clear", "keep"}) {
        Harness h;
        auto screen = h.mount(options(policy));
        REQUIRE(h.dispatcher.calls.size() == 1);
        CHECK(h.dispatcher.calls.at(0).body == R"({"matrix":null})");
        h.reply(0, R"([{"id":"ph","label":"pH","cost":2},{"id":"pb","label":"Lead","cost":5}])");
        CHECK(h.text("total") == "7");
        int const analysis = h.byTestId("Select", "analysis");
        h.backend.choose(analysis, ui::Key{std::string{"pb"}});
        h.settle();
        CHECK(h.text("chosen") == "Lead");
        h.backend.choose(h.byTestId("Select", "matrix"), ui::Key{std::string{"soil"}});
        h.settle();
        REQUIRE(h.dispatcher.calls.size() == 2);
        CHECK(h.dispatcher.calls.at(1).body == R"({"matrix":"soil"})");
        h.reply(1, R"([{"id":"ph","label":"pH","cost":3}])");
        CHECK(h.text("total") == "3");
        if (policy == "clear") {
            CHECK(readJson(*screen, R"({"ref": "analysis.selected"})") == "null");
        } else {
            CHECK(readJson(*screen, R"({"ref": "analysis.selected"})") == R"("pb")");
        }
        CHECK(h.text("chosen") == "none");
    }
}
