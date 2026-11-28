// SPDX-License-Identifier: Apache-2.0

// A form grid grouped by a value of its rows: the runs it makes of rows whose value repeats apart, and the headings
// and summaries of each run.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

constexpr std::string_view kGrouped = R"({"vocab":"ui/1","id":"specimens",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit"},
    {"kind":"table","id":"grid","testId":"grid","form":"rec","field":"specimens","selectionMode":"cells",
     "columns":[{"id":"label","field":"label"},{"id":"note","field":"note","sortable":true}],
     "summaries":[{"key":"all","cells":{"label":{"fn":"count","args":[{"ref":"rows"}]}}}],
     "group":{"by":BY,"heading":[{"kind":"text","text":HEADING}],
              "summaries":[{"key":"m","cells":{"label":{"fn":"count","args":[{"ref":"rows"}]}}}]}}]}})";

std::string grouped(std::string_view by, std::string_view heading) {
    std::string text{kGrouped};
    text.replace(text.find("BY"), 2, by);
    text.replace(text.find("HEADING"), 7, heading);
    return text;
}

struct Mounted {
    ui::testing::TableHarness h;
    std::unique_ptr<ui::Screen> screen;
    int table = 0;

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a test rig; the names say which text is which.
    Mounted(std::string_view by, std::string_view heading, std::string_view rows) {
        ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                               ui::ActionInfo{.modelType = "Lims_SampleModel"});
        screen = h.mount(grouped(by, heading));
        forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
        form.prefill(std::string{R"({"sampleId":4,"specimens":)"} + std::string{rows} + "}");
        h.settle();
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): the table exists only after the screen is mounted and settled.
        table = h.byTestId("Table", "grid");
    }

    std::string dump() const {
        std::string out;
        for (int const row : h.backend.children(table)) {
            out += h.backend.prop(row, "heading") == "true" ? "H" : "R";
            // A heading and a summary row are keyed by a text that starts with the unit separator.
            std::string key = h.backend.prop(row, "rowKey");
            std::erase(key, '\x1f');
            out += "[" + key + "]";
        }
        return out;
    }
};

}  // namespace

TEST_CASE("covd grid groups: rows whose value repeats apart make a run each, and a blank value is one group",
          "[ui][grid][interpreter][covd]") {
    struct Case {
        std::string_view by;
        std::string_view expected;
    };
    auto const [by, expected] = GENERATE(
        Case{R"({"ref":"row.label"})",
             R"(H["group:\"A\":0"]R[1]R["summary:0/m"]H["group:\"B\":0"]R[2]R["summary:1/m"])"
             R"(H["group:\"A\":1"]R[3]R["summary:2/m"]H["group:null:0"]R[4]R[5]R["summary:3/m"]R["summary:all"])"},
        Case{R"({"fn":"coalesce","args":[{"ref":"row.label"},"none"]})",
             R"(H["group:\"A\":0"]R[1]R["summary:0/m"]H["group:\"B\":0"]R[2]R["summary:1/m"])"
             R"(H["group:\"A\":1"]R[3]R["summary:2/m"]H["group:\"none\":0"]R[4]R[5]R["summary:3/m"])"
             R"(R["summary:all"])"},
        Case{R"({"fn":"concat","args":["same"]})",
             R"(H["group:\"same\":0"]R[1]R[2]R[3]R[4]R[5]R["summary:0/m"]R["summary:all"])"});
    CAPTURE(by);
    Mounted const m{by, R"({"ref":"row.key"})", R"([{"label":"A"},{"label":"B"},{"label":"A"},{},{"label":""}])"};
    CHECK(m.dump() == expected);
}

TEST_CASE("covd grid groups: a grouped grid sorts within its runs", "[ui][grid][interpreter][covd]") {
    Mounted m{R"({"ref":"row.label"})", R"({"ref":"row.key"})",
              R"([{"label":"A","note":"z"},{"label":"A","note":"y"},{"label":"B","note":"x"}])"};
    m.h.backend.clickHeader(m.table, 1);
    m.h.settle();
    CHECK(m.dump() ==
          R"(H["group:\"A\":0"]R[2]R[1]R["summary:0/m"]H["group:\"B\":0"]R[3]R["summary:1/m"]R["summary:all"])");
}
