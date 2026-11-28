// SPDX-License-Identifier: Apache-2.0

// The header and filter menus, the column chooser and table dialogs.
//
// Part of the client table model tests; see docs/spec/table/table.md.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace table = morph::table;

using namespace table_model_support;

using morph::ui::Value;
using morph::ui::testing::pageJson;
using morph::ui::testing::samplesDescription;
using namespace std::chrono_literals;
using morph::ui::HeaderClick;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::clickFilter;
using morph::ui::testing::clickGroupFilter;
using morph::ui::testing::elementValue;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::kConfigModelTable;
using morph::ui::testing::kNine;
using morph::ui::testing::labelsOf;
using morph::ui::testing::lastQuery;
using morph::ui::testing::menuOf;
using morph::ui::testing::moveColumn;
using morph::ui::testing::queryOf;
using morph::ui::testing::readJson;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::replyPage;
using morph::ui::testing::statusOf;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;
using morph::ui::testing::withTableKey;

// Mutation: the lowering applying a stored layout after the first query.

TEST_CASE("persisted layout: a stored layout applies before the first query, a change saves once after the debounce",
          "[ui][table][config]") {
    constexpr std::string_view kLayout =
        R"({"version":1,"layout":{"order":["c2","c1"],"hidden":["c3"],"pinned":0,"widths":{"c2":20}},)"
        R"("sort":[{"column":"c4","direction":"desc"}]})";
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kLayout});
    morph::ui::testing::TableHarness h{store};
    SECTION("the table's own layout") {
        auto const screen = h.mount(kNine);
        int const table = h.backend.all("Table").front();
        CHECK(morph::ui::testing::rowOrder(h).empty());  // the rows wait for the stored state
        store->release();
        h.settle();
        // c3 to c9 are placed after their nearest declared predecessor the layout places, c2; c3 stays hidden.
        CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c2","c4","c5","c6","c7","c8","c9","c1"])");
        CHECK(elementValue(*screen, "t", "layout").contains(R"("widths":{"c2":20})"));
        CHECK_FALSE(morph::ui::testing::rowOrder(h).empty());
        chooseMenu(h, table, 0, {"Hide column"});
        h.advance(100ms);
        chooseMenu(h, table, 3, {"Hide column"});
        h.advance(100ms);
        chooseMenu(h, table, 4, {"Hide column"});
        CHECK(store->saves() == 0);
        h.advance(500ms);
        REQUIRE(store->saves() == 1);
        auto const saved = table::readState(store->lastSaved());
        REQUIRE(saved.has_value());
        REQUIRE(saved->layout.has_value());
        std::vector<std::string> hiddenColumns = saved->layout->hidden;
        std::ranges::sort(hiddenColumns);
        CHECK(hiddenColumns == std::vector<std::string>{"c1", "c3", "c4", "c5"});
    }
    SECTION("a model's table: the first query names only the columns the stored layout shows") {
        auto const modelStore = std::make_shared<GatedTableStateStore>(
            storeExecutor(),
            R"({"version":1,"layout":{"order":["name","id","c3","c4","c5"],"hidden":["c3"],"pinned":0,"widths":{}}})");
        morph::ui::testing::TableHarness m{modelStore};
        auto const screen = m.mount(kConfigModelTable);
        replyDescribe(m, samplesDescription(5));
        REQUIRE(m.dispatcher.count("Table.samples") == 0);  // the first query waits for the state
        modelStore->release();
        m.settle();
        REQUIRE(m.dispatcher.count("Table.samples") >= 1);
        std::size_t firstCall = 0;
        while (m.dispatcher.calls.at(firstCall).action != "Table.samples") {
            ++firstCall;
        }
        table::TableQuery const first = queryOf(m, firstCall);
        REQUIRE(first.columns.has_value());
        CHECK_FALSE(std::ranges::contains(*first.columns, "c3"));
        CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["name","id","c4","c5"])");
    }
    SECTION("a bound layout") {
        auto const screen = h.mount(withTableKey(withState(kNine, "lay", "null"), R"("layout":{"ref":"lay"})"));
        int const table = h.backend.all("Table").front();
        store->release();
        h.settle();
        CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1","c2","c3","c4","c5","c6","c7","c8","c9"])");
        CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c4","direction":"desc"}])");
        chooseMenu(h, table, 0, {"Hide column"});
        h.advance(500ms);
        CHECK(store->saves() == 0);  // the layout is the document's
        h.backend.clickHeader(table, 1, HeaderClick::Plain);
        h.advance(500ms);
        REQUIRE(store->saves() == 1);
        auto const saved = table::readState(store->lastSaved());
        REQUIRE(saved.has_value());
        CHECK(saved->sort == table::SortChain{{.column = "c2", .dir = table::SortDirection::Ascending}});
        INFO(store->lastSaved());
        REQUIRE(saved->layout.has_value());  // the stored one, as it was kept
        CHECK(saved->layout->order == std::vector<std::string>{"c2", "c1"});
        CHECK_FALSE(std::ranges::contains(saved->layout->hidden, "c1"));  // the user's hide is not saved
        CHECK(saved->layout->hidden == std::vector<std::string>{"c3"});   // a sort-only save keeps it whole
    }
}

// Review Focus 1. Mutation: hiding a column removing it from the query's sort.

TEST_CASE("hiding a sorted and filtered column changes no row", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto const screen = h.mount(withTableKey(
        kConfigModelTable,
        R"("sort":{"lit":[{"column":"name","direction":"asc"}]},"filters":{"columns":{"name":{"include":[{"contains":"a"}]}}})"));
    replyDescribe(h, samplesDescription(5));
    table::TableQuery const first = lastQuery(h);
    REQUIRE(first.columns.has_value());
    replyPage(h, pageJson(0, 50, 50, *first.columns));
    int const table = h.backend.all("Table").front();
    std::string const rows = morph::ui::testing::rowOrder(h);
    REQUIRE_FALSE(rows.empty());
    std::size_t const calls = h.dispatcher.count("Table.samples");
    CHECK(h.backend.prop(table, "sort") == "1:Ascending");
    CHECK(h.backend.prop(table, "filtered") == "[1]");
    chooseMenu(h, table, 1, {"Hide column"});
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["id","c3","c4","c5"])");
    if (h.dispatcher.count("Table.samples") > calls) {  // a column fetch of the rows held
        table::TableQuery const fetched = lastQuery(h);
        CHECK(fetched.sort == first.sort);
        CHECK(fetched.filters == first.filters);
        REQUIRE(fetched.columns.has_value());
        CHECK_FALSE(std::ranges::contains(*fetched.columns, "name"));
    }
    CHECK(morph::ui::testing::rowOrder(h) == rows);
    chooseMenu(h, table, std::nullopt, {"Reset columns"});
    CHECK(h.backend.prop(table, "sort") == "1:Ascending");
    CHECK(h.backend.prop(table, "filtered") == "[1]");
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["id","name","c3","c4","c5"])");
}

// Review Focus 2 (message half). Mutation: refusing Shift on a column already in the chain at the bound.

TEST_CASE("sort chains: at the bound a key in the chain still flips, with no message", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    for (std::size_t column = 0; column < 8; ++column) {
        h.backend.clickHeader(table, column, HeaderClick::Shift);
    }
    h.settle();
    REQUIRE(statusOf(h, table).message.empty());
    h.backend.clickHeader(table, 3, HeaderClick::Shift);
    h.settle();
    Value const sort = screen->read(*morph::ui::parseJson(R"({"ref":"t.sort"})"));
    REQUIRE(sort.asList() != nullptr);
    REQUIRE(sort.asList()->size() == 8);
    CHECK(*sort.asList()->at(3).field("direction") == Value{std::string{"desc"}});
    CHECK(statusOf(h, table).message.empty());
}

namespace {

// The column chooser's show boxes, in the order they were listed when it opened.

std::vector<std::string> chooserColumns(morph::ui::testing::TableHarness const& h) {
    std::vector<std::string> out;
    for (int const box : h.backend.all("Checkbox")) {
        if (std::string const id = h.backend.prop(box, "testId"); id.starts_with("table_columns_show_")) {
            out.push_back(id.substr(std::string_view{"table_columns_show_"}.size()));
        }
    }
    return out;
}

}  // namespace

// Mutation: the chooser listing only the visible columns.

TEST_CASE("column chooser: every column in order, with show, move and reset", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto const screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, 2, {"Hide column"});
    REQUIRE(elementValue(*screen, "t", "hiddenCount") == "1");
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_columns"), "open") == "true");
    CHECK(chooserColumns(h) == std::vector<std::string>{"c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9"});
    int const c3 = h.byTestId("Checkbox", "table_columns_show_c3");
    CHECK(h.backend.prop(c3, "checked") == "false");
    CHECK(h.backend.prop(h.byTestId("Checkbox", "table_columns_show_c1"), "checked") == "true");
    h.backend.toggle(c3);
    h.settle();
    CHECK(elementValue(*screen, "t", "hiddenCount") == "0");
    CHECK(h.backend.prop(c3, "checked") == "true");
    CHECK_FALSE(enabled(h, "Button", "table_columns_up_c1"));
    press(h, "table_columns_down_c1");
    CHECK(elementValue(*screen, "t", "visibleColumns").starts_with(R"(["c2","c1",)"));
    press(h, "table_columns_reset");
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1","c2","c3","c4","c5","c6","c7","c8","c9"])");
    press(h, "table_columns_done");
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_columns"), "open") == "false");
}

// Mutations: group creation accepting columns of two kinds (n stays enabled once a is ticked); the Remove group
// button closing the line without applying RemoveGroup (the layout keeps g1).

TEST_CASE("column chooser: Group columns forms a group of one kind, which the layout keeps", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto const screen = h.mount(countingNine("onLayoutChange", kKinds));
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    press(h, "table_columns_group");
    h.backend.toggle(h.byTestId("Checkbox", "table_group_column_a"));
    h.settle();
    CHECK_FALSE(enabled(h, "Checkbox", "table_group_column_n"));
    CHECK(enabled(h, "Checkbox", "table_group_column_b"));
    CHECK_FALSE(enabled(h, "Button", "table_group_create"));
    h.backend.edit(h.byTestId("TextInput", "table_group_label"), "Pair");
    h.settle();
    CHECK_FALSE(enabled(h, "Button", "table_group_create"));  // a label and one column
    h.backend.toggle(h.byTestId("Checkbox", "table_group_column_b"));
    h.settle();
    REQUIRE(enabled(h, "Button", "table_group_create"));
    press(h, "table_group_create");
    CHECK(readJson(*screen, R"({"ref":"t.layout.groups"})") == R"([{"columns":["a","b"],"id":"g1","label":"Pair"}])");
    CHECK(std::ranges::contains(labelsOf(menuOf(h, table, 0)), "Filter Pair, 2 columns…"));
    CHECK(h.text("table_group_g1") == "Pair, 2 columns");
    CHECK(readJson(*screen, R"({"ref":"n"})") == "1");
    // A column already in a group is refused in the status line, and the form stays as the user left it.
    press(h, "table_columns_group");
    h.backend.toggle(h.byTestId("Checkbox", "table_group_column_a"));
    h.backend.toggle(h.byTestId("Checkbox", "table_group_column_c"));
    h.backend.edit(h.byTestId("TextInput", "table_group_label"), "More");
    h.settle();
    press(h, "table_group_create");
    CHECK(statusOf(h, table).message == "A is already in Pair");
    CHECK(h.backend.prop(h.byTestId("Checkbox", "table_group_column_c"), "checked") == "true");
    CHECK(readJson(*screen, R"({"ref":"n"})") == "1");
    press(h, "table_group_remove_g1");
    CHECK(readJson(*screen, R"({"ref":"t.layout.groups"})") == "null");
    CHECK_FALSE(std::ranges::contains(labelsOf(menuOf(h, table, 0)), "Filter Pair, 2 columns…"));
    CHECK(readJson(*screen, R"({"ref":"n"})") == "2");
}

// Mutation: the sort dialog's move applying to the wrong neighbour.

TEST_CASE("sort dialog: keys in order with direction, move and remove; Add key refuses at the bound",
          "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    h.backend.clickHeader(table, 0, HeaderClick::Plain);
    h.backend.clickHeader(table, 1, HeaderClick::Shift);
    h.backend.clickHeader(table, 1, HeaderClick::Shift);
    h.settle();
    REQUIRE(elementValue(*screen, "t", "sort") ==
            R"([{"column":"c1","direction":"asc"},{"column":"c2","direction":"desc"}])");
    chooseMenu(h, table, std::nullopt, {"Sort…"});
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_sort"), "open") == "true");
    press(h, "table_sort_up_1");
    CHECK(elementValue(*screen, "t", "sort") ==
          R"([{"column":"c2","direction":"desc"},{"column":"c1","direction":"asc"}])");
    h.backend.choose(h.byTestId("Select", "table_sort_direction_0"), morph::ui::Key{std::string{"ascending"}});
    h.settle();
    CHECK(elementValue(*screen, "t", "sort") ==
          R"([{"column":"c2","direction":"asc"},{"column":"c1","direction":"asc"}])");
    press(h, "table_sort_remove_1");
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c2","direction":"asc"}])");
    for (int key = 1; key < 8; ++key) {
        press(h, "table_sort_add");
    }
    Value sort = screen->read(*morph::ui::parseJson(R"({"ref":"t.sort"})"));
    REQUIRE(sort.asList() != nullptr);
    REQUIRE(sort.asList()->size() == 8);
    CHECK(statusOf(h, table).message.empty());
    press(h, "table_sort_add");
    sort = screen->read(*morph::ui::parseJson(R"({"ref":"t.sort"})"));
    REQUIRE(sort.asList() != nullptr);
    CHECK(sort.asList()->size() == 8);
    CHECK(statusOf(h, table).message == "A table sorts by at most 8 columns");
    press(h, "table_sort_done");
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_sort"), "open") == "false");
}

// Mutations: the move buttons read without the pin boundary (c3, the first unpinned column, can move up); the show
// box of the only visible column enabled; the chooser's dismissal leaving it open; a key's column select offering a
// column another key holds.

TEST_CASE("column chooser and sort dialog: locks, the pin boundary, a key's column and a dismissal",
          "[ui][table][config]") {
    {
        morph::ui::testing::TableHarness h;
        auto const screen = h.mount(withFirstColumnKey(kNine, R"("locked":true)"));
        int const table = h.backend.all("Table").front();
        chooseMenu(h, table, 1, {"Pin column"});
        moveColumn(h, table, 2, 1);
        REQUIRE(elementValue(*screen, "t", "layout").contains(R"("pinned":1)"));
        REQUIRE(elementValue(*screen, "t", "visibleColumns").starts_with(R"(["c2","c3","c1",)"));
        chooseMenu(h, table, std::nullopt, {"Columns…"});
        CHECK_FALSE(enabled(h, "Checkbox", "table_columns_show_c1"));  // locked
        CHECK_FALSE(enabled(h, "Button", "table_columns_down_c1"));
        CHECK_FALSE(enabled(h, "Button", "table_columns_down_c2"));  // the last pinned column
        CHECK_FALSE(enabled(h, "Button", "table_columns_up_c3"));    // the first unpinned one
        CHECK(enabled(h, "Button", "table_columns_down_c3"));
        h.backend.dismiss(h.byTestId("Dialog", "table_columns"));
        h.settle();
        CHECK(h.backend.prop(h.byTestId("Dialog", "table_columns"), "open") == "false");
    }
    morph::ui::testing::TableHarness h;
    auto const screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    for (std::string_view const column : {"c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9"}) {
        h.backend.toggle(h.byTestId("Checkbox", "table_columns_show_" + std::string{column}));
    }
    h.settle();
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1"])");
    CHECK_FALSE(enabled(h, "Checkbox", "table_columns_show_c1"));  // the only visible column
    CHECK(enabled(h, "Checkbox", "table_columns_show_c2"));
    press(h, "table_columns_done");

    h.backend.clickHeader(table, 0, HeaderClick::Plain);
    h.settle();
    REQUIRE(elementValue(*screen, "t", "sort") == R"([{"column":"c1","direction":"asc"}])");
    chooseMenu(h, table, std::nullopt, {"Sort…"});
    press(h, "table_sort_add");
    REQUIRE(elementValue(*screen, "t", "sort") ==
            R"([{"column":"c1","direction":"asc"},{"column":"c2","direction":"asc"}])");
    int const second = h.byTestId("Select", "table_sort_column_1");
    CHECK_THROWS(h.backend.choose(second, morph::ui::Key{std::string{"c1"}}));  // c1 is the first key's
    h.backend.choose(second, morph::ui::Key{std::string{"c5"}});
    h.settle();
    CHECK(elementValue(*screen, "t", "sort") ==
          R"([{"column":"c1","direction":"asc"},{"column":"c5","direction":"asc"}])");
    h.backend.dismiss(h.byTestId("Dialog", "table_sort"));
    h.settle();
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_sort"), "open") == "false");
}

namespace {

// kNine's table in a row beside a text, the table's node taking the member @p member.

std::string nineInRow(std::string_view member) {
    auto json = morph::ui::parseJson(withTableKey(withTableKey(kNine, R"("testId":"t")"), member));
    REQUIRE(json.has_value());
    auto& root = std::get<morph::ui::Json::object_t>(json->data);
    std::string const table = glz::write_json(root.at("view")).value_or("");
    root.erase("view");
    std::string text = glz::write_json(*json).value_or("");
    text.pop_back();
    return text + R"(,"view":{"kind":"row","children":[)" + table +
           R"(,{"kind":"text","text":"beside","sizing":{"lit":{"stretch":1}}}]}})";
}

}  // namespace

// Mutation: the table inside the dialogs' column keeping its percent (the mount throws: a column's child takes no
// share).

TEST_CASE("table dialogs: a configurable table with a share of its row mounts, its column taking the share",
          "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto const screen = h.mount(nineInRow(R"("sizing":{"lit":{"percent":40}})"));
    int const table = h.byTestId("Table", "t");
    CHECK(h.backend.prop(table, "layout") == "stretch(1)/stretch(1)");
    CHECK(h.backend.dump().contains("Column#2 gap=0 layout=percent(40)/content"));
    CHECK(h.diagnostics.empty());
}

// Mutation: the table inside the dialogs' column keeping a content height (the row's offer to fill its height reaches
// only the column, and the table stops at its own).

TEST_CASE("table dialogs: a content-height table in a row fills the column the row stretches", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto const screen = h.mount(nineInRow(R"("maxWidth":50)"));
    int const table = h.byTestId("Table", "t");
    CHECK(h.backend.prop(table, "layout") == "content max=50/stretch(1)");
    CHECK(h.backend.dump().contains("Column#2 gap=0 layout=content max=50/content"));
}

namespace {

// A column filter read back from the table's `filters` element value.

table::ColumnFilter columnFilterOf(morph::ui::Screen const& screen, std::string_view column) {
    table::ColumnFilter filter;
    std::string const text = readJson(screen, R"({"ref":"t.filters.columns.)" + std::string{column} + R"("})");
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(filter, text));
    return filter;
}

}  // namespace

// Mutation: Apply writing the side the plan did not choose (the ticked values where it chose the unticked ones).

TEST_CASE("filter menu: values are counted over what the other filters leave, and ticks build the smaller side",
          "[ui][table][config]") {
    TableHarness h;
    auto const screen = h.mount(kStatusRows);
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    chooseOption(h, "table_filter_op_1", "equals");
    typeInto(h, "table_filter_operand_1", "A");
    h.advance(250ms);
    REQUIRE(readJson(*screen, R"({"ref":"t.filters.columns.site"})") ==
            R"({"combine":"any","exclude":[],"include":[{"eq":"A"}]})");
    dismissMenu(h);
    std::int64_t const runs = std::stoll(readJson(*screen, R"({"ref":"n"})"));
    clickFilter(h, table, 0);
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "label") == "Closed (2)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_1", "label") == "Open (2)");
    CHECK_FALSE(mounted(h, "Checkbox", "table_filter_value_2"));  // no Held
    CHECK(propOf(h, "Checkbox", "table_filter_blanks", "label") == "(Blanks)");
    CHECK(propOf(h, "Checkbox", "table_filter_blanks", "visible") != "false");
    toggleBox(h, "table_filter_value_1");
    pressButton(h, "table_filter_apply");
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.status"})") ==
          R"({"combine":"any","exclude":[{"oneOf":["Open"]}],"include":[]})");
    CHECK(propOf(h, "Dialog", "table_filter", "open") == "false");
    CHECK(std::stoll(readJson(*screen, R"({"ref":"n"})")) == runs + 1);
    CHECK(h.backend.prop(table, "filtered") == "[0,1]");
    CHECK(elementValue(*screen, "t", "viewCount") == "3");
    clickFilter(h, table, 0);
    CHECK(propOf(h, "Checkbox", "table_filter_value_1", "label") == "Open (2)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_1", "checked") == "false");
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "checked") == "true");
    dismissMenu(h);
    // Hiding a filtered column keeps its filter, and its header says so again once it is shown.
    chooseMenu(h, table, 0, {"Hide column"});
    CHECK(elementValue(*screen, "t", "viewCount") == "3");
    CHECK(h.backend.prop(table, "filtered") == "[0,1]");
    chooseMenu(h, table, std::nullopt, {"Reset columns"});
    CHECK(h.backend.prop(table, "filtered") == "[0,1]");
    chooseMenu(h, table, std::nullopt, {"Clear all filters"});
    CHECK(elementValue(*screen, "t", "filters") == R"({"columns":{},"groups":[]})");
    CHECK(h.backend.prop(table, "filtered") == "[]");
    CHECK(std::stoll(readJson(*screen, R"({"ref":"n"})")) == runs + 2);
}

// Mutation: a condition's edit keeping the ticks (the values part stays beside the condition).

TEST_CASE("filter menu: values and conditions are exclusive, and reopening shows the stored part",
          "[ui][table][config]") {
    SECTION("a text column") {
        TableHarness h;
        auto const screen = h.mount(kStatusRows);
        int const table = h.backend.all("Table").front();
        clickFilter(h, table, 0);
        selectAll(h, false);
        toggleBox(h, "table_filter_blanks");
        pressButton(h, "table_filter_apply");
        CHECK(readJson(*screen, R"({"ref":"t.filters.columns.status"})") ==
              R"({"combine":"any","exclude":[],"include":[{"isEmpty":true}]})");
        clickFilter(h, table, 0);
        CHECK(propOf(h, "Checkbox", "table_filter_blanks", "checked") == "true");
        for (std::string_view const value : {"table_filter_value_0", "table_filter_value_1", "table_filter_value_2"}) {
            CHECK(propOf(h, "Checkbox", value, "checked") == "false");
        }
        CHECK(propOf(h, "Select", "table_filter_op_1", "selected") == "none");
        chooseOption(h, "table_filter_op_1", "contains");
        typeInto(h, "table_filter_operand_1", "pe");
        h.advance(250ms);
        CHECK(readJson(*screen, R"({"ref":"t.filters.columns.status"})") ==
              R"({"combine":"any","exclude":[],"include":[{"contains":"pe"}]})");
        CHECK(propOf(h, "Checkbox", "table_filter_blanks", "checked") == "true");  // the ticks start again
        CHECK(propOf(h, "Checkbox", "table_filter_all", "checked") == "true");
        dismissMenu(h);
        clickFilter(h, table, 0);
        CHECK(propOf(h, "Select", "table_filter_op_1", "selected") == R"("contains")");
        CHECK(propOf(h, "TextInput", "table_filter_operand_1", "text") == "pe");
        CHECK(propOf(h, "Checkbox", "table_filter_all", "checked") == "true");
    }
    SECTION("a custom column has conditions alone") {
        TableHarness h;
        auto const screen =
            mountDescribed(h, kindsDescription(),
                           withTableKey(morph::ui::testing::kConfigModelTable,
                                        R"("filters":{"columns":{"code":{"include":[{"isEmpty":true}]}}})"));
        int const table = h.backend.all("Table").front();
        std::size_t const calls = h.dispatcher.count("Table.samples");
        clickFilter(h, table, 3);
        CHECK(propOf(h, "Select", "table_filter_op_1", "selected") == R"("is empty")");
        CHECK_FALSE(mounted(h, "Checkbox", "table_filter_value_0"));
        CHECK_FALSE(mounted(h, "TextInput", "table_filter_search"));
        CHECK(h.dispatcher.count("Table.samples") == calls);  // no values asked for
    }
}

// Mutation: the search sent on every keystroke (no debounce).

TEST_CASE("filter menu: a search typed quickly is one distinct request after the pause", "[ui][table][config]") {
    TableHarness h;
    auto const screen = mountDescribed(h, samplesDescription(5));
    int const table = h.backend.all("Table").front();
    std::size_t const before = h.dispatcher.count("Table.samples");
    clickFilter(h, table, 1);
    REQUIRE(h.dispatcher.count("Table.samples") == before + 1);
    REQUIRE(lastQuery(h).distinct.has_value());
    h.reply(lastDistinct(h),
            distinctReply(h.described.revision, R"({"values":[{"text":"a","operand":"a","count":1}],"distinct":1})"));
    typeInto(h, "table_filter_search", "a");
    h.advance(50ms);
    typeInto(h, "table_filter_search", "ab");
    h.advance(50ms);
    typeInto(h, "table_filter_search", "abc");
    h.advance(249ms);
    CHECK(h.dispatcher.count("Table.samples") == before + 1);
    h.advance(1ms);
    CHECK(h.dispatcher.count("Table.samples") == before + 2);
    REQUIRE(lastQuery(h).distinct.has_value());
    CHECK(lastQuery(h).distinct->search == "abc");
}

// Mutation: the condition sent on every keystroke, or a half-typed between sent.

TEST_CASE("filter menu: a condition applies once after typing pauses, and an incomplete between is not sent",
          "[ui][table][config]") {
    TableHarness h;
    auto const screen = mountDescribed(h, samplesDescription(5));
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    chooseOption(h, "table_filter_op_1", "contains");
    std::string const letters = "abcdefghij";
    for (std::size_t at = 1; at <= letters.size(); ++at) {
        typeInto(h, "table_filter_operand_1", letters.substr(0, at));
        if (at < letters.size()) {
            h.advance(150ms);
        }
    }
    std::size_t const pages = pageCalls(h);
    h.advance(249ms);
    CHECK(pageCalls(h) == pages);
    h.advance(1ms);
    REQUIRE(pageCalls(h) == pages + 1);
    REQUIRE(lastQuery(h).filters.columns.contains("name"));
    CHECK(lastQuery(h).filters.columns.at("name").include.front().contains == "abcdefghij");
    dismissMenu(h);

    clickFilter(h, table, 2);
    chooseOption(h, "table_filter_op_1", "between");
    typeInto(h, "table_filter_operand_1", "2");
    std::size_t const waiting = pageCalls(h);
    h.advance(1s);
    CHECK(pageCalls(h) == waiting);  // no second end
    typeInto(h, "table_filter_second_1", "5");
    h.advance(250ms);
    REQUIRE(pageCalls(h) == waiting + 1);
    REQUIRE(lastQuery(h).filters.columns.contains("c3"));
    CHECK(lastQuery(h).filters.columns.at("c3").include.front().between == std::array<std::string, 2>{"2", "5"});
    typeInto(h, "table_filter_operand_1", "3");
    pressButton(h, "table_filter_apply");
    REQUIRE(pageCalls(h) == waiting + 2);  // at once
    CHECK(lastQuery(h).filters.columns.at("c3").include.front().between == std::array<std::string, 2>{"3", "5"});
}

// Mutation: the note line ignoring `partial`.

TEST_CASE("filter menu: a partial list says it may grow and invalid cells are counted under it",
          "[ui][table][config]") {
    TableHarness h;
    auto const screen = mountDescribed(h, samplesDescription(5));
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    h.reply(lastDistinct(h),
            distinctReply(h.described.revision, R"({"values":[{"text":"x","operand":"x","count":4}],"distinct":1,)"
                                                R"("blanks":0,"invalid":3,"partial":true})"));
    // The recording backend writes a line break as `\n`.
    CHECK(h.text("table_filter_note") == R"(The list may grow while data loads\n3 cells could not be read)");
    CHECK(mounted(h, "Checkbox", "table_filter_value_0"));
    CHECK_FALSE(mounted(h, "Checkbox", "table_filter_value_1"));
}

// Mutation: "(Select all)" under a search resolved to the first page of the values it shows.

TEST_CASE("filter menu: Select all under a search covers every match, and both sides over the bound refuse",
          "[ui][table][config]") {
    std::string rows;
    for (int at = 0; at < 25'000; ++at) {
        rows += std::format(R"({}{{"id":{},"code":"c{:05}"}})", at == 0 ? "" : ",", at, at);
    }
    std::string const text = R"({"vocab":"ui/1","id":"codes","state":{"items":{"init":{"lit":[)" + rows +
                             R"(]}}},"view":{"kind":"table","id":"t","rows":{"ref":"items"},)"
                             R"("columns":[{"id":"code","label":"Code"}]}})";
    TableHarness h;
    auto const screen = h.mount(text);
    int const table = h.backend.all("Table").front();
    auto const search = [&h](std::string_view typed) {
        typeInto(h, "table_filter_search", typed);
        h.advance(250ms);
    };
    auto const members = [](table::FilterEntry const& entry) {
        return entry.oneOf.value_or(std::vector<std::string>{});
    };

    clickFilter(h, table, 0);
    selectAll(h, false);
    search("c0");
    selectAll(h, true);
    pressButton(h, "table_filter_apply");
    table::ColumnFilter filter = columnFilterOf(*screen, "code");
    REQUIRE(filter.include.size() == 1);
    CHECK(filter.exclude.empty());
    std::vector<std::string> named = members(filter.include.front());
    REQUIRE(named.size() == 10'000);
    CHECK(named.front() == "c00000");
    CHECK(named.back() == "c09999");

    clickFilter(h, table, 0);
    selectAll(h, false);
    search("c1");
    selectAll(h, true);
    search("c2");
    selectAll(h, true);
    pressButton(h, "table_filter_apply");
    filter = columnFilterOf(*screen, "code");
    CHECK(filter.include.empty());
    REQUIRE(filter.exclude.size() == 1);
    named = members(filter.exclude.front());
    REQUIRE(named.size() == 10'000);
    CHECK(named.front() == "c00000");
    CHECK(named.back() == "c09999");

    std::string const before = elementValue(*screen, "t", "filters");
    clickFilter(h, table, 0);
    selectAll(h, false);
    for (std::string_view const typed : {"c1", "c00", "c01"}) {
        search(typed);
        selectAll(h, true);
    }
    pressButton(h, "table_filter_apply");
    CHECK(h.text("table_filter_note") == "Too many values to list; filter with a condition");
    CHECK(elementValue(*screen, "t", "filters") == before);
}

// Mutation: conditions offered by Text for every kind; an operand sent as typed.

TEST_CASE("filter menu: conditions follow the column's kind, and typed operands are canonical",
          "[ui][table][config]") {
    TableHarness h;
    auto const screen =
        mountDescribed(h, kindsDescription(),
                       R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
                       R"("view":{"kind":"localeScope","locale":"de","children":[)"
                       R"({"kind":"table","id":"t","model":"sample","table":"samples"}]}})");
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    CHECK(propOf(h, "Select", "table_filter_op_1", "options") == optionsOf({"=", "≠", "<", "≤", ">", "≥", "between"}));
    dismissMenu(h);
    clickFilter(h, table, 2);
    CHECK_FALSE(mounted(h, "Select", "table_filter_op_1"));
    dismissMenu(h);
    clickFilter(h, table, 3);
    CHECK(propOf(h, "Select", "table_filter_op_1", "options") == optionsOf({"is empty", "is not empty"}));
    dismissMenu(h);
    clickFilter(h, table, 4);
    CHECK(propOf(h, "DateTimeInput", "table_filter_operand_1", "mode") == "Date");
    dismissMenu(h);

    clickFilter(h, table, 5);
    chooseOption(h, "table_filter_op_1", ">");
    typeInto(h, "table_filter_operand_1", "1.234,5");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.dec.include"})") == R"([{"gt":"1234.5"}])");
    dismissMenu(h);

    clickFilter(h, table, 6);
    CHECK(propOf(h, "Select", "table_filter_unit_1", "options") == optionsOf({"g", "kg"}));
    chooseOption(h, "table_filter_op_1", ">");
    typeInto(h, "table_filter_operand_1", "0,3 kg");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.weight.include"})") == R"([{"gt":"0.3 kg"}])");
    typeInto(h, "table_filter_operand_1", "250");
    chooseOption(h, "table_filter_unit_1", "g");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.weight.include"})") == R"([{"gt":"250 g"}])");
    typeInto(h, "table_filter_operand_1", "12,5 xx");
    h.advance(250ms);
    CHECK(propOf(h, "TextInput", "table_filter_operand_1", "errors") != "[]");
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.weight.include"})") == R"([{"gt":"250 g"}])");
}

// Mutation: the Measure list drawn without dimension headings.

TEST_CASE("filter menu: a Measure column's values are grouped under a heading per dimension", "[ui][table][config]") {
    TableHarness h;
    auto const screen = mountDescribed(h, kindsDescription());
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 6);
    h.reply(lastDistinct(h),
            distinctReply(
                h.described.revision,
                R"({"values":[{"text":"0.3 kg","operand":"0.3 kg","count":2,"dimension":"mass","unit":"kg"},)"
                R"({"text":"0.5 %","operand":"0.5 %","count":1,"dimension":"ratio","unit":"%"}],"distinct":2})"));
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "label").ends_with("(2)"));  // 300 g counted with 0.3 kg
    std::string const dump = h.backend.dump();
    std::size_t const mass = dump.find("testId=table_filter_heading_mass");
    std::size_t const first = dump.find("testId=table_filter_value_0");
    std::size_t const ratio = dump.find("testId=table_filter_heading_ratio");
    std::size_t const second = dump.find("testId=table_filter_value_1");
    REQUIRE(mass != std::string::npos);
    REQUIRE(ratio != std::string::npos);
    CHECK(mass < first);
    CHECK(first < ratio);
    CHECK(ratio < second);
}

// Mutation: the group menu sending `together` as one oneOf.

TEST_CASE("filter menu: a group's values are pooled and the match line builds the quantifier", "[ui][table][config]") {
    constexpr std::string_view kCompanies = R"({
      "vocab": "ui/1", "id": "companies",
      "state": { "items": { "init": { "lit": [
        { "id": 1, "companyA": "F", "companyB": "S", "companyC": "X" },
        { "id": 2, "companyA": "F", "companyB": "F", "companyC": "X" },
        { "id": 3, "companyA": "S", "companyB": "F", "companyC": "F" },
        { "id": 4, "companyA": "F", "companyB": "X", "companyC": "Y" },
        { "id": 5, "companyA": "X", "companyB": "Y", "companyC": "Z" } ] } } },
      "view": { "kind": "table", "id": "t", "rows": { "ref": "items" },
                "columns": [ { "id": "companyA" }, { "id": "companyB" }, { "id": "companyC" } ],
                "groups": [ { "id": "company", "label": "Company", "columns": { "lit": ["companyA", "companyB", "companyC"] } } ] } })";
    TableHarness h;
    auto const screen = h.mount(kCompanies);
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.prop(table, "columnGroups") == "[Company:0+3]");
    clickGroupFilter(h, table, 0);
    CHECK(propOf(h, "Dialog", "table_filter", "open") == "true");
    dismissMenu(h);
    moveColumn(h, table, 1, 2);
    CHECK(h.backend.prop(table, "columnGroups") == "[]");
    CHECK(std::ranges::contains(labelsOf(menuOf(h, table, 0)), "Filter Company, 3 columns…"));
    chooseMenu(h, table, 0, {"Filter Company, 3 columns…"});
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "label") == "F (4)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_1", "label") == "S (2)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_2", "label") == "X (4)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_3", "label") == "Y (2)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_4", "label") == "Z (1)");
    toggleBox(h, "table_filter_value_0");
    toggleBox(h, "table_filter_value_1");
    chooseOption(h, "table_filter_match", "all of them, each in a column");
    pressButton(h, "table_filter_apply");
    CHECK(readJson(*screen, R"({"ref":"t.filters.groups"})") ==
          R"([{"combine":"any","exclude":[],"group":"company","include":[{"eq":"F"},{"eq":"S"}],"match":"any",)"
          R"("mode":"together"}])");
    CHECK(elementValue(*screen, "t", "viewCount") == "2");
    CHECK(morph::ui::testing::rowOrder(h) == "1 3");
    chooseMenu(h, table, 0, {"Filter Company, 3 columns…"});
    chooseOption(h, "table_filter_match", "in at least N columns");
    typeInto(h, "table_filter_count", "2");
    pressButton(h, "table_filter_apply");
    CHECK(readJson(*screen, R"({"ref":"t.filters.groups"})") ==
          R"([{"combine":"any","exclude":[],"group":"company","include":[{"oneOf":["F","S"]}],)"
          R"("match":{"atLeast":2},"mode":"columns"}])");
    CHECK(elementValue(*screen, "t", "viewCount") == "3");
    CHECK(morph::ui::testing::rowOrder(h) == "1 2 3");
}
