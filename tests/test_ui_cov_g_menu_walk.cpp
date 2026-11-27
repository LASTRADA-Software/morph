// SPDX-License-Identifier: Apache-2.0

// Every entry of a column header's menu and of the table's menu, chosen as a user chooses it, does what its label
// says to the sort chain and the visible columns.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_support.hpp"

namespace {

using morph::ui::testing::elementValue;
using morph::ui::testing::kThree;
using morph::ui::testing::rowsDoc;
using morph::ui::testing::TableHarness;
using morph::ui::testing::tableOf;

struct Step {
    int column;  // -1 for the table's menu
    std::vector<std::string_view> labels;
};

struct Walk {
    std::string_view name;
    std::vector<Step> steps;
    std::string_view sort;
    std::string_view visible;
};

}  // namespace

TEST_CASE("covg menu walk: each entry of the header and table menus changes the table as it says",
          "[covg][ui][table][menu]") {
    auto const& w = GENERATE(values<Walk>({
        {"sort ascending", {{1, {"Sort ascending"}}}, "1:Ascending", R"(["id","name","weight"])"},
        {"sort descending", {{1, {"Sort descending"}}}, "1:Descending", R"(["id","name","weight"])"},
        {"add to sort",
         {{1, {"Add to sort"}}, {2, {"Add to sort"}}},
         "1:Ascending,2:Ascending",
         R"(["id","name","weight"])"},
        {"remove from sort",
         {{1, {"Sort ascending"}}, {1, {"Remove from sort"}}},
         "none",
         R"(["id","name","weight"])"},
        {"move left", {{1, {"Move left"}}}, "none", R"(["name","id","weight"])"},
        {"move right", {{1, {"Move right"}}}, "none", R"(["id","weight","name"])"},
        {"pin", {{1, {"Pin column"}}}, "none", R"(["name","id","weight"])"},
        {"unpin", {{1, {"Pin column"}}, {1, {"Unpin"}}}, "none", R"(["name","id","weight"])"},
        {"hide", {{1, {"Hide column"}}}, "none", R"(["id","weight"])"},
        {"reset", {{1, {"Hide column"}}, {-1, {"Reset columns"}}}, "none", R"(["id","name","weight"])"},
    }));
    CAPTURE(w.name);
    TableHarness h;
    auto const screen = h.mount(rowsDoc(kThree));
    for (Step const& step : w.steps) {
        h.backend.chooseMenu(
            tableOf(h),
            step.column < 0 ? std::nullopt : std::optional<std::size_t>{static_cast<std::size_t>(step.column)},
            step.labels);
        h.settle();
    }
    CHECK(h.backend.prop(tableOf(h), "sort") == w.sort);
    CHECK(elementValue(*screen, "list", "visibleColumns") == w.visible);
}
