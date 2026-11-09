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
using ui::testing::readJson;

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
    auto const screen = h.mount(kDivided);
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
    auto const screen = h.mount(vertical);
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
    auto const screen = h.mount(kSelectable);
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
    auto const screen = h.mount(kNoteField);
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
           "fields": {)" +
           std::string{fields} + R"(} } })";
}

}  // namespace

// Mutation: ignore the overlay's `decimals` (the scale stays the schema's 2) or its clamp at 18.
TEST_CASE("interpreter reports: a field overlay's decimals set the scale of the submitted number",
          "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(captureForm("auto", R"("concentration": { "decimals": 3 })"));
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
        auto const screen =
            h.mount(captureForm("explicit", R"("note": { "blankAs": ")" + std::string{note} + R"(" })"));
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

namespace {

constexpr char const* kPadded = R"({
  "vocab": "ui/1", "id": "padded", "state": { "bad": { "init": { "lit": [ 1, 2 ] } } },
  "view": { "kind": "column", "children": [
    { "kind": "column", "testId": "list", "padding": { "list": [ 1, 2 ] }, "children": [ { "kind": "text", "text": "a" } ] },
    { "kind": "row", "testId": "ref", "padding": { "ref": "bad" }, "children": [ { "kind": "text", "text": "b" } ] },
    { "kind": "column", "testId": "step", "padding": 2, "children": [ { "kind": "text", "text": "c" } ] },
    { "kind": "column", "testId": "pair", "padding": { "lit": [ 1, 3 ] }, "children": [ { "kind": "text", "text": "d" } ] }
  ] } })";

}  // namespace

// Mutation: take a computed list as its two steps (no report), or keep the report off the row.
TEST_CASE("interpreter reports: a computed padding that is a list is reported and taken as none",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kPadded);
    INFO(h.backend.dump());
    REQUIRE(h.diagnostics.size() == 2);
    CHECK(h.diagnostics.at(0).path == "/view/children/0/padding");
    CHECK(h.diagnostics.at(1).path == "/view/children/1/padding");
    CHECK(h.diagnostics.at(0).reason == "a computed `padding` is one spacing step, not a list; taken as 0");
    CHECK(h.backend.prop(h.byTestId("Column", "list"), "padding") ==
          h.backend.prop(h.byTestId("Row", "ref"), "padding"));
    CHECK(h.backend.prop(h.byTestId("Column", "pair"), "padding") !=
          h.backend.prop(h.byTestId("Column", "step"), "padding"));
}

namespace {

constexpr char const* kErrored = R"({
  "vocab": "ui/1", "id": "errored", "showErrors": "always",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "mutations": { "attach": { "model": "sample", "action": "Lims_Attach" } },
  "view": { "kind": "column", "children": [
    { "kind": "textInput", "id": "mass", "testId": "mass" },
    { "kind": "textInput", "testId": "bare", "errors": { "list": [ "bad one", "bad two" ] } },
    { "kind": "button", "testId": "send", "label": "s", "onClick": [ { "run": "attach", "body": { "lit": 1 } } ] } ] } })";

}  // namespace

// Mutation: leave a mutation's `errors` out of a field's errors, or the field's own `errors` out of an id-less field.
TEST_CASE(
    "interpreter reports: a mutation reply's errors attach to the field of that name, and a bare field shows its own",
    "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kErrored);
    int const mass = h.byTestId("TextInput", "mass");
    CHECK(h.backend.prop(mass, "errors") == "[]");
    CHECK(h.backend.prop(h.byTestId("TextInput", "bare"), "errors") == "[bad one,bad two]");
    h.backend.click(h.byTestId("Button", "send"));
    h.settle();
    h.reply(h.dispatcher.last("Lims_Attach"), R"({"errors":{"mass":"too low"}})");
    CHECK(h.backend.prop(mass, "errors") == "[too low]");
    CHECK(h.backend.prop(h.byTestId("TextInput", "bare"), "errors") == "[bad one,bad two]");
}

namespace {

// The keys of a table's rows, in the order the table holds them, joined by spaces.
std::string rowOrder(Harness const& h, int table) {
    std::string order;
    for (int const row : h.backend.children(table)) {
        std::string const shown = h.backend.prop(row, "rowKey");
        order += (order.empty() ? "" : " ") +
                 (shown.size() >= 2 && shown.front() == '"' ? shown.substr(1, shown.size() - 2) : shown);
    }
    return order;
}

std::string sortedTable(std::string_view order, std::string_view rows) {
    return R"({ "vocab": "ui/1", "id": "sorted", "state": { "order": { "init": )" + std::string{order} +
           R"( }, "items": { "init": { "lit": )" + std::string{rows} + R"( } } },
      "view": { "kind": "table", "id": "t", "rows": { "ref": "items" }, "sort": { "ref": "order" },
                "columns": [ { "id": "v", "label": "V", "sortable": true } ] } })";
}

}  // namespace

// Mutation: read a null sort as a bare record (a report, and no order), or as an error.
TEST_CASE("interpreter reports: a table bound to a null sort keeps the source order, unreported",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(sortedTable("null", R"([ { "id": "b", "v": 2 }, { "id": "a", "v": 1 } ])"));
    CHECK(rowOrder(h, h.backend.all("Table").front()) == "b a");
    CHECK(h.backend.prop(h.backend.all("Table").front(), "sort") == "none");
    CHECK(h.diagnostics.empty());
}

// Mutation: leave a row's missing sort field out of the keys (the keys are then misaligned with the rows).
TEST_CASE("interpreter reports: a row without the sorted field sorts as null, after the others", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(sortedTable(R"({ "lit": [ { "column": "v", "direction": "asc" } ] })",
                                            R"([ { "id": "b", "v": 2 }, { "id": "n" }, { "id": "a", "v": 1 } ])"));
    INFO(rowOrder(h, h.backend.all("Table").front()));
    CHECK(rowOrder(h, h.backend.all("Table").front()) == "a b n");
    CHECK(h.diagnostics.empty());
}

// Mutation: leave a drawer's `descriptionSpans` unbound (it shows its plain description).
TEST_CASE("interpreter reports: a drawer's description spans reach the drawer", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({ "vocab": "ui/1", "id": "spanned",
      "view": { "kind": "drawer", "open": true, "title": "T", "description": "plain",
                "descriptionSpans": { "list": [ "see ", { "obj": { "text": "this", "strong": true } } ] },
                "children": [ { "kind": "spacer" } ] } })");
    INFO(h.backend.dump());
    int const drawer = h.backend.all("Drawer").front();
    CHECK(h.backend.prop(drawer, "descriptionSpans").contains("this"));
    CHECK(h.backend.prop(drawer, "descriptionSpans").contains("see "));
}

namespace {

constexpr char const* kReadForm = R"({
  "vocab": "ui/1", "id": "readform", "models": { "sample": { "type": "Lims_SampleModel" } },
  "view": { "kind": "column", "children": [
    { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_CaptureConcentration",
      "prefill": { "lit": { "sampleId": 4 } } },
    { "kind": "form", "id": "gone", "model": "sample", "action": "Lims_NoSuchAction" } ] } })";

}  // namespace

// Mutation: answer `pending` with `submitted`, read the body while not ready, or answer a form without a session.
TEST_CASE("interpreter reports: a form's pending, body and submitted follow its submission; a failed form reads null",
          "[ui][interpreter][forms]") {
    Harness h;
    auto const screen = h.mount(kReadForm);
    REQUIRE(h.diagnostics.size() == 1);
    CHECK(h.diagnostics.front().reason ==
          "no schema for `Lims_NoSuchAction`: the catalog has none for a `form` to build from");
    CHECK(readJson(*screen, R"({"ref":"cap.pending"})") == "false");
    CHECK(readJson(*screen, R"({"ref":"cap.body"})") == "null");
    CHECK(readJson(*screen, R"({"ref":"cap.submitted"})") == "false");
    h.backend.edit(h.byTestId("TextInput", "field_concentration"), "2");
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"cap.body.sampleId"})") == "4");
    CHECK(readJson(*screen, R"({"ref":"cap.submitted"})") == "true");
    CHECK(readJson(*screen, R"({"ref":"cap.pending"})") == "true");
    h.reply(h.dispatcher.last("Lims_CaptureConcentration"), R"({"ok":true})");
    CHECK(readJson(*screen, R"({"ref":"cap.pending"})") == "false");
    CHECK(readJson(*screen, R"({"ref":"gone.pending"})") == "null");
    CHECK(readJson(*screen, R"({"ref":"gone.body"})") == "null");
    CHECK(readJson(*screen, R"({"ref":"gone.ready"})") == "null");
}
