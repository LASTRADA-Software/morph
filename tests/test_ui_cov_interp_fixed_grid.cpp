// SPDX-License-Identifier: Apache-2.0

// A document's fixed grid: the selection it binds and reports, what it copies, and the counts of its edits.

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "grid_test_support.hpp"
#include "ui_table_support.hpp"

namespace {

using namespace morph;

constexpr std::string_view kSelectableFixed = R"({"vocab":"ui/1","id":"limits",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "state":{"picked":{"init":{"lit":{"anchor":{"key":"density","column":"min"},"focus":{"key":"count","column":"max"}}}}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_SetLimits","submit":"explicit"},
    {"kind":"table","id":"grid","testId":"grid","form":"rec","selectionMode":"cells",
     "selection":{"ref":"picked"},
     "onSelectionChange":[{"set":"picked","value":{"ref":"event"}}],
     "columns":[{"id":"min","label":"Min"},{"id":"max","label":"Max"}],
     "layoutRows":[
       {"key":"density","label":"Density",
        "cells":{"min":{"field":"limits.density.min"},"max":{"field":"limits.density.max"}}},
       {"key":"count","label":"Count","cells":{"min":{"field":"limits.count"},"max":{"text":"none"}}},
       {"forEach":"specimens","rows":[{"key":"mass","label":"Mass","cells":{"min":{"field":"mass"}}}]}]},
    {"kind":"text","testId":"picked","text":{"fn":"coalesce","args":[{"ref":"picked.focus.column"},"-"]}}]}})";

}  // namespace

TEST_CASE("cov fixed grid: a bound selection is shown, and a new selection is reported", "[ui][grid][fixed][cov]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_SetLimits", std::string{morph::test::grid::kSetLimits},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    auto const screen = h.mount(kSelectableFixed);
    forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
    form.prefill(R"({"limits":{"count":4},"specimens":[{},{}]})");
    h.settle();
    int const table = h.byTestId("Table", "grid");
    CHECK(ui::testing::elementValue(h, "grid", "selection") ==
          ui::testing::value(R"({"anchor":{"key":"density","column":"min"},"focus":{"key":"count","column":"max"}})"));
    h.backend.selectCells(table, 0, 0, 0, 1);
    h.settle();
    CHECK(
        ui::testing::elementValue(h, "grid", "selection") ==
        ui::testing::value(R"({"anchor":{"key":"density","column":"min"},"focus":{"key":"density","column":"max"}})"));
    CHECK(h.text("picked") == "max");
}

TEST_CASE("cov fixed grid: copying selected cells copies them under their column labels", "[ui][grid][fixed][cov]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_SetLimits", std::string{morph::test::grid::kSetLimits},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    auto const screen = h.mount(kSelectableFixed);
    forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
    form.prefill(R"({"limits":{"count":4},"specimens":[{},{}]})");
    h.settle();
    int const table = h.byTestId("Table", "grid");
    h.backend.selectCells(table, 1, 0, 1, 1);  // the count row, both columns
    h.settle();
    h.backend.press(table, "Ctrl+C");
    h.settle();
    CHECK(h.backend.clipboardText() == "4\tnone");
}

TEST_CASE("cov fixed grid: the counts of edited cells and rows", "[ui][grid][fixed][cov]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_SetLimits", std::string{morph::test::grid::kSetLimits},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    auto const screen = h.mount(kSelectableFixed);
    forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
    form.prefill(R"({"limits":{},"specimens":[{},{}]})");
    h.settle();
    CHECK(ui::testing::elementValue(h, "grid", "modifiedRows") == ui::Value{std::int64_t{0}});
    int const table = h.byTestId("Table", "grid");
    h.backend.clickCell(table, 0, 1);
    h.settle();
    h.backend.typeInto(table, "4");
    h.settle();
    h.backend.press(table, "Enter");
    h.settle();
    CHECK(ui::testing::elementValue(h, "grid", "modifiedCount") == ui::Value{std::int64_t{1}});
    CHECK(ui::testing::elementValue(h, "grid", "modifiedRows") == ui::Value{std::int64_t{1}});
}

namespace {

std::string configurableDocument(std::string_view configurable) {
    return std::string{R"({"vocab":"ui/1","id":"c",
  "state":{"items":{"init":{"lit":[{"id":1,"a":"x","b":"y","c":"z"}]}}},
  "view":{"kind":"table","id":"t","testId":"t","rows":{"ref":"items"},"key":{"row":"id"},"configurable":)"} +
           std::string{configurable} +
           R"(,"columns":[{"id":"a","label":"A"},{"id":"b","label":"B"},{"id":"c","label":"C"}]}})";
}

// The configuring entries the header menu of the middle column offers under a `configurable` value.
std::vector<std::string> configuringEntries(std::string_view configurable) {
    ui::testing::TableHarness h;
    auto const screen = h.mount(configurableDocument(configurable));
    int const table = h.byTestId("Table", "t");
    std::vector<std::string> kept;
    for (std::string const& label : ui::testing::labelsOf(ui::testing::menuOf(h, table, 1))) {
        if (label == "Move left" || label == "Move right" || label == "Pin column" || label == "Hide column") {
            kept.push_back(label);
        }
    }
    return kept;
}

}  // namespace

TEST_CASE("cov configurable: a list names the changes to a table's columns the user may make",
          "[ui][table][config][cov]") {
    using Names = std::vector<std::string>;
    CHECK(configuringEntries(R"({"lit":["reorder"]})") == Names{"Move left", "Move right"});
    CHECK(configuringEntries(R"({"lit":["pin"]})") == Names{"Pin column"});
    CHECK(configuringEntries(R"({"lit":["hide"]})") == Names{"Hide column"});
    CHECK(configuringEntries(R"({"lit":["resize"]})").empty());
    CHECK(configuringEntries(R"({"lit":["hide","pin"]})") == Names{"Pin column", "Hide column"});
    CHECK(configuringEntries("true") == Names{"Move left", "Move right", "Pin column", "Hide column"});
    CHECK(configuringEntries("false").empty());
}
