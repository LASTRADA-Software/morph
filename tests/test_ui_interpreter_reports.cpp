// SPDX-License-Identifier: Apache-2.0

// The interpreter's reports of values a document computes that the loader cannot judge: a `forEach` whose computed
// axis is not a column's, a head's lead placement and span tones, and the rendered result each leaves behind.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

constexpr char const* kDivided = R"({
  "vocab": "ui/1", "id": "divided", "state": { "dir": { "init": "horizontal" } },
  "view": { "kind": "forEach", "rows": { "lit": [ 1, 2 ] }, "key": { "row": "" },
            "axis": { "ref": "dir" }, "divided": true,
            "children": [ { "kind": "text", "text": "x" } ] } })";

constexpr char const* kHead = R"({
  "vocab": "ui/1", "id": "head", "title": "T", "leadPlacement": { "ref": "place" },
  "titleSpans": { "list": [ "a ", { "obj": { "text": "b", "tone": { "ref": "tone" } } } ] },
  "state": { "place": { "init": "end" }, "tone": { "init": "warn" } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "bogus", "label": "b", "onClick": [ { "set": "place", "value": "middle" } ] },
    { "kind": "button", "testId": "unset", "label": "u", "onClick": [ { "set": "place", "value": null } ] },
    { "kind": "button", "testId": "loud", "label": "l", "onClick": [ { "set": "tone", "value": "loud" } ] },
    { "kind": "button", "testId": "clear", "label": "c", "onClick": [ { "set": "tone", "value": null } ] } ] } })";

}  // namespace

// Mutation: drop the report, or keep the rules (divided stays set) on a row.
TEST_CASE("interpreter reports: a divided forEach on a computed horizontal axis is reported and not divided",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kDivided);
    REQUIRE(h.diagnostics.size() == 1);
    CHECK(h.diagnostics.front().path == "/view/divided");
    CHECK(h.diagnostics.front().reason ==
          "`divided` draws rules between rows of a column; this `forEach`'s `axis` is \"horizontal\"");
    CHECK_FALSE(h.backend.dump().contains("divided=true"));
}

// Mutation: report nothing for a vertical axis that was computed (a false report), or keep the rules off a column.
TEST_CASE("interpreter reports: a divided forEach on a computed vertical axis is divided and silent",
          "[ui][interpreter]") {
    Harness h;
    std::string vertical = kDivided;
    vertical.replace(vertical.find("\"horizontal\""), 12, "\"vertical\"");
    auto screen = h.mount(vertical);
    CHECK(h.diagnostics.empty());
    CHECK(h.backend.dump().contains("divided=true"));
}

// Mutation: take a name of no place as `inline`/`end`, report nothing for it, or report a null (unset) placement.
TEST_CASE("interpreter reports: a lead placement is read by name, and a bad one is reported and below",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kHead);
    CHECK(screen->leadPlacement() == ui::LeadPlacement::End);
    CHECK(h.diagnostics.empty());
    h.backend.click(h.byTestId("Button", "bogus"));
    h.settle();
    CHECK(screen->leadPlacement() == ui::LeadPlacement::Below);
    REQUIRE(h.diagnostics.size() == 1);
    CHECK(h.diagnostics.front().path == "/leadPlacement");
    CHECK(h.diagnostics.front().reason == "`leadPlacement` is `below`, `inline` or `end`");
    h.diagnostics.clear();
    h.backend.click(h.byTestId("Button", "unset"));
    h.settle();
    CHECK(screen->leadPlacement() == ui::LeadPlacement::Below);
    CHECK(h.diagnostics.empty());
}

// Mutation: drop the tone report from the head's spans, or report a run whose tone is null.
TEST_CASE("interpreter reports: a head span run of an unknown tone is reported and drawn untoned",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kHead);
    CHECK(screen->titleSpans().size() == 2);
    CHECK(h.diagnostics.empty());
    h.backend.click(h.byTestId("Button", "loud"));
    h.settle();
    auto const spans = screen->titleSpans();
    REQUIRE(spans.size() == 2);
    CHECK(spans.back().text == "b");
    CHECK(spans.back().tone == ui::Tone::Neutral);
    REQUIRE(h.diagnostics.size() == 1);
    CHECK(h.diagnostics.front().path == "/titleSpans");
    h.diagnostics.clear();
    h.backend.click(h.byTestId("Button", "clear"));
    h.settle();
    static_cast<void>(screen->titleSpans());
    CHECK(h.diagnostics.empty());
}

namespace {

constexpr char const* kSelectable = R"({
  "vocab": "ui/1", "id": "selectable", "state": { "on": { "init": false } },
  "view": { "kind": "column", "children": [
    { "kind": "row", "testId": "r", "selected": { "ref": "on" }, "children": [ { "kind": "text", "text": "a" } ] },
    { "kind": "column", "testId": "c", "selected": { "ref": "on" }, "children": [ { "kind": "text", "text": "b" } ] },
    { "kind": "button", "testId": "flip", "label": "f", "onClick": [ { "set": "on", "value": true } ] } ] } })";

constexpr char const* kNoteField = R"({
  "vocab": "ui/1", "id": "notefield", "models": { "sample": { "type": "Lims_SampleModel" } },
  "components": { "Memo": { "host": "Memo", "version": 1,
    "props": { "value": "string", "readonly": "bool", "required": "bool", "weight": "number", "errors": "json" },
    "events": { "change": "any" }, "fallback": { "kind": "text", "text": "no memo" } } },
  "view": { "kind": "form", "model": "sample", "action": "Lims_CaptureConcentration", "submit": "explicit",
    "prefill": { "lit": { "sampleId": 7, "note": "hi" } },
    "fields": { "note": { "component": "Memo", "props": { "weight": 2 } } } } })";

}  // namespace

// Mutation: leave `selected` unbound on a row (or a column): the stack is never drawn selected.
TEST_CASE("interpreter reports: a row and a column follow their own selected", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kSelectable);
    INFO(h.backend.dump());
    CHECK(h.backend.prop(h.byTestId("Row", "r"), "selected") == "false");
    CHECK(h.backend.prop(h.byTestId("Column", "c"), "selected") == "false");
    h.backend.click(h.byTestId("Button", "flip"));
    h.settle();
    CHECK(h.backend.prop(h.byTestId("Row", "r"), "selected") == "true");
    CHECK(h.backend.prop(h.byTestId("Column", "c"), "selected") == "true");
}

// Mutation: fill `value` from the wrong member, `readonly` with a constant, or a document prop of type number as text.
TEST_CASE("interpreter reports: a field component is given the field's value, flags and the document's number",
          "[ui][interpreter]") {
    Harness h;
    h.backend.loadsComponents({"Memo"});
    auto screen = h.mount(kNoteField);
    INFO(h.backend.dump());
    std::optional<int> const memo = h.backend.find("Custom", "component", "Memo");
    REQUIRE(memo.has_value());
    CHECK(h.backend.prop(*memo, "prop.value") == "hi");
    CHECK(h.backend.prop(*memo, "prop.readonly") == "false");
    CHECK(h.backend.prop(*memo, "prop.required") == "false");
    CHECK(h.backend.prop(*memo, "prop.weight") == "2");
    CHECK(h.backend.prop(*memo, "prop.errors") == "[]");
}

namespace {

/// A capture form whose `fields` and `submit` the cases fill in.
std::string captureForm(std::string_view submit, std::string_view fields) {
    return R"({ "vocab": "ui/1", "id": "capture", "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "form", "model": "sample", "action": "Lims_CaptureConcentration", "submit": ")" +
           std::string{submit} + R"(", "prefill": { "lit": { "sampleId": 4, "note": "a" } },
           "fields": {)" + std::string{fields} + R"(} } })";
}

}  // namespace

// Mutation: ignore the overlay's `decimals` (the scale stays the schema's 2) or its clamp at 18.
TEST_CASE("interpreter reports: a field overlay's decimals set the scale of the submitted number",
          "[ui][interpreter][forms]") {
    Harness h;
    auto screen = h.mount(captureForm("auto", R"("concentration": { "decimals": 3 })"));
    h.backend.edit(h.byTestId("TextInput", "field_concentration"), "12.5");
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_CaptureConcentration") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_CaptureConcentration")).body ==
          R"({"sampleId":4,"concentration":{"num":12500,"den":1000,"dp":2},"note":"a"})");
}

// Mutation: read `blankAs` "empty" as omit (the blank note is dropped from the body), or any text as empty.
TEST_CASE("interpreter reports: a field overlay's blankAs decides whether a blank note is sent",
          "[ui][interpreter][forms]") {
    auto const body = [](std::string_view note) {
        Harness h;
        auto screen = h.mount(captureForm("explicit", R"("note": { "blankAs": ")" + std::string{note} + R"(" })"));
        h.backend.edit(h.byTestId("TextInput", "field_concentration"), "1");
        h.backend.edit(h.byTestId("TextInput", "field_note"), "");
        h.settle();
        h.backend.click(h.byTestId("Button", "form_submit"));
        h.settle();
        return h.dispatcher.count("Lims_CaptureConcentration") == 0
                   ? std::string{"none"}
                   : h.dispatcher.calls.at(h.dispatcher.last("Lims_CaptureConcentration")).body;
    };
    INFO(body("empty"));
    CHECK(body("empty").contains(R"("note":"")"));
    CHECK_FALSE(body("omit").contains("note"));
}
