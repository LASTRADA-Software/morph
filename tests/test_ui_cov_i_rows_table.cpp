// SPDX-License-Identifier: Apache-2.0

// A table over `rows` and the properties that shape its arrangement: where its state is kept, how long its sort may
// be, its column groups, a bound layout, and what a copy and a cell's menu do at their edges.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/table/state_store.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
using namespace std::chrono_literals;
using morph::ui::Key;
using morph::ui::MenuEntry;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::clipboardText;
using morph::ui::testing::elementValue;
using morph::ui::testing::entryOf;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::kThree;
using morph::ui::testing::labelsOf;
using morph::ui::testing::pressCopy;
using morph::ui::testing::rowKeys;
using morph::ui::testing::rowsDoc;
using morph::ui::testing::statusOf;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;
using morph::ui::testing::tableOf;

}  // namespace

TEST_CASE("covi rows table: persist says whether and under what name the arrangement is kept",
          "[covi][ui][table][state]") {
    struct Case {
        char const* what;
        std::string extra;
        bool kept;
        std::string qualifier;
    };
    for (Case const& item : std::vector<Case>{
             {"by default, under the screen's id", "", true, "samples"},
             {"persist true", R"(,"persist":true)", true, "samples"},
             {"persist false", R"(,"persist":false)", false, ""},
             {"persist under a name", R"(,"persist":"mine")", true, "mine"},
         }) {
        INFO(item.what);
        auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
        TableHarness h{store};
        auto const screen = h.mount(rowsDoc(kThree, item.extra));
        store->release();
        h.settle();
        h.backend.clickHeader(tableOf(h), 1);
        h.advance(500ms);
        if (item.kept) {
            CHECK(store->loads() >= 1);
            CHECK(store->lastLoadKey().qualifier == item.qualifier);
            CHECK(store->saves() == 1);
        } else {
            CHECK(store->loads() == 0);
            CHECK(store->saves() == 0);
        }
        CHECK(elementValue(*screen, "list", "sort") == R"([{"column":"name","direction":"asc"}])");
    }
}

TEST_CASE("covi rows table: a sort chain holds as many keys as maxSortKeys says", "[covi][ui][table]") {
    TableHarness h;
    auto const screen = h.mount(rowsDoc(kThree, R"(,"maxSortKeys":1)"));
    int const table = tableOf(h);
    h.backend.clickHeader(table, 0);
    h.settle();
    REQUIRE(elementValue(*screen, "list", "sort") == R"([{"column":"id","direction":"asc"}])");
    h.backend.clickHeader(table, 1, morph::ui::HeaderClick::Shift);
    h.settle();
    CHECK(elementValue(*screen, "list", "sort") == R"([{"column":"id","direction":"asc"}])");
    CHECK_FALSE(statusOf(h).message.empty());
}

TEST_CASE("covi rows table: a group's label and members reach the header row", "[covi][ui][table]") {
    SECTION("a labelled group") {
        TableHarness h;
        auto const screen =
            h.mount(rowsDoc(kThree, R"(,"groups":[{"id":"g","label":"Both","columns":{"lit":["id","name"]}}])"));
        CHECK(h.backend.prop(tableOf(h), "columnGroups") == "[Both:0+2]");
    }
    SECTION("a group with no label is named by its id") {
        TableHarness h;
        auto const screen = h.mount(rowsDoc(kThree, R"(,"groups":[{"id":"g","columns":{"lit":["name","weight"]}}])"));
        CHECK(h.backend.prop(tableOf(h), "columnGroups") == "[g:1+2]");
    }
}

TEST_CASE("covi rows table: a bound layout is the document's alone, a null literal leaves the table its own",
          "[covi][ui][table]") {
    constexpr std::string_view kBound = R"(,"layout":{"lit":{"order":["name","id","weight"],"hidden":[],"pinned":0,)"
                                        R"("widths":{}}})";
    SECTION("bound") {
        TableHarness h;
        auto const screen = h.mount(rowsDoc(kThree, kBound));
        REQUIRE(elementValue(*screen, "list", "visibleColumns") == R"(["name","id","weight"])");
        chooseMenu(h, tableOf(h), 0, {"Hide column"});
        h.settle();
        CHECK(elementValue(*screen, "list", "visibleColumns") == R"(["name","id","weight"])");
    }
    SECTION("a null literal binds nothing") {
        TableHarness h;
        auto const screen = h.mount(rowsDoc(kThree, R"(,"layout":{"lit":null})"));
        REQUIRE(elementValue(*screen, "list", "visibleColumns") == R"(["id","name","weight"])");
        chooseMenu(h, tableOf(h), 0, {"Hide column"});
        h.settle();
        CHECK(elementValue(*screen, "list", "visibleColumns") == R"(["name","weight"])");
    }
}

TEST_CASE("covi rows table: a copy with the focus on a column that is hidden copies nothing", "[covi][ui][table]") {
    TableHarness h;
    auto const screen = h.mount(rowsDoc(kThree));
    int const table = tableOf(h);
    h.backend.focusMove(table, morph::ui::FocusMove::First);
    h.settle();
    pressCopy(h, table);
    REQUIRE_FALSE(clipboardText(h).empty());
    std::string const shown = clipboardText(h);
    chooseMenu(h, table, 0, {"Hide column"});
    h.settle();
    REQUIRE(elementValue(*screen, "list", "visibleColumns") == R"(["name","weight"])");
    pressCopy(h, table);
    CHECK(clipboardText(h) == shown);
}

TEST_CASE("covi rows table: a cell's menu names no filter for a column the table lacks, and filters by a value",
          "[covi][ui][table][menu]") {
    TableHarness h;
    auto const screen = h.mount(rowsDoc(kThree));
    int const table = tableOf(h);
    std::vector<MenuEntry> const past = h.backend.rightClickCell(table, Key{std::string{"b"}}, 99);
    CHECK(labelsOf(past) ==
          std::vector<std::string>{"Copy", "Copy with headers", "Filter by this value", "Exclude this value"});
    CHECK_FALSE(entryOf(past, "Filter by this value").enabled);
    CHECK(entryOf(h.backend.rightClickCell(table, Key{std::string{"b"}}, 1), "Filter by this value").enabled);
    h.backend.chooseCellMenu(table, Key{std::string{"b"}}, 1, {"Filter by this value"});
    h.settle();
    CHECK(rowKeys(h, table) == std::vector<std::string>{"b"});
    h.backend.chooseCellMenu(table, Key{std::string{"b"}}, 1, {"Exclude this value"});
    h.settle();
    CHECK(rowKeys(h, table).empty());
}

TEST_CASE("covi rows table: a click, a range and select all select rows by their keys", "[covi][ui][table]") {
    TableHarness h;
    auto const screen = h.mount(rowsDoc(kThree, R"(,"selectionMode":"multiple")"));
    int const table = tableOf(h);
    h.backend.clickRow(table, Key{std::string{"a"}});
    h.settle();
    CHECK(elementValue(*screen, "list", "selection") == R"(["a"])");
    h.backend.clickRow(table, Key{std::string{"c"}}, morph::ui::RowClick::Range);
    h.settle();
    CHECK(elementValue(*screen, "list", "selection") == R"(["a","b","c"])");
    h.backend.ctrlClickRow(table, Key{std::string{"b"}});
    h.settle();
    CHECK(elementValue(*screen, "list", "selection") == R"(["a","c"])");
    h.backend.clickRow(table, Key{std::string{"b"}});
    h.settle();
    CHECK(elementValue(*screen, "list", "selection") == R"(["b"])");
    h.backend.selectAll(table);
    h.settle();
    CHECK(elementValue(*screen, "list", "selection") == R"(["a","b","c"])");
}

TEST_CASE("covi rows table: a bound sort with entries that name no column or direction still marks the keys it can",
          "[covi][ui][table]") {
    TableHarness h;
    std::string doc = rowsDoc(kThree, R"(,"sort":{"ref":"s"})");
    doc.insert(doc.find(R"("state":{)") + 9,
               R"("s":{"init":{"lit":[{"column":"name"},{"column":5,"direction":"asc"},)"
               R"({"column":"id","direction":7}]}},)");
    auto const screen = h.mount(doc);
    int const table = tableOf(h);
    std::vector<std::string> const labels = labelsOf(morph::ui::testing::menuOf(h, table, 1));
    CHECK(std::ranges::contains(labels, std::string{"Remove from sort"}));
    CHECK_FALSE(morph::ui::testing::reportsOf(h).empty());
}

TEST_CASE("covi rows table: a table in a template over names its own rows and keeps no state", "[covi][ui][table]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    auto const screen = h.mount(
        R"({"vocab":"ui/1","id":"fe","view":{"kind":"forEach","rows":{"lit":[{"name":"one","rows":)"
        R"([{"id":"a","v":2},{"id":"b","v":1}]}]},"key":{"row":"name"},"children":[{"kind":"table","id":"t",)"
        R"("testId":"t","rows":{"row":"rows"},"key":{"row":"id"},"columns":[{"id":"id","label":"Id","sortable":true},)"
        R"({"id":"v","label":"V","sortable":true}]}]}})");
    int const table = h.byTestId("Table", "t");
    h.backend.clickHeader(table, 1);
    h.advance(500ms);
    CHECK(rowKeys(h, table) == std::vector<std::string>{"b", "a"});
    CHECK(store->saves() == 0);
}
