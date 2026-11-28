// SPDX-License-Identifier: Apache-2.0

// A sort dialog over a table with a column that does not sort.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

using morph::ui::testing::chooseMenu;
using morph::ui::testing::elementValue;
using morph::ui::testing::TableHarness;
using table_model_support::press;

}  // namespace

TEST_CASE("covi sort dialog: offers only the columns that sort", "[covi][ui][table][config]") {
    TableHarness h;
    auto const screen = h.mount(
        R"({"vocab":"ui/1","id":"mixed",
  "state":{"items":{"init":{"lit":[{"id":1,"a":"x","b":"p"},{"id":2,"a":"y","b":"q"}]}}},
  "view":{"kind":"table","id":"t","testId":"t","rows":{"ref":"items"},"key":{"row":"id"},
          "columns":[{"id":"a","label":"A"},{"id":"b","label":"B","sortable":true}]}})");
    int const table = h.byTestId("Table", "t");
    chooseMenu(h, table, std::nullopt, {"Sort…"});
    press(h, "table_sort_add");
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"b","direction":"asc"}])");
}
