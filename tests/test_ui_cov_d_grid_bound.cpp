// SPDX-License-Identifier: Apache-2.0

// A form grid whose cell selection a document binds, and whose header sorts its rows: the
// selection it holds follows the cells the user selects.

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
        table = h.byTestId("Table", "grid");
    }

    std::vector<std::string> order() { return ui::testing::rowKeys(h, table); }
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
