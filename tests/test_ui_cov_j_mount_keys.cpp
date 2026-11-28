// SPDX-License-Identifier: Apache-2.0

// A grid's own chords and the document's chords on the same table: a chord the grid declares is the grid's, and one
// only the document binds runs the document's commands.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/forms/engine/form_session.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>
#include <string_view>

#include "grid_test_support.hpp"
#include "ui_table_support.hpp"

namespace {

namespace ui = morph::ui;

constexpr std::string_view kKeyedGrid = R"({"vocab":"ui/1","id":"specimens",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "state":{"hit":{"init":"none"}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit"},
    {"kind":"text","testId":"out","text":{"ref":"hit"}},
    {"kind":"table","id":"grid","testId":"grid","form":"rec","field":"specimens",
     "keys":{"Ctrl+Shift+Y":[{"set":"hit","value":{"lit":"table"}}],"Tab":[{"set":"hit","value":{"lit":"tab"}}]},
     "columns":[{"id":"mass","field":"mass"},{"id":"note","field":"note"}]}]}})";

struct KeyedScreen {
    KeyedScreen() {
        ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                               ui::ActionInfo{.modelType = "Lims_SampleModel"});
        screen = h.mount(kKeyedGrid);
        ui::testing::elementOf(*screen, "rec").form->prefill(R"({"sampleId":4,"specimens":[{},{}]})");
        h.settle();
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): the table exists only after the screen is mounted and settled.
        table = h.byTestId("Table", "grid");
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): the text exists only after the screen is mounted and settled.
        out = h.byTestId("Text", "out");
    }

    [[nodiscard]] std::string shown() const { return h.backend.prop(out, "text"); }

    ui::testing::TableHarness h;
    std::unique_ptr<ui::Screen> screen;
    int table = 0;
    int out = 0;
};

}  // namespace

TEST_CASE("covj table keys: a chord only the document binds runs its commands while focus is in the grid",
          "[covj][ui][grid][mount]") {
    KeyedScreen s;
    REQUIRE(s.shown() == "none");
    s.h.backend.clickCell(s.table, 0, 0);
    s.h.settle();
    CHECK(s.h.backend.press(s.table, "Ctrl+Shift+Y"));
    s.h.settle();
    CHECK(s.shown() == "table");
}

TEST_CASE("covj table keys: a chord the grid declares is the grid's, not the document's", "[covj][ui][grid][mount]") {
    KeyedScreen s;
    s.h.backend.clickCell(s.table, 0, 0);
    s.h.settle();
    CHECK(s.h.backend.press(s.table, "Tab"));
    s.h.settle();
    CHECK(s.shown() == "none");
}
