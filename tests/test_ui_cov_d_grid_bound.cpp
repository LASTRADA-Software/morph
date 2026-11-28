// SPDX-License-Identifier: Apache-2.0

// A form grid whose cell selection a document binds, and whose header sorts its rows: the
// selection it holds follows the cells the user selects.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
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

constexpr std::string_view kBoundGrid = R"({"vocab":"ui/1","id":"specimens",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "state":{"sel":{"init":null}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit"},
    {"kind":"table","id":"grid","testId":"grid","form":"rec","field":"specimens","selectionMode":"cells",
     "layout":{"lit":null},
     "selection":{"ref":"sel"},"onSelectionChange":[{"set":"sel","value":{"ref":"event"}}],
     "columns":[{"id":"mass","field":"mass","sortable":true},{"id":"note","field":"note","sortable":true}]},
    {"kind":"text","testId":"focus","text":{"fn":"coalesce","args":[{"ref":"sel.focus.column"},"-"]}}]}})";

struct Mounted {
    ui::testing::TableHarness h;
    std::unique_ptr<ui::Screen> screen;
    int table = 0;

    Mounted() {
        ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                               ui::ActionInfo{.modelType = "Lims_SampleModel"});
        screen = h.mount(kBoundGrid);
        forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
        form.prefill(R"({"sampleId":4,"specimens":[{"note":"b"},{"note":"c"},{"note":"a"}]})");
        h.settle();
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): the table exists only after the screen is mounted and settled.
        table = h.byTestId("Table", "grid");
    }

    std::vector<std::string> order() const { return ui::testing::rowKeys(h, table); }
};

}  // namespace

TEST_CASE("covd bound grid: a header click sorts the rows ascending, and again descending, and again ascending",
          "[ui][grid][interpreter][covd]") {
    Mounted m;
    using Order = std::vector<std::string>;
    CHECK(m.order() == Order{"1", "2", "3"});
    m.h.backend.clickHeader(m.table, 1);
    m.h.settle();
    CHECK(m.order() == Order{"3", "1", "2"});
    m.h.backend.clickHeader(m.table, 1);
    m.h.settle();
    CHECK(m.order() == Order{"2", "1", "3"});
    m.h.backend.clickHeader(m.table, 1);
    m.h.settle();
    CHECK(m.order() == Order{"3", "1", "2"});
}

TEST_CASE("covd bound grid: the selection the document holds follows the cells the user selects",
          "[ui][grid][interpreter][covd]") {
    Mounted m;
    m.h.backend.selectCells(m.table, 0, 0, 1, 1);
    m.h.settle();
    CHECK(m.h.text("focus") == "note");
    CHECK(ui::testing::elementValue(m.h, "grid", "selection") ==
          ui::testing::value(R"({"anchor":{"key":1,"column":"mass"},"focus":{"key":2,"column":"note"}})"));
    m.h.backend.selectCells(m.table, 2, 1, 2, 1);
    m.h.settle();
    CHECK(m.h.text("focus") == "note");
    CHECK(ui::testing::elementValue(m.h, "grid", "selection") ==
          ui::testing::value(R"({"anchor":{"key":3,"column":"note"},"focus":{"key":3,"column":"note"}})"));
}

TEST_CASE("covd bound grid: the table menu offers the focused row's entries, which insert and delete rows",
          "[ui][grid][interpreter][covd]") {
    Mounted m;
    auto const labels = [&m] {
        std::vector<std::string> out;
        for (ui::MenuEntry const& entry : m.h.backend.menuOf(m.table, std::nullopt)) {
            out.push_back(entry.separator ? std::string{"--"} : entry.label);
        }
        return out;
    };
    // With no focus there is no row to offer entries for.
    std::vector<std::string> const before = labels();
    CHECK(std::ranges::find(before, "Insert row below") == before.end());
    CHECK(std::ranges::count(before, "--") == 1);

    m.h.backend.clickCell(m.table, 0, 1);
    m.h.settle();
    std::vector<std::string> const focused = labels();
    CHECK(std::ranges::count(focused, "--") == 2);
    CHECK(std::ranges::find(focused, "Insert row below") != focused.end());
    CHECK(m.order().size() == 3);
    m.h.backend.chooseMenu(m.table, std::nullopt, {"Insert row below"});
    m.h.settle();
    CHECK(m.order().size() == 4);
    m.h.backend.chooseMenu(m.table, std::nullopt, {"Insert row above"});
    m.h.settle();
    CHECK(m.order().size() == 5);
    m.h.backend.chooseMenu(m.table, std::nullopt, {"Delete row"});
    m.h.settle();
    CHECK(m.order().size() == 4);
}

namespace {

// The sort is bound from the start but set only once the form holds its rows: rows a form takes in while the
// grid is ordered stay where they arrive until the next sort.
constexpr std::string_view kSortedGrid = R"({"vocab":"ui/1","id":"specimens",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "state":{"srt":{"init":null}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit"},
    {"kind":"table","id":"grid","testId":"grid","form":"rec","field":"specimens","selectionMode":"cells",
     "layout":{"lit":null},"sort":{"ref":"srt"},
     "columns":[{"id":"mass","field":"mass","sortable":true},{"id":"note","field":"note","sortable":true}]},
    {"kind":"button","testId":"desc","label":"desc",
     "onClick":[{"set":"srt","value":{"lit":[{"column":"note","direction":"desc"}]}}]},
    {"kind":"button","testId":"asc","label":"asc",
     "onClick":[{"set":"srt","value":{"lit":[{"column":"note","direction":"asc"}]}}]}]}})";

}  // namespace

TEST_CASE("covd bound grid: a sort the document binds orders a form grid's rows and follows the state",
          "[ui][grid][interpreter][covd]") {
    using Order = std::vector<std::string>;
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    std::unique_ptr<ui::Screen> const screen = h.mount(kSortedGrid);
    ui::testing::elementOf(*screen, "rec")
        .form->prefill(R"({"sampleId":4,"specimens":[{"note":"b"},{"note":"c"},{"note":"a"}]})");
    h.settle();
    int const table = h.byTestId("Table", "grid");
    CHECK(ui::testing::rowKeys(h, table) == Order{"1", "2", "3"});
    h.backend.click(h.byTestId("Button", "desc"));
    h.settle();
    CHECK(ui::testing::rowKeys(h, table) == Order{"2", "1", "3"});
    h.backend.click(h.byTestId("Button", "asc"));
    h.settle();
    CHECK(ui::testing::rowKeys(h, table) == Order{"3", "1", "2"});
}
