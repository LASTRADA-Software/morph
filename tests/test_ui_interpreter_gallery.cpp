// SPDX-License-Identifier: Apache-2.0

// Interpreter behaviour the gallery's screens lean on, mounted on RecordingBackend: a tabs node's shown tab as an
// element value, and the reports of a computed value that names nothing.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

// How many reports carry @p text.
std::ptrdiff_t reported(Harness const& h, std::string_view text) {
    return std::ranges::count_if(h.diagnostics,
                                 [text](ui::Refusal const& refusal) { return refusal.reason.contains(text); });
}

}  // namespace

// Mutation: read an unbound tabs node's `selected` as the first tab's always (the text stays `one` after the pick).
TEST_CASE("gallery: an unbound tabs node's selected is the user's pick, else its first tab", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "picks",
      "state": { "chose": { "init": "" } },
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "shown", "text": { "ref": "t.selected" } },
        { "kind": "text", "testId": "chose", "text": { "ref": "chose" } },
        { "kind": "tabs", "id": "t", "testId": "bar", "onChoose": [ { "set": "chose", "value": { "ref": "event" } } ],
          "tabs": [ { "id": "one", "label": "One", "children": [ { "kind": "spacer" } ] },
                    { "id": "two", "label": "Two", "children": [ { "kind": "spacer" } ] } ] } ] } })");
    int const bar = h.backend.find("Tabs", "testId", "bar").value();
    CHECK(h.text("shown") == "one");
    h.backend.chooseIndex(bar, 1);
    h.settle();
    CHECK(h.text("shown") == "two");
    CHECK(h.text("chose") == "two");
    CHECK(h.backend.prop(bar, "selected") == "1");
}

// Mutation: read the first tab's empty id as a name (the value then reads `""`).
TEST_CASE("gallery: a tabs node whose tabs have no ids has no selected tab to name", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "anonymous",
      "view": { "kind": "tabs", "id": "t", "tabs": [ { "label": "One", "children": [ { "kind": "spacer" } ] } ] } })");
    CHECK(ui::testing::readJson(*screen, R"({ "ref": "t.selected" })") == "null");
}

// Mutation: report a bound value that names no tab on every read (the count is then 2 after the second write).
TEST_CASE("gallery: a bound selected that names no tab shows the first, reported once", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "bound",
      "state": { "tab": { "init": "three" } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "two", "label": "two", "onClick": [ { "set": "tab", "value": "two" } ] },
        { "kind": "button", "testId": "four", "label": "four", "onClick": [ { "set": "tab", "value": "four" } ] },
        { "kind": "tabs", "id": "t", "testId": "bar", "selected": { "ref": "tab" },
          "tabs": [ { "id": "one", "label": "One", "children": [ { "kind": "spacer" } ] },
                    { "id": "two", "label": "Two", "children": [ { "kind": "spacer" } ] } ] } ] } })");
    int const bar = h.backend.find("Tabs", "testId", "bar").value();
    CHECK(h.backend.prop(bar, "selected") == "0");
    CHECK(reported(h, R"("three" names no tab; the first is shown)") == 1);
    h.backend.click(h.byTestId("Button", "two"));
    h.settle();
    CHECK(h.backend.prop(bar, "selected") == "1");
    h.backend.click(h.byTestId("Button", "four"));
    h.settle();
    CHECK(h.backend.prop(bar, "selected") == "0");
    CHECK(reported(h, "names no tab") == 1);
}

// Mutation: step a segmented select the other way on Right (the first Right then picks `a`).
TEST_CASE("gallery: Left and Right step a segmented select to its neighbour, as the user's choice",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "segments",
      "state": { "picked": { "init": "b" } },
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "picked", "text": { "ref": "picked" } },
        { "kind": "select", "testId": "seg", "label": "Show", "style": "segmented",
          "options": { "lit": [ "a", "b", "c" ] }, "selected": { "ref": "picked" },
          "onChoose": [ { "set": "picked", "value": { "ref": "event" } } ] } ] } })");
    int const segments = h.byTestId("Select", "seg");
    CHECK(h.backend.press(segments, "Right"));
    h.settle();
    CHECK(h.text("picked") == "c");
    // Past the last segment there is no neighbour: the key is still the select's, and nothing is chosen.
    CHECK(h.backend.press(segments, "Right"));
    h.settle();
    CHECK(h.text("picked") == "c");
    CHECK(h.backend.press(segments, "Left"));
    h.settle();
    CHECK(h.text("picked") == "b");
    // Up and Down are not a segmented select's keys.
    CHECK_FALSE(h.backend.press(h.byTestId("Select", "seg"), "Up"));
}

// Mutation: let Enter commit a multiline input (the text then reads `two lines`).
TEST_CASE("gallery: Enter and Space press a button, and Enter commits a one-line input but not a text area",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "keys",
      "state": { "n": { "init": 0 }, "line": { "init": "" }, "area": { "init": "" } },
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "n", "text": { "fn": "format", "args": [ "{0}", { "ref": "n" } ] } },
        { "kind": "text", "testId": "line", "text": { "ref": "line" } },
        { "kind": "text", "testId": "area", "text": { "ref": "area" } },
        { "kind": "button", "testId": "add", "label": "add",
          "onClick": [ { "set": "n", "value": { "fn": "add", "args": [ { "ref": "n" }, 1 ] } } ] },
        { "kind": "textInput", "testId": "lineInput", "onCommit": [ { "set": "line", "value": { "ref": "event" } } ] },
        { "kind": "textInput", "testId": "areaInput", "mode": "multiline",
          "onCommit": [ { "set": "area", "value": { "ref": "event" } } ] } ] } })");
    int const add = h.byTestId("Button", "add");
    CHECK(h.backend.press(add, "Enter"));
    CHECK(h.backend.press(add, "Space"));
    h.settle();
    CHECK(h.text("n") == "2");
    CHECK_FALSE(h.backend.press(add, "Tab"));

    int const line = h.byTestId("TextInput", "lineInput");
    h.backend.edit(line, "one line");
    CHECK_FALSE(h.backend.press(line, "Enter"));
    h.settle();
    CHECK(h.text("line") == "one line");

    int const area = h.byTestId("TextInput", "areaInput");
    h.backend.edit(area, "two lines");
    CHECK_FALSE(h.backend.press(area, "Enter"));
    h.settle();
    CHECK(h.text("area").empty());
}

namespace {

// A table over `items`, paged by the state `size` at the state `page`.
constexpr std::string_view kPaged = R"({
  "vocab": "ui/1", "id": "paged",
  "state": { "items": { "init": { "lit": [ { "id": 1 }, { "id": 2 }, { "id": 3 }, { "id": 4 }, { "id": 5 } ] } },
             "size": { "type": "any" }, "page": { "type": "any" } },
  "onMount": [ { "set": "size", "value": 2 } ],
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "noSize", "label": "x", "onClick": [ { "set": "size", "value": null } ] },
    { "kind": "button", "testId": "textSize", "label": "x", "onClick": [ { "set": "size", "value": "two" } ] },
    { "kind": "button", "testId": "lastPage", "label": "x", "onClick": [ { "set": "page", "value": 2 } ] },
    { "kind": "button", "testId": "badPage", "label": "x", "onClick": [ { "set": "page", "value": -1 } ] },
    { "kind": "button", "testId": "empty", "label": "x", "onClick": [ { "set": "items", "value": { "lit": [] } } ] },
    { "kind": "table", "id": "t", "rows": { "ref": "items" }, "pageSize": { "ref": "size" }, "page": { "ref": "page" },
      "columns": [ { "id": "id", "label": "Id" } ] } ] } })";

// The table's page values: `page/pageCount first-last of viewCount`.
std::string pages(ui::Screen const& screen) {
    auto const read = [&screen](std::string_view name) {
        return ui::testing::readJson(screen, R"({ "ref": "t.)" + std::string{name} + R"(" })");
    };
    return read("page") + "/" + read("pageCount") + " " + read("pageFirst") + "-" + read("pageLast") + " of " +
           read("viewCount");
}

}  // namespace

// Mutation: read a negative `page` as the last page (the values then read `2/3 5-5 of 5`).
TEST_CASE("gallery: a table pages by its computed pageSize, and a page or size that is none is reported",
          "[ui][interpreter][table]") {
    Harness h;
    auto const screen = h.mount(kPaged);
    CHECK(pages(*screen) == "0/3 1-2 of 5");
    h.backend.click(h.byTestId("Button", "lastPage"));
    h.settle();
    CHECK(pages(*screen) == "2/3 5-5 of 5");
    h.backend.click(h.byTestId("Button", "badPage"));
    h.settle();
    CHECK(pages(*screen) == "0/3 1-2 of 5");
    CHECK(reported(h, "page is -1; a page is a whole number from 0") == 1);
    h.backend.click(h.byTestId("Button", "noSize"));
    h.settle();
    CHECK(pages(*screen) == "0/1 1-5 of 5");
    CHECK(h.diagnostics.size() == 1);
    h.backend.click(h.byTestId("Button", "textSize"));
    h.settle();
    CHECK(pages(*screen) == "0/1 1-5 of 5");
    CHECK(reported(h, R"(pageSize is "two"; a page holds a positive whole number of rows)") == 1);
    h.backend.click(h.byTestId("Button", "empty"));
    h.settle();
    CHECK(pages(*screen) == "0/1 0-0 of 0");
}

// Mutation: read a model-backed table's pageCount as 0 (the values then read `0/0`).
TEST_CASE("gallery: a model-backed table, which its engine pages, reads page 0 of 1 over no rows of its own",
          "[ui][interpreter][table]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "engine",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "table", "id": "t", "model": "sample", "action": "Lims_ListResults" } })");
    CHECK(pages(*screen) == "0/1 0-0 of 0");
    CHECK(ui::testing::readJson(*screen, R"({ "ref": "t.sourceCount" })") == "0");
}

namespace {

// A head whose menu and tools hold an entry or tool of every state: enabled, disabled, hidden, and inside a disabled
// submenu.
constexpr std::string_view kHead = R"({
  "vocab": "ui/1", "id": "head", "title": "Head",
  "state": { "log": { "init": "" } },
  "menu": [
    { "separator": true },
    { "id": "off", "label": "Off", "enabled": false, "onClick": [ { "set": "log", "value": "off" } ] },
    { "id": "gone", "label": "Gone", "visible": false },
    { "separator": true },
    { "separator": true },
    { "label": "Export", "enabled": false, "items": [ { "id": "csv", "label": "CSV" } ] },
    { "id": "bare", "label": "Bare" },
    { "label": "Later", "visible": false, "items": [ { "id": "deep", "label": "Deep" } ] },
    { "separator": true } ],
  "actions": [ { "id": "noop", "label": "Noop" } ],
  "tools": [
    { "id": "frozen", "kind": "search", "label": "Frozen", "enabled": false },
    { "id": "hidden", "kind": "search", "label": "Hidden", "visible": false },
    { "id": "find", "kind": "search", "label": "Find", "value": { "ref": "log" },
      "onSubmit": [ { "set": "log", "value": { "fn": "concat", "args": [ "submitted ", { "ref": "event" } ] } } ] },
    { "id": "lab", "kind": "select", "label": "Lab", "options": { "lit": [ "North" ] },
      "onChoose": [ { "set": "log", "value": { "ref": "event" } } ] } ],
  "view": { "kind": "text", "testId": "log", "text": { "ref": "log" } } })";

// The menu's shown entries: ids, `-` for a separator, a submenu's in brackets.
// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::string entries(std::vector<ui::HeadMenuEntry> const& level) {
    std::string out;
    for (ui::HeadMenuEntry const& entry : level) {
        out += out.empty() ? "" : " ";
        if (entry.separator) {
            out += '-';
        } else {
            out += (entry.id.empty() ? entry.label : entry.id) + (entry.enabled ? "" : "!");
            if (!entry.items.empty()) {
                out += "[" + entries(entry.items) + "]";
            }
        }
    }
    return out;
}

}  // namespace

// Mutation: keep a separator that hidden entries leave last (the menu then ends ` -`).
TEST_CASE("gallery: a head menu leaves out hidden entries and the separators they strand", "[ui][interpreter][head]") {
    Harness h;
    auto const screen = h.mount(kHead);
    CHECK(entries(screen->menu()) == "off! - Export![csv] bare");
}

// Mutation: run a disabled submenu's entry (`csv` is then chosen), or a hidden one's (`deep`).
TEST_CASE("gallery: a head menu entry is chosen only when it and every submenu around it are shown and enabled",
          "[ui][interpreter][head]") {
    Harness h;
    auto const screen = h.mount(kHead);
    CHECK_FALSE(screen->chooseMenuEntry("off"));
    CHECK_FALSE(screen->chooseMenuEntry("gone"));
    CHECK_FALSE(screen->chooseMenuEntry("csv"));
    CHECK_FALSE(screen->chooseMenuEntry("nothing"));
    h.settle();
    CHECK(h.text("log").empty());
    CHECK(reported(h, "head menu entry off is disabled") == 1);
    CHECK(reported(h, "head menu entry gone is hidden") == 1);
    CHECK(reported(h, "head menu entry csv is disabled") == 1);
    CHECK(reported(h, "head menu entry nothing is unknown") == 1);
    CHECK_FALSE(screen->chooseMenuEntry("deep"));
    CHECK(reported(h, "head menu entry deep is hidden") == 1);
    // An entry or an action without onClick is still chosen.
    CHECK(screen->chooseMenuEntry("bare"));
    CHECK(screen->runAction("noop"));
}

// Mutation: let a search be edited with a number (the edit then reports nothing and runs).
TEST_CASE("gallery: a head tool is edited or submitted only as its kind is, and only shown and enabled",
          "[ui][interpreter][head]") {
    Harness h;
    auto const screen = h.mount(kHead);
    CHECK_FALSE(screen->editTool("frozen", ui::Value{std::string{"x"}}));
    CHECK_FALSE(screen->editTool("hidden", ui::Value{std::string{"x"}}));
    CHECK_FALSE(screen->editTool("find", ui::Value{std::int64_t{3}}));
    CHECK_FALSE(screen->submitTool("lab"));
    CHECK_FALSE(screen->submitTool("nothing"));
    h.settle();
    CHECK(h.text("log").empty());
    CHECK(reported(h, "head tool frozen is disabled") == 1);
    CHECK(reported(h, "head tool hidden is hidden") == 1);
    CHECK(reported(h, "head tool find is a search, edited with text") == 1);
    CHECK(reported(h, "head tool lab is a select, which is not submitted") == 1);
    CHECK(reported(h, "head tool nothing is unknown") == 1);
    CHECK(screen->editTool("lab", ui::Value{std::string{"North"}}));
    h.settle();
    CHECK(h.text("log") == "North");
    CHECK(screen->submitTool("find"));
    h.settle();
    CHECK(h.text("log") == "submitted North");
    std::vector<ui::HeadTool> const tools = screen->tools();
    REQUIRE(tools.size() == 3);
    CHECK_FALSE(tools.at(0).enabled);
    CHECK(tools.at(1).value == "submitted North");
    CHECK(tools.at(2).options.size() == 1);
}

namespace {

// Spans as `text` runs, `*` before a strong one and a backtick before a mono one, then `dropped` when one was.
std::string spans(std::string_view json) {
    auto const parsed = ui::parseJson(json);
    REQUIRE(parsed.has_value());
    auto const value = ui::valueFromJson(*parsed);
    REQUIRE(value.has_value());
    bool dropped = false;
    std::string out;
    ui::FormatContext const format;
    for (ui::TextSpan const& span : ui::detail::spansOf(*value, format, dropped)) {
        out += std::string{"|"} + (span.strong ? "*" : "") + (span.mono ? "`" : "") + span.text;
    }
    return out + (dropped ? " dropped" : "");
}

}  // namespace

// Mutation: read any flag value as set (`"mono": "yes"` then marks its run).
TEST_CASE("gallery: computed spans keep values and records with text, and drop lists, nulls and records without",
          "[ui][interpreter]") {
    CHECK(spans("null").empty());
    CHECK(spans(R"("Lot")") == " dropped");
    CHECK(spans(R"(["Lot ", 7, {"text": "L-1", "mono": true}, {"text": "!", "strong": true, "mono": "yes"}])") ==
          "|Lot |7|`L-1|*!");
    CHECK(spans(R"(["a", null, ["b"], {"strong": true}])") == "|a dropped");
    CHECK(ui::detail::toneNamed(ui::Value{}) == ui::Tone::Neutral);
    CHECK(ui::detail::toneNamed(ui::Value{std::string{"warn"}}) == ui::Tone::Warn);
    CHECK(ui::detail::toneNamed(ui::Value{std::string{"loud"}}) == std::nullopt);
    CHECK(ui::detail::toneNamed(ui::Value{std::int64_t{3}}) == std::nullopt);
}

// Mutation: read a menu trigger's `danger` as primary (the second trigger then records `Primary`).
TEST_CASE("gallery: a menu drawn as one button takes its emphasis by name", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "menus",
      "view": { "kind": "column", "children": [
        { "kind": "menu", "testId": "primary", "style": "button", "label": "P", "emphasis": "primary",
          "items": [ { "label": "One" } ] },
        { "kind": "menu", "testId": "danger", "style": "button", "label": "D", "emphasis": "danger",
          "items": [ { "label": "One" } ] },
        { "kind": "menu", "testId": "ghost", "style": "button", "label": "G", "emphasis": "ghost",
          "items": [ { "label": "One" } ] },
        { "kind": "menu", "testId": "plain", "style": "button", "label": "N", "items": [ { "label": "One" } ] } ] } })");
    CHECK(h.backend.prop(h.byTestId("Menu", "primary"), "emphasis") == "Primary");
    CHECK(h.backend.prop(h.byTestId("Menu", "danger"), "emphasis") == "Danger");
    CHECK(h.backend.prop(h.byTestId("Menu", "ghost"), "emphasis") == "Ghost");
    CHECK_FALSE(h.backend.hasProp(h.byTestId("Menu", "plain"), "emphasis"));
}

// Mutation: draw only an entry's first child (the second line is then not in the entry).
TEST_CASE("gallery: a list entry's content of several nodes is drawn as a column inside the entry",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "entries",
      "view": { "kind": "button", "testId": "entry", "label": "Sample", "style": "item", "selected": false,
                "children": [ { "kind": "text", "testId": "first", "text": "S-1" },
                              { "kind": "text", "testId": "second", "text": "due today" } ] } })");
    int const entry = h.byTestId("Button", "entry");
    CHECK(h.backend.prop(entry, "style") == "Item");
    int const first = h.byTestId("Text", "first");
    int const second = h.byTestId("Text", "second");
    std::vector<int> const columns = h.backend.all("Column");
    CHECK(std::ranges::any_of(columns, [&h, first, second](int column) {
        return h.backend.children(column) == std::vector<int>{first, second};
    }));
}

// Mutation: let a segmented select with nothing chosen step from its first option (Right then picks `b`).
TEST_CASE("gallery: a segmented select steps only from a chosen option, and not past either end",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "unpicked",
      "state": { "picked": { "type": "string?", "init": null } },
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "picked", "text": { "fn": "coalesce", "args": [ { "ref": "picked" }, "-" ] } },
        { "kind": "button", "testId": "first", "label": "first", "onClick": [ { "set": "picked", "value": "a" } ] },
        { "kind": "button", "testId": "stray", "label": "stray", "onClick": [ { "set": "picked", "value": "z" } ] },
        { "kind": "select", "testId": "seg", "label": "Show", "style": "segmented",
          "options": { "lit": [ "a", "b" ] }, "selected": { "ref": "picked" },
          "onChoose": [ { "set": "picked", "value": { "ref": "event" } } ] } ] } })");
    int const segments = h.byTestId("Select", "seg");
    CHECK(h.backend.press(segments, "Right"));
    h.settle();
    CHECK(h.text("picked") == "-");
    h.backend.click(h.byTestId("Button", "first"));
    h.settle();
    CHECK(h.backend.press(segments, "Left"));
    h.settle();
    CHECK(h.text("picked") == "a");
    // A selected key no option has steps nowhere.
    h.backend.click(h.byTestId("Button", "stray"));
    h.settle();
    CHECK(h.backend.press(segments, "Right"));
    h.settle();
    CHECK(h.text("picked") == "z");
}

// Mutation: let chooseMenuEntry run a disabled entry (the log then reads `off`).
TEST_CASE("gallery: RecordingBackend chooses only an enabled leaf entry of a menu", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "menu",
      "state": { "log": { "init": "" } },
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "log", "text": { "ref": "log" } },
        { "kind": "menu", "testId": "m", "items": [
          { "label": "Off", "enabled": false, "onClick": [ { "set": "log", "value": "off" } ] },
          { "separator": true },
          { "label": "Export", "items": [ { "label": "CSV", "onClick": [ { "set": "log", "value": "csv" } ] } ] } ] } ] } })");
    int const menu = h.byTestId("Menu", "m");
    h.backend.chooseMenuEntry(menu, {0});
    h.backend.chooseMenuEntry(menu, {1});
    h.backend.chooseMenuEntry(menu, {2});
    h.backend.chooseMenuEntry(menu, {7});
    h.backend.chooseMenuEntry(menu, {2, 3});
    h.settle();
    CHECK(h.text("log").empty());
    h.backend.chooseMenuEntry(menu, {2, 0});
    h.settle();
    CHECK(h.text("log") == "csv");
}

// Mutation: let selectRows take a key twice (the second call then selects without throwing).
TEST_CASE("gallery: RecordingBackend refuses a selection a user could not make", "[ui][interpreter][table]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "rows",
      "state": { "items": { "init": { "lit": [ { "id": "a" }, { "id": "b" } ] } } },
      "view": { "kind": "column", "children": [
        { "kind": "table", "testId": "single", "rows": { "ref": "items" }, "selectionMode": "single",
          "columns": [ { "id": "id", "label": "Id" } ] },
        { "kind": "table", "testId": "multiple", "rows": { "ref": "items" }, "selectionMode": "multiple",
          "columns": [ { "id": "id", "label": "Id" } ] },
        { "kind": "table", "testId": "none", "rows": { "ref": "items" },
          "columns": [ { "id": "id", "label": "Id" } ] } ] } })");
    int const single = h.byTestId("Table", "single");
    int const multiple = h.byTestId("Table", "multiple");
    int const none = h.byTestId("Table", "none");
    ui::Key const a{std::string{"a"}};
    ui::Key const b{std::string{"b"}};
    CHECK_THROWS_AS(h.backend.selectRows(single, {a, b}), std::logic_error);
    CHECK_THROWS_AS(h.backend.selectRows(multiple, {a, a}), std::logic_error);
    CHECK_THROWS_AS(h.backend.selectRows(multiple, {ui::Key{std::string{"c"}}}), std::logic_error);
    CHECK_THROWS_AS(h.backend.selectRows(none, {a}), std::logic_error);
    CHECK_THROWS_AS(h.backend.clickRow(none, a, ui::RowClick::Only), std::logic_error);
    CHECK_THROWS_AS(h.backend.clickRow(multiple, ui::Key{std::string{"c"}}, ui::RowClick::Only), std::logic_error);
    CHECK_THROWS_AS(h.backend.activateRow(multiple, ui::Key{std::string{"c"}}), std::logic_error);
    h.backend.selectRows(multiple, {a, b});
    h.settle();
    CHECK(h.backend.prop(multiple, "selection") == R"(["a","b"])");
    // Ctrl+A is a multiple table's, and Enter on a table with nothing marked activates nothing.
    CHECK_FALSE(h.backend.press(single, "Ctrl+A"));
    CHECK(h.backend.press(none, "Enter"));
}
