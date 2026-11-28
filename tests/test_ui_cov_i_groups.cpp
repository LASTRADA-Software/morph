// SPDX-License-Identifier: Apache-2.0

// A column group the user forms from the column chooser: what the layout event carries, the filter its header button
// opens, and what removing the group does to that filter.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <string_view>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

using namespace std::chrono_literals;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::clickGroupFilter;
using morph::ui::testing::readJson;
using morph::ui::testing::TableHarness;
using table_model_support::chooseOption;
using table_model_support::press;
using table_model_support::propOf;
using table_model_support::toggleBox;
using table_model_support::typeInto;

constexpr std::string_view kGrouped = R"({
  "vocab": "ui/1", "id": "grouped",
  "state": { "items": { "init": { "lit": [ { "id": 1, "a": "x", "b": "y", "c": "z" } ] } },
             "lay": { "init": { "lit": null } } },
  "view": { "kind": "table", "id": "t", "testId": "t", "rows": { "ref": "items" }, "key": { "row": "id" },
            "columns": [ { "id": "a", "label": "A" }, { "id": "b", "label": "B" }, { "id": "c", "label": "C" } ],
            "onLayoutChange": [ { "set": "lay", "value": { "ref": "event" } } ] } })";

// Forms the group `Pair` of the columns a and b from the chooser.
void formPair(TableHarness& h, int table) {
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    press(h, "table_columns_group");
    toggleBox(h, "table_group_column_a");
    toggleBox(h, "table_group_column_b");
    typeInto(h, "table_group_label", "Pair");
    press(h, "table_group_create");
}

}  // namespace

TEST_CASE("covi groups: the layout event names the group the user formed", "[covi][ui][table][config]") {
    TableHarness h;
    auto const screen = h.mount(kGrouped);
    int const table = h.byTestId("Table", "t");
    formPair(h, table);
    press(h, "table_columns_done");
    std::string const layout = readJson(*screen, R"({"ref":"lay"})");
    CHECK(layout.contains(R"("groups":[)"));
    CHECK(layout.contains(R"("label":"Pair")"));
    CHECK(layout.contains(R"("columns":["a","b"])"));
}

TEST_CASE("covi groups: the group's header button opens its filter, and removing the group closes it",
          "[covi][ui][table][config]") {
    TableHarness h;
    auto const screen = h.mount(kGrouped);
    int const table = h.byTestId("Table", "t");
    formPair(h, table);
    press(h, "table_columns_done");
    REQUIRE(h.backend.prop(table, "columnGroups") == "[Pair:0+2]");
    clickGroupFilter(h, table, 0);
    CHECK(propOf(h, "Dialog", "table_filter", "open") == "true");
    chooseOption(h, "table_filter_op_1", "equals");
    typeInto(h, "table_filter_operand_1", "x");
    h.advance(250ms);
    REQUIRE(readJson(*screen, R"({"ref":"t.filters.groups"})") != "[]");
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    press(h, "table_group_remove_g1");
    CHECK(readJson(*screen, R"({"ref":"t.filters.groups"})") == "[]");
    CHECK(propOf(h, "Dialog", "table_filter", "open") == "false");
    CHECK(h.backend.prop(table, "columnGroups") == "[]");
}

TEST_CASE("covi groups: removing a group that has no filter leaves the filters as they were", "[covi][ui][table][config]") {
    TableHarness h;
    auto const screen = h.mount(kGrouped);
    int const table = h.byTestId("Table", "t");
    formPair(h, table);
    press(h, "table_columns_done");
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    press(h, "table_group_remove_g1");
    CHECK(readJson(*screen, R"({"ref":"t.filters.groups"})") == "[]");
    CHECK(h.backend.prop(table, "columnGroups") == "[]");
}
