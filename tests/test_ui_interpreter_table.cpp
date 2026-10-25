// SPDX-License-Identifier: Apache-2.0

// A table over rows, lowered by the interpreter and mounted on RecordingBackend: its columns' options and its
// selection.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;
using ui::testing::readJson;

namespace {

// The roles of the default cells, row by row, left to right. A row's first cell sits inside the column that holds
// the row's scope.
std::vector<std::string> cellRoles(Harness const& h) {
    std::vector<std::string> roles;
    for (int const row : h.backend.all("Row")) {
        for (int cell : h.backend.children(row)) {
            while (h.backend.kindOf(cell) != "Text" && !h.backend.children(cell).empty()) {
                cell = h.backend.children(cell).front();
            }
            roles.push_back(h.backend.prop(cell, "role"));
        }
    }
    return roles;
}

}  // namespace

// Mutation: map `mono` to `Normal` in `roleNamed` (the first column reads `Normal`).
TEST_CASE("table: a column's mono role reaches its default cell, and a computed role that names none is reported once",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "mono",
      "state": { "kind": { "init": "monospace" },
                 "items": { "init": { "lit": [ { "id": "S-1", "n": 1 }, { "id": "S-2", "n": 2 } ] } } },
      "view": { "kind": "table", "rows": { "ref": "items" },
                "columns": [ { "id": "id", "label": "Sample", "role": "mono" },
                             { "id": "n", "label": "N", "role": { "ref": "kind" } } ] } })");
    CHECK(cellRoles(h) == std::vector<std::string>{"Mono", "Normal", "Mono", "Normal"});
    auto const reports = std::ranges::count_if(h.diagnostics, [](ui::Refusal const& refusal) {
        return refusal.reason.contains(R"(role is "monospace")") && refusal.reason.contains("mono");
    });
    CHECK(reports == 1);
}

// Mutation: take a column's valign from the column alone (the first column records no valign).
TEST_CASE("table: a column's align and valign reach the view, the table's valign the columns that give none",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "places",
      "state": { "items": { "init": { "lit": [ { "id": "S-1", "area": "0.21" } ] } } },
      "view": { "kind": "table", "rows": { "ref": "items" }, "valign": "start",
                "columns": [ { "id": "id", "label": "Sample" },
                             { "id": "area", "label": "Area", "align": "end", "valign": "end" } ] } })");
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.prop(table, "columns") == "[Sample:content;valign=start,Area:content;align=end;valign=end]");
}

TEST_CASE("table: a computed column's place that names none is reported once and takes the default",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "places",
      "state": { "items": { "init": { "lit": [ { "id": "S-1" } ] } } },
      "view": { "kind": "table", "rows": { "ref": "items" },
                "columns": { "lit": [ { "id": "id", "label": "Sample", "align": "right" },
                                      { "id": "n", "label": "N", "valign": "top", "align": "center" } ] } } })");
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.prop(table, "columns") == "[Sample:content,N:content;align=center]");
    auto const reports = std::ranges::count_if(
        h.diagnostics, [](ui::Refusal const& refusal) { return refusal.reason.contains("a cell's place is"); });
    CHECK(reports == 1);
}

namespace {

// A multiple table over rows a, b and c, its selection bound to the state `selected`, which starts as a and c.
// @p handler is the table's `onSelectionChange`, or empty for none.
std::string boundTable(std::string_view handler) {
    return std::string{R"({
      "vocab": "ui/1", "id": "bound",
      "state": { "selected": { "init": { "lit": [ "a", "c" ] } },
                 "items": { "init": { "lit": [ { "id": "a" }, { "id": "b" }, { "id": "c" } ] } } },
      "view": { "kind": "column", "children": [
        { "kind": "table", "id": "t", "rows": { "ref": "items" }, "selectionMode": "multiple",
          "selection": { "ref": "selected" })"} +
           std::string{handler} + R"(,
          "columns": [ { "id": "id", "label": "Id" } ] },
        { "kind": "text", "testId": "count", "text": { "ref": "t.selectedCount" } },
        { "kind": "button", "testId": "clear", "label": "Clear",
          "onClick": [ { "set": "selected", "value": { "lit": [] } } ] } ] } })";
}

constexpr std::string_view kWriteBack =
    R"(, "onSelectionChange": [ { "set": "selected", "value": { "ref": "event" } } ])";

ui::Key key(std::string text) { return ui::Key{std::move(text)}; }

}  // namespace

// Mutation: bind the view's selection to the table's own selection alone (the widget marks nothing, and the count
// reads 0).
TEST_CASE("table: a bound selection marks its rows and is what the table's element values read", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(boundTable(kWriteBack));
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.prop(table, "selection") == R"(["a","c"])");
    CHECK(h.text("count") == "2");
    CHECK(readJson(*screen, R"({"ref":"t.selection"})") == R"(["a","c"])");
}

TEST_CASE("table: a user's selection runs onSelectionChange with it, and the written state is what the table marks",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(boundTable(kWriteBack));
    int const table = h.backend.all("Table").front();
    h.backend.selectRows(table, {key("b")});
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"selected"})") == R"(["b"])");
    CHECK(h.backend.prop(table, "selection") == R"(["b"])");
}

// Mutation: write the table's own selection even when the key is bound (nothing snaps back).
TEST_CASE("table: a bound selection the document does not write back snaps back", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(boundTable(""));
    int const table = h.backend.all("Table").front();
    h.backend.selectRows(table, {key("b")});
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"selected"})") == R"(["a","c"])");
    CHECK(h.backend.prop(table, "selection") == R"(["a","c"])");
    CHECK(h.text("count") == "2");
}

TEST_CASE("table: setting the bound state clears the table's marks", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(boundTable(kWriteBack));
    int const table = h.backend.all("Table").front();
    REQUIRE(h.backend.prop(table, "selection") == R"(["a","c"])");
    h.backend.click(h.byTestId("Button", "clear"));
    h.settle();
    CHECK(h.backend.prop(table, "selection") == "[]");
    CHECK(h.text("count") == "0");
}

TEST_CASE("table: with no selection bound, or a literal null, the table keeps the user's selection and reports it",
          "[ui][interpreter]") {
    for (std::string_view const selection : {"", R"(, "selection": null)"}) {
        Harness h;
        auto screen = h.mount(std::string{R"({
          "vocab": "ui/1", "id": "own",
          "state": { "seen": { "init": null },
                     "items": { "init": { "lit": [ { "id": "a" }, { "id": "b" } ] } } },
          "view": { "kind": "table", "id": "t", "rows": { "ref": "items" }, "selectionMode": "multiple")"} +
                              std::string{selection} + R"(,
                    "onSelectionChange": [ { "set": "seen", "value": { "ref": "event" } } ],
                    "columns": [ { "id": "id", "label": "Id" } ] } })");
        INFO(selection);
        int const table = h.backend.all("Table").front();
        h.backend.selectRows(table, {key("b")});
        h.settle();
        CHECK(readJson(*screen, R"({"ref":"seen"})") == R"(["b"])");
        CHECK(readJson(*screen, R"({"ref":"t.selection"})") == R"(["b"])");
        CHECK(h.backend.prop(table, "selection") == R"(["b"])");
    }
}
