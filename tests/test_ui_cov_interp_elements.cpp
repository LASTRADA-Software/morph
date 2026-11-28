// SPDX-License-Identifier: Apache-2.0

// What a document reads from its elements by name: one document, each element asked for the properties it has.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/mount.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;

namespace {

constexpr std::string_view kElementsDocument = R"({
  "vocab": "ui/1", "id": "elements",
  "state": { "items": { "init": { "lit": [ { "id": 1, "n": "a" }, { "id": 2, "n": "b" } ] } },
             "isOpen": { "init": false }, "pick": { "init": "two" } },
  "view": { "kind": "column", "children": [
    { "kind": "table", "id": "grid", "testId": "grid", "rows": { "ref": "items" }, "key": { "row": "id" }, "selectionMode": "multiple",
      "columns": [ { "id": "n", "label": "N", "sortable": true } ] },
    { "kind": "tabs", "id": "pages", "tabs": [
      { "id": "one", "label": "One", "children": [ { "kind": "text", "text": "1" } ] },
      { "id": "two", "label": "Two", "children": [ { "kind": "text", "text": "2" } ] } ] },
    { "kind": "tabs", "id": "bound", "selected": { "ref": "pick" }, "tabs": [
      { "id": "one", "label": "One", "children": [ { "kind": "text", "text": "1" } ] },
      { "id": "two", "label": "Two", "children": [ { "kind": "text", "text": "2" } ] } ] },
    { "kind": "dialog", "id": "dlg", "open": { "ref": "isOpen" }, "title": "T", "children": [ { "kind": "text", "text": "x" } ] },
    { "kind": "dialog", "id": "dlg0", "title": "T", "children": [ { "kind": "text", "text": "x" } ] },
    { "kind": "drawer", "id": "dr", "side": "start", "children": [ { "kind": "spacer" } ] },
    { "kind": "textInput", "id": "name", "value": "abc", "visible": false, "enabled": { "ref": "isOpen" } },
    { "kind": "checkbox", "id": "chk", "label": "C", "checked": true },
    { "kind": "button", "testId": "go", "label": "go",
      "onClick": [ { "set": "isOpen", "value": true }, { "set": "pick", "value": "one" } ] } ] } })";

}  // namespace

TEST_CASE("cov elements: each element answers for the properties it has", "[ui][interpreter][cov][r2]") {
    ui::testing::Harness h;
    auto const screen = h.mount(kElementsDocument);
    auto const read = [&screen](std::string_view ref) {
        return ui::testing::readJson(*screen, R"({"ref": ")" + std::string{ref} + "\"}");
    };
    SECTION("before anything happens") {
        for (auto const& [ref, expected] : std::vector<std::pair<std::string_view, std::string_view>>{
                 {"grid.selection", "[]"},
                 {"grid.selectedCount", "0"},
                 {"grid.selectedCountKind", R"("exact")"},
                 {"grid.selectionSet", R"({"keys":[]})"},
                 {"grid.activeKey", "null"},
                 {"grid.sort", "[]"},
                 {"grid.viewCount", "2"},
                 {"grid.sourceCount", "2"},
                 {"grid.pending", "false"},
                 {"grid.error", "null"},
                 {"grid.modifiedCount", "0"},
                 {"grid.busyCount", "0"},
                 {"pages.selected", R"("one")"},
                 {"bound.selected", R"("two")"},
                 {"dlg.open", "false"},
                 {"dlg0.open", "false"},
                 {"dr.open", "false"},
                 {"name.visible", "false"},
                 {"name.enabled", "false"},
                 {"name.errors", "[]"},
                 {"name.valid", "true"},
                 {"name.shownErrors", "[]"},
                 {"name.draft", R"("abc")"},
                 {"name.dirty", "false"},
                 {"name.focused", "false"},
                 {"name.touched", "false"},
                 {"name.value", R"("abc")"},
                 {"chk.checked", "true"},
                 {"chk.value", "true"},
                 {"chk.nothing", "null"},
             }) {
            INFO(ref);
            CHECK(read(ref) == expected);
        }
    }
    SECTION("after a click sets what the properties are bound to") {
        h.backend.click(h.byTestId("Button", "go"));
        h.settle();
        CHECK(read("dlg.open") == "true");
        CHECK(read("name.enabled") == "true");
        CHECK(read("bound.selected") == R"("one")");
        CHECK(read("pages.selected") == R"("one")");
        CHECK(read("dlg0.open") == "false");
    }
    SECTION("a table's selection and sort follow the rows the user picks") {
        int const grid = h.byTestId("Table", "grid");
        h.backend.selectRows(grid, {ui::Key{std::int64_t{2}}});
        h.settle();
        CHECK(read("grid.selection") == "[2]");
        CHECK(read("grid.selectedCount") == "1");
        h.backend.clickHeader(grid, 0, ui::HeaderClick::Plain);
        h.settle();
        CHECK(read("grid.sort") == R"([{"column":"n","direction":"asc"}])");
    }
}
