// SPDX-License-Identifier: Apache-2.0

// A document's fixed grid as the document spells it: the configuring it allows, the arrangement and selection it
// binds, the spans and bands it gives, and the copy of its focused row.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_support.hpp"

namespace {

using namespace morph;

// A grid of values over three columns, `configurable` as given, in a document that binds nothing else.
std::string textGrid(std::string_view keys) {
    return std::string{R"({"vocab":"ui/1","id":"sheet","state":{"n":{"init":3}},
  "view":{"kind":"table","id":"grid","testId":"grid",)"} +
           std::string{keys} + R"(,
   "columns":[{"id":"a","label":"A"},{"id":"b","label":"B"},{"id":"c","label":"C"}],
   "layoutRows":[{"key":"first","label":"First","cells":{"a":{"text":"one"},"b":{"text":"two"},"c":{"value":{"ref":"n"}}}},
                 {"key":"second","cells":{"a":{"text":"three"},"b":{"value":{"fn":"coalesce","args":[{"ref":"n"},1]}}}}]}})";
}

std::vector<std::string> configuringEntries(std::string_view configurable) {
    ui::testing::TableHarness h;
    auto const screen = h.mount(textGrid(R"("configurable":)" + std::string{configurable}));
    int const table = h.byTestId("Table", "grid");
    std::vector<std::string> kept;
    for (std::string const& label : ui::testing::labelsOf(ui::testing::menuOf(h, table, 1))) {
        if (label == "Move left" || label == "Move right" || label == "Pin column" || label == "Hide column") {
            kept.push_back(label);
        }
    }
    return kept;
}

}  // namespace

TEST_CASE("covd fixed grid: a configurable list allows the changes it names and ignores the rest",
          "[ui][grid][fixed][covd]") {
    using Entries = std::vector<std::string>;
    CHECK(configuringEntries("true") == Entries{"Move left", "Move right", "Pin column", "Hide column"});
    CHECK(configuringEntries(R"({"lit":["hide"]})") == Entries{"Hide column"});
    CHECK(configuringEntries(R"({"lit":["pin"]})") == Entries{"Pin column"});
    CHECK(configuringEntries(R"({"lit":["reorder"]})") == Entries{"Move left", "Move right"});
    CHECK(configuringEntries(R"({"lit":["resize"]})").empty());
    CHECK(configuringEntries(R"({"lit":["hide","pin"]})") == Entries{"Pin column", "Hide column"});
    CHECK(configuringEntries("false").empty());
}

TEST_CASE("covd fixed grid: a bound layout orders and hides the columns, and a null layout binds nothing",
          "[ui][grid][fixed][covd]") {
    struct Case {
        std::string_view layout;
        std::string_view visible;
    };
    auto const [layout, visible] = GENERATE(
        Case{R"("layout":{"lit":null})", R"(["a","b","c"])"},
        Case{R"("layout":{"lit":{"order":["c","b","a"],"hidden":[],"pinned":0,"widths":{}}})", R"(["c","b","a"])"},
        Case{R"("layout":{"lit":{"order":["a","b","c"],"hidden":["b"],"pinned":0,"widths":{}}})", R"(["a","c"])"},
        Case{R"("layout":{"lit":7})", R"(["a","b","c"])"});
    CAPTURE(layout);
    ui::testing::TableHarness h;
    auto const screen = h.mount(textGrid(layout));
    CHECK(ui::testing::elementValue(h, "grid", "visibleColumns") == ui::testing::value(visible));
}

TEST_CASE("covd fixed grid: a selection that is null or no range selects nothing", "[ui][grid][fixed][covd]") {
    auto const selected = GENERATE(as<std::string_view>{}, R"("selection":{"lit":null})", R"("selection":{"lit":5})",
                                   R"("selection":{"lit":{"anchor":{"key":"nosuch","column":"a"}}})");
    CAPTURE(selected);
    ui::testing::TableHarness h;
    auto const screen = h.mount(textGrid(std::string{selected} + R"(,"selectionMode":"cells")"));
    CHECK(ui::testing::elementValue(h, "grid", "selection") == ui::Value{});
}

TEST_CASE("covd fixed grid: a copy with no cell range copies the focused row's cells under their labels",
          "[ui][grid][fixed][covd]") {
    ui::testing::TableHarness h;
    auto const screen = h.mount(textGrid(R"("selectionMode":"cells")"));
    int const table = h.byTestId("Table", "grid");
    h.backend.clickCell(table, 0, 0);
    h.settle();
    h.backend.press(table, "Ctrl+C");
    h.settle();
    CHECK(h.backend.clipboardText().contains("one"));
}

TEST_CASE("covd fixed grid: a grid's bands reach the backend over its columns", "[ui][grid][fixed][covd]") {
    ui::testing::TableHarness h;
    auto const screen = h.mount(textGrid(R"("bands":[{"label":"AB","unit":"u","columns":{"lit":["a","b"]},)"
                                         R"("bands":[{"label":"Only A","columns":{"lit":["a"]}}]}])"));
    std::string const bands = h.backend.prop(h.byTestId("Table", "grid"), "bands");
    CAPTURE(bands);
    CHECK(bands.contains("AB|u@0+2"));
    CHECK(bands.contains("Only A@0+1"));
}

TEST_CASE("covd fixed grid: a cell's menu copies its row's cell under its column's label", "[ui][grid][fixed][covd]") {
    ui::testing::TableHarness h;
    auto const screen = h.mount(textGrid(R"("selectionMode":"cells")"));
    int const table = h.byTestId("Table", "grid");
    std::vector<std::string> labels;
    for (ui::MenuEntry const& entry : h.backend.rightClickCell(table, ui::Key{std::string{"first"}}, 1)) {
        labels.push_back(entry.label);
    }
    CHECK(std::ranges::find(labels, "Copy") != labels.end());
    h.backend.chooseCellMenu(table, ui::Key{std::string{"first"}}, 1, {"Copy with headers"});
    h.settle();
    CHECK(h.backend.clipboardText() == "B\ntwo");
    // A row that is not there has no cell to open a menu on: its key is gone, so the menu has no held cell.
    h.backend.chooseCellMenu(table, ui::Key{std::string{"second"}}, 0, {"Copy"});
    h.settle();
    CHECK(h.backend.clipboardText() == "three");
}

TEST_CASE("covd fixed grid: the table menu of a grid has no row entries beyond its own", "[ui][grid][fixed][covd]") {
    ui::testing::TableHarness h;
    auto const screen = h.mount(textGrid(R"("selectionMode":"cells")"));
    int const table = h.byTestId("Table", "grid");
    h.backend.clickCell(table, 0, 0);
    h.settle();
    std::vector<std::string> labels;
    for (ui::MenuEntry const& entry : h.backend.menuOf(table, std::nullopt)) {
        labels.push_back(entry.separator ? std::string{"--"} : entry.label);
    }
    CHECK(std::ranges::count(labels, "--") == 1);
    CHECK(std::ranges::find(labels, "Insert row below") == labels.end());
}
