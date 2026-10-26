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

// Mutation: drop the mount's setSelectColumn call (the table records no select column).
TEST_CASE("table: a select column, its header's name and each row's label reach the backend", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "boxes",
      "state": { "items": { "init": { "lit": [ { "id": "S-1" }, { "id": "S-2" } ] } } },
      "view": { "kind": "table", "rows": { "ref": "items" }, "selectionMode": "multiple", "selectColumn": true,
                "selectAllLabel": "Select all on this page", "rowLabel": { "row": "id" },
                "columns": [ { "id": "id", "label": "Sample" } ] } })");
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.prop(table, "selectColumn") == "true");
    CHECK(h.backend.prop(table, "selectAllLabel") == "Select all on this page");
    std::vector<std::string> labels;
    for (int const row : h.backend.children(table)) {
        labels.push_back(h.backend.prop(row, "label"));
    }
    CHECK(labels == std::vector<std::string>{"S-1", "S-2"});
}

namespace {

// The keys of a table's rows, in the order the table holds them, joined by spaces: `x z y`.
std::string rowOrder(Harness const& h, int table) {
    std::string order;
    for (int const row : h.backend.children(table)) {
        std::string const shown = h.backend.prop(row, "rowKey");
        order += (order.empty() ? "" : " ") + (shown.size() >= 2 && shown.front() == '"' ? shown.substr(1, shown.size() - 2) : shown);
    }
    return order;
}

// A table over rows x, z and y whose `id` column is sortable; @p extra goes after the table's `rows`.
std::string sortTable(std::string_view state, std::string_view extra) {
    return std::string{R"({
      "vocab": "ui/1", "id": "sorted",
      "state": { "seen": { "init": null }, "items": { "init": { "lit": [ { "id": "x" }, { "id": "z" }, { "id": "y" } ] } })"} +
           std::string{state} + R"( },
      "view": { "kind": "table", "id": "t", "rows": { "ref": "items" })" + std::string{extra} + R"(,
                "columns": [ { "id": "id", "label": "Id", "sortable": true }, { "id": "other", "label": "Other" } ] } })";
}

constexpr std::string_view kNoteSort = R"(, "onSortChange": [ { "set": "seen", "value": { "ref": "event" } } ])";

long sortReports(Harness const& h) {
    return std::ranges::count_if(h.diagnostics,
                                 [](ui::Refusal const& refusal) { return refusal.path.ends_with("/view/sort"); });
}

}  // namespace

// Mutation: skip the sort in `tableView` (the rows keep their source order).
TEST_CASE("table: a bound sort orders the rows and is the element's sort; a bare record keeps the order, reported once",
          "[ui][interpreter]") {
    {
        Harness h;
        auto screen = h.mount(sortTable(R"(, "order": { "init": { "lit": [ { "column": "id", "direction": "desc" } ] } })",
                                        R"(, "sort": { "ref": "order" })"));
        int const table = h.backend.all("Table").front();
        CHECK(rowOrder(h, table) == "z y x");
        CHECK(readJson(*screen, R"({"ref":"t.sort"})") == R"([{"column":"id","direction":"desc"}])");
        CHECK(h.backend.prop(table, "sort") == "0:Descending");
        CHECK(h.backend.prop(table, "columns") == "[Id:content;sortable=true,Other:content]");
    }
    Harness h;
    auto screen = h.mount(sortTable(R"(, "order": { "init": { "lit": { "column": "id", "direction": "desc" } } })",
                                    R"(, "sort": { "ref": "order" })"));
    int const table = h.backend.all("Table").front();
    CHECK(rowOrder(h, table) == "x z y");
    CHECK(h.backend.prop(table, "sort") == "none");
    CHECK(sortReports(h) == 1);
}

// Mutation: leave the table's own sort unwritten on a header click (the rows stay in source order).
TEST_CASE("table: unbound, a header click sorts ascending, then descending, and onSortChange sees each",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(sortTable("", kNoteSort));
    int const table = h.backend.all("Table").front();
    CHECK(rowOrder(h, table) == "x z y");
    h.backend.clickHeader(table, 0);
    h.settle();
    CHECK(rowOrder(h, table) == "x y z");
    CHECK(readJson(*screen, R"({"ref":"seen"})") == R"([{"column":"id","direction":"asc"}])");
    CHECK(h.backend.prop(table, "sort") == "0:Ascending");
    h.backend.clickHeader(table, 0);
    h.settle();
    CHECK(rowOrder(h, table) == "z y x");
    CHECK(readJson(*screen, R"({"ref":"seen"})") == R"([{"column":"id","direction":"desc"}])");
    CHECK(readJson(*screen, R"({"ref":"t.sort"})") == R"([{"column":"id","direction":"desc"}])");
    // A header that does not sort reports nothing.
    h.backend.clickHeader(table, 1);
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"seen"})") == R"([{"column":"id","direction":"desc"}])");
}

// Mutation: write the table's own sort even when the key is bound (the rows reorder).
TEST_CASE("table: a bound sort the document does not write back runs onSortChange and keeps the order",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(sortTable(R"(, "order": { "init": { "lit": [] } })",
                                    std::string{R"(, "sort": { "ref": "order" })"} + std::string{kNoteSort}));
    int const table = h.backend.all("Table").front();
    h.backend.clickHeader(table, 0);
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"seen"})") == R"([{"column":"id","direction":"asc"}])");
    CHECK(rowOrder(h, table) == "x z y");
    CHECK(h.backend.prop(table, "sort") == "none");
}

// Mutation: order by the row's field named by the column's id, ignoring `sortBy` (the rows order by their text).
TEST_CASE("table: sortBy orders a text column by another value", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "due",
      "state": { "items": { "init": { "lit": [ { "id": "d10", "dueAt": 10 }, { "id": "d2", "dueAt": 2 },
                                               { "id": "d9", "dueAt": 9 } ] } } },
      "view": { "kind": "table", "rows": { "ref": "items" }, "sort": { "lit": [ { "column": "due", "direction": "asc" } ] },
                "columns": [ { "id": "due", "label": "Due", "sortable": true, "sortBy": { "row": "dueAt" },
                               "cell": [ { "kind": "text", "text": { "row": "id" } } ] } ] } })");
    int const table = h.backend.all("Table").front();
    CHECK(rowOrder(h, table) == "d2 d9 d10");
}

// Mutation: key each sorted row by its position instead of `key` (the rows' widgets are made again).
TEST_CASE("table: a selected row keeps its widget and its selection across a sort", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(sortTable("", R"(, "selectionMode": "multiple")"));
    int const table = h.backend.all("Table").front();
    std::vector<int> const before = h.backend.children(table);
    h.backend.selectRows(table, {key("z")});
    h.settle();
    h.backend.clickHeader(table, 0);
    h.settle();
    REQUIRE(rowOrder(h, table) == "x y z");
    std::vector<int> after = h.backend.children(table);
    CHECK(after.at(2) == before.at(1));
    CHECK(h.backend.prop(table, "selection") == R"(["z"])");
    CHECK(readJson(*screen, R"({"ref":"t.selection"})") == R"(["z"])");
}

TEST_CASE("table: values a sort cannot order keep the source order, reported once", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "mixed",
      "state": { "items": { "init": { "lit": [ { "id": "b", "v": 2 }, { "id": "a", "v": "one" } ] } } },
      "view": { "kind": "table", "rows": { "ref": "items" }, "sort": { "lit": [ { "column": "v", "direction": "asc" } ] },
                "columns": [ { "id": "v", "label": "V", "sortable": true } ] } })");
    int const table = h.backend.all("Table").front();
    CHECK(rowOrder(h, table) == "b a");
    CHECK(sortReports(h) == 1);
}

// Mutation: sort by the first key alone (rows of one status keep their source order).
TEST_CASE("table: a sort of two keys orders by each in turn, and a header click makes its column the one key",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "two",
      "state": { "order": { "init": { "lit": [ { "column": "status", "direction": "asc" },
                                               { "column": "due", "direction": "asc" } ] } },
                 "items": { "init": { "lit": [ { "id": "p", "status": "b", "due": 1 }, { "id": "q", "status": "a", "due": 3 },
                                               { "id": "r", "status": "b", "due": 0 }, { "id": "s", "status": "a", "due": 2 } ] } } },
      "view": { "kind": "table", "id": "t", "rows": { "ref": "items" }, "sort": { "ref": "order" },
                "onSortChange": [ { "set": "order", "value": { "ref": "event" } } ],
                "columns": [ { "id": "status", "label": "Status", "sortable": true },
                             { "id": "due", "label": "Due", "sortable": true } ] } })");
    int const table = h.backend.all("Table").front();
    CHECK(rowOrder(h, table) == "s q r p");
    CHECK(h.backend.prop(table, "sort") == "0:Ascending");
    h.backend.clickHeader(table, 1);
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"order"})") == R"([{"column":"due","direction":"asc"}])");
    CHECK(rowOrder(h, table) == "r p s q");
}

// Mutation: compare strings by their bytes alone (`Paracetamol` sorts before `lactose`).
TEST_CASE("table: text sorts case-folded, and by its bytes when it folds the same", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "fold",
      "state": { "items": { "init": { "lit": [ { "id": "Paracetamol" }, { "id": "lactose" }, { "id": "Lactose" } ] } } },
      "view": { "kind": "table", "rows": { "ref": "items" }, "sort": { "lit": [ { "column": "id", "direction": "asc" } ] },
                "columns": [ { "id": "id", "label": "Substance", "sortable": true } ] } })");
    int const table = h.backend.all("Table").front();
    CHECK(rowOrder(h, table) == "Lactose lactose Paracetamol");
}

namespace {

// A table over 57 rows keyed 1 to 57, whose `n` runs from 57 down to 1, 25 rows a page, at the state `page`, its
// selection bound to `selected` and written back; a pager text placed before it, and buttons that turn to pages 0,
// 1, 2 and 9 and select row 51. @p extra goes after the table's `rows`.
std::string pagedTable(std::string_view mode, std::string_view extra = "", std::string_view pageSize = "25") {
    std::string rows;
    for (int id = 1; id <= 57; ++id) {
        rows += (id == 1 ? "" : ", ") + std::string{R"({ "id": )"} + std::to_string(id) + R"(, "n": )" +
                std::to_string(58 - id) + " }";
    }
    std::string buttons;
    for (int const page : {0, 1, 2, 9}) {
        buttons += R"(, { "kind": "button", "testId": "to)" + std::to_string(page) + R"(", "label": "go",
          "onClick": [ { "set": "page", "value": )" + std::to_string(page) + " } ] }";
    }
    return std::string{R"({
      "vocab": "ui/1", "id": "paged",
      "state": { "page": { "init": 0 }, "size": { "init": )"} + std::string{pageSize} + R"( },
                 "selected": { "init": { "lit": [] } }, "items": { "init": { "lit": [ )" + rows + R"( ] } } },
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "pager", "text": { "fn": "format", "args": [ "{0}-{1} of {2}, page {3} of {4}",
            { "ref": "t.pageFirst" }, { "ref": "t.pageLast" }, { "ref": "t.viewCount" }, { "ref": "t.page" },
            { "ref": "t.pageCount" } ] } },
        { "kind": "table", "id": "t", "rows": { "ref": "items" })" + std::string{extra} + R"(,
          "selectionMode": ")" + std::string{mode} + R"(", "selection": { "ref": "selected" },
          "onSelectionChange": [ { "set": "selected", "value": { "ref": "event" } } ],
          "pageSize": { "ref": "size" }, "page": { "ref": "page" },
          "columns": [ { "id": "n", "label": "N", "sortable": true } ] },
        { "kind": "button", "testId": "pick51", "label": "51",
          "onClick": [ { "set": "selected", "value": { "lit": [ 51 ] } } ] })" + buttons + R"( ] } })";
}

void turnTo(Harness& h, int page) {
    h.backend.click(h.byTestId("Button", "to" + std::to_string(page)));
    h.settle();
}

ui::Key id(std::int64_t value) { return ui::Key{value}; }

}  // namespace

// Mutations: page before sorting (the last page holds rows 51 to 57 of the source order); compute the page bounds in
// the lowering instead of on the element (the pager, drawn first, reads the defaults).
TEST_CASE("table: a table pages its sorted rows, and a pager drawn before it reads the pages", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(pagedTable("multiple", R"(, "sort": { "lit": [ { "column": "n", "direction": "asc" } ] })"));
    int const table = h.backend.all("Table").front();
    CHECK(h.text("pager") == "1-25 of 57, page 0 of 3");
    CHECK(h.backend.children(table).size() == 25);
    turnTo(h, 1);
    CHECK(h.backend.children(table).size() == 25);
    CHECK(h.text("pager") == "26-50 of 57, page 1 of 3");
    turnTo(h, 2);
    CHECK(h.text("pager") == "51-57 of 57, page 2 of 3");
    CHECK(rowOrder(h, table) == "7 6 5 4 3 2 1");
}

// Mutation: clamp nothing (page 9 shows no rows and reads 9).
TEST_CASE("table: a page past the last shows the last, and the element reads it", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(pagedTable("multiple"));
    turnTo(h, 9);
    int const table = h.backend.all("Table").front();
    CHECK(rowOrder(h, table) == "51 52 53 54 55 56 57");
    CHECK(readJson(*screen, R"({"ref":"t.page"})") == "2");
    CHECK(h.diagnostics.empty());
}

TEST_CASE("table: a computed pageSize of 0 is reported once and shows every row", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(pagedTable("multiple", "", "0"));
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.children(table).size() == 57);
    CHECK(h.text("pager") == "1-57 of 57, page 0 of 1");
    auto const reports = std::ranges::count_if(
        h.diagnostics, [](ui::Refusal const& refusal) { return refusal.path.ends_with("/pageSize"); });
    CHECK(reports == 1);
}

// Mutation: skip the merge (the Ctrl-click on page 1 reports only row 26).
TEST_CASE("table: a Ctrl-click on another page keeps the keys selected on the first", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(pagedTable("multiple"));
    int const table = h.backend.all("Table").front();
    h.backend.clickRow(table, id(1), ui::RowClick::Only);
    h.settle();
    h.backend.clickRow(table, id(2), ui::RowClick::Toggle);
    h.settle();
    turnTo(h, 1);
    h.backend.clickRow(table, id(26), ui::RowClick::Toggle);
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"selected"})") == "[1,2,26]");
    CHECK(h.text("pager") == "26-50 of 57, page 1 of 3");
}

// Mutations: merge on every report, ignoring its kind (the plain click keeps row 51); never merge (the last
// Ctrl-click drops row 51).
TEST_CASE("table: a plain click replaces the selection on every page, a Ctrl-click keeps the other pages'",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(pagedTable("multiple"));
    int const table = h.backend.all("Table").front();
    h.backend.click(h.byTestId("Button", "pick51"));
    h.settle();
    h.backend.clickRow(table, id(3), ui::RowClick::Only);
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"selected"})") == "[3]");
    h.backend.clickRow(table, id(4), ui::RowClick::Toggle);
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"selected"})") == "[3,4]");
    h.backend.click(h.byTestId("Button", "pick51"));
    h.settle();
    h.backend.clickRow(table, id(4), ui::RowClick::Toggle);
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"selected"})") == "[51,4]");
}

// Mutation: merge on every report (the single table holds two keys).
TEST_CASE("table: a single table's pick on another page is its whole selection", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(pagedTable("single"));
    int const table = h.backend.all("Table").front();
    h.backend.clickRow(table, id(3), ui::RowClick::Only);
    h.settle();
    turnTo(h, 1);
    h.backend.clickRow(table, id(30), ui::RowClick::Only);
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"selected"})") == "[30]");
}

TEST_CASE("table: a selected key whose row is gone drops on the next user change", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "gone",
      "state": { "selected": { "init": { "lit": [ 99, 1 ] } },
                 "items": { "init": { "lit": [ { "id": 1 }, { "id": 2 }, { "id": 3 } ] } } },
      "view": { "kind": "table", "id": "t", "rows": { "ref": "items" }, "selectionMode": "multiple",
                "selection": { "ref": "selected" }, "pageSize": 2, "page": 1,
                "onSelectionChange": [ { "set": "selected", "value": { "ref": "event" } } ],
                "columns": [ { "id": "id", "label": "Id" } ] } })");
    int const table = h.backend.all("Table").front();
    h.backend.clickRow(table, id(3), ui::RowClick::Toggle);
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"selected"})") == "[1,3]");
}

// Mutation: skip the merge (Ctrl+A on the first page drops row 51).
TEST_CASE("table: Ctrl+A selects the page's rows and keeps a key selected on another page", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(pagedTable("multiple"));
    int const table = h.backend.all("Table").front();
    h.backend.click(h.byTestId("Button", "pick51"));
    h.settle();
    CHECK(h.backend.press(table, "Ctrl+A"));
    h.settle();
    std::string expected = "[51";
    for (int row = 1; row <= 25; ++row) {
        expected += "," + std::to_string(row);
    }
    CHECK(readJson(*screen, R"({"ref":"selected"})") == expected + "]");
}

namespace {

// A table's rows in order: a data row as its key, a heading row as `[<its text>]`.
std::string entryOrder(Harness const& h, int table) {
    std::string order;
    for (int const row : h.backend.children(table)) {
        std::string shown;
        if (h.backend.prop(row, "heading") == "true") {
            int cell = row;
            while (h.backend.kindOf(cell) != "Text" && !h.backend.children(cell).empty()) {
                cell = h.backend.children(cell).front();
            }
            shown = "[" + h.backend.prop(cell, "text") + "]";
        } else {
            shown = h.backend.prop(row, "rowKey");
        }
        order += (order.empty() ? "" : " ") + shown;
    }
    return order;
}

// Results of samples grouped by sample: S-1 holds results 1 to 3, S-2 results 4 to 5; @p extra goes after the
// table's `rows`, and @p results replaces the rows.
std::string groupedTable(std::string_view extra = "", std::string_view results = "") {
    std::string const rows = results.empty() ? std::string{R"([ { "id": 1, "sample": "S-1", "lot": "L-1", "area": 30 },
        { "id": 2, "sample": "S-1", "lot": "L-1", "area": 10 }, { "id": 3, "sample": "S-1", "lot": "L-1", "area": 20 },
        { "id": 4, "sample": "S-2", "lot": "L-2", "area": 5 }, { "id": 5, "sample": "S-2", "lot": "L-2", "area": 1 } ])"}
                                             : std::string{results};
    return std::string{R"({
      "vocab": "ui/1", "id": "grouped",
      "state": { "items": { "init": { "lit": )"} + rows + R"( } } },
      "view": { "kind": "column", "children": [
        { "kind": "table", "id": "t", "rows": { "ref": "items" }, "selectionMode": "multiple")" + std::string{extra} + R"(,
          "group": { "by": { "row": "sample" },
                     "heading": { "kind": "text", "text": { "fn": "format", "args": [ "{0} · lot {1} · {2}",
                         { "row": "key" }, { "row": "first.lot" }, { "row": "count" } ] } } },
          "columns": [ { "id": "area", "label": "Peak area", "sortable": true } ] },
        { "kind": "text", "testId": "count", "text": { "ref": "t.selectedCount" } } ] } })";
}

}  // namespace

TEST_CASE("table: a group draws a heading row before each sample's results, reading the group", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(groupedTable());
    int const table = h.backend.all("Table").front();
    CHECK(entryOrder(h, table) == "[S-1 · lot L-1 · 3] 1 2 3 [S-2 · lot L-2 · 2] 4 5");
    CHECK(readJson(*screen, R"({"ref":"t.viewCount"})") == "5");
}

TEST_CASE("table: Ctrl+A selects the data rows, not the headings", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(groupedTable());
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.press(table, "Ctrl+A"));
    h.settle();
    CHECK(h.text("count") == "5");
    CHECK(readJson(*screen, R"({"ref":"t.selection"})") == "[1,2,3,4,5]");
}

// Mutation: sort the whole list before grouping (a group's rows leave its heading).
TEST_CASE("table: a sorted grouped table sorts within each group, which keeps its place", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(groupedTable(R"(, "sort": { "lit": [ { "column": "area", "direction": "asc" } ] })"));
    int const table = h.backend.all("Table").front();
    CHECK(entryOrder(h, table) == "[S-1 · lot L-1 · 3] 2 3 1 [S-2 · lot L-2 · 2] 5 4");
}

// Mutation: make the runs over the page's rows instead of every row (the second page's heading reads 1).
TEST_CASE("table: a group split across pages has its heading on both, reading the whole group", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(groupedTable(R"(, "pageSize": 2, "page": 1)"));
    int const table = h.backend.all("Table").front();
    CHECK(entryOrder(h, table) == "[S-1 · lot L-1 · 3] 3 [S-2 · lot L-2 · 2] 4");
    CHECK(readJson(*screen, R"({"ref":"t.pageFirst"})") == "3");
    CHECK(readJson(*screen, R"({"ref":"t.pageLast"})") == "4");
}

// Mutation: key a heading by its run's key alone (the third run's heading is a duplicate key and left out).
TEST_CASE("table: groups A, B, A give three headings and no duplicate key", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(groupedTable("", R"([ { "id": 1, "sample": "A", "lot": "L", "area": 1 },
        { "id": 2, "sample": "B", "lot": "L", "area": 1 }, { "id": 3, "sample": "A", "lot": "L", "area": 1 } ])"));
    int const table = h.backend.all("Table").front();
    CHECK(entryOrder(h, table) == "[A · lot L · 1] 1 [B · lot L · 1] 2 [A · lot L · 1] 3");
    CHECK(h.diagnostics.empty());
}
