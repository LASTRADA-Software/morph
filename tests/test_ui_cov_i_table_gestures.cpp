// SPDX-License-Identifier: Apache-2.0

// Gestures on a table over `rows` that change nothing: sorting by the key already held, resetting a layout that is the
// default, a layout that names columns the table lacks, and groups that are no longer side by side.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

using morph::ui::testing::chooseMenu;
using morph::ui::testing::elementValue;
using morph::ui::testing::kThree;
using morph::ui::testing::labelsOf;
using morph::ui::testing::menuOf;
using morph::ui::testing::rowsDoc;
using morph::ui::testing::TableHarness;
using morph::ui::testing::tableOf;

}  // namespace

TEST_CASE("covi gestures: sorting by the key already held, and resetting the default layout, change nothing",
          "[covi][ui][table][menu]") {
    TableHarness h;
    auto const screen = h.mount(rowsDoc(kThree));
    int const table = tableOf(h);
    chooseMenu(h, table, 1, {"Sort ascending"});
    h.settle();
    REQUIRE(elementValue(*screen, "list", "sort") == R"([{"column":"name","direction":"asc"}])");
    chooseMenu(h, table, 1, {"Sort ascending"});
    h.settle();
    CHECK(elementValue(*screen, "list", "sort") == R"([{"column":"name","direction":"asc"}])");
    chooseMenu(h, table, std::nullopt, {"Reset columns"});
    h.settle();
    CHECK(elementValue(*screen, "list", "visibleColumns") == R"(["id","name","weight"])");
}

TEST_CASE("covi gestures: a bound layout that names columns the table lacks arranges the ones it has, in the order it gives",
          "[covi][ui][table]") {
    TableHarness h;
    auto const screen = h.mount(rowsDoc(
        kThree, R"(,"layout":{"lit":{"order":["name","zz","id","weight"],"hidden":[],"pinned":0,)"
                R"("widths":{"zz":9,"name":12}}})"));
    CHECK(elementValue(*screen, "list", "visibleColumns") == R"(["name","id","weight"])");
}

TEST_CASE("covi gestures: a group's header stands only while its columns stand side by side and shown",
          "[covi][ui][table][config]") {
    TableHarness h;
    auto const screen = h.mount(rowsDoc(
        kThree, R"(,"groups":[{"id":"g","label":"Both","columns":{"lit":["id","name"]}}])"));
    int const table = tableOf(h);
    REQUIRE(h.backend.prop(table, "columnGroups") == "[Both:0+2]");
    SECTION("moved apart") {
        chooseMenu(h, table, 1, {"Move right"});
        h.settle();
        CHECK(h.backend.prop(table, "columnGroups") == "[]");
    }
    SECTION("a member hidden") {
        chooseMenu(h, table, 0, {"Hide column"});
        h.settle();
        CHECK(h.backend.prop(table, "columnGroups") == "[]");
    }
}

TEST_CASE("covi gestures: a table that lets its user hide columns only, with a header that may be disabled, offers no "
          "resizing",
          "[covi][ui][table][menu]") {
    TableHarness h;
    auto const screen = h.mount(rowsDoc(kThree, R"(,"configurable":{"lit":["hide"]},"headerEnabled":true)"));
    std::vector<std::string> const labels = labelsOf(menuOf(h, tableOf(h), 1));
    CHECK(std::ranges::contains(labels, std::string{"Hide column"}));
    CHECK_FALSE(std::ranges::contains(labels, std::string{"Move left"}));
    CHECK_FALSE(std::ranges::contains(labels, std::string{"Pin column"}));
}
