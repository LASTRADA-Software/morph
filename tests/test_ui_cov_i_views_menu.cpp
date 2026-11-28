// SPDX-License-Identifier: Apache-2.0

// The saved view a table is arranged from: when the menu calls it modified, and what applying it again or saving it
// under no name does.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/table/filter.hpp>
#include <morph/table/state_store.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::elementValue;
using morph::ui::testing::entryOf;
using morph::ui::testing::kNine;
using morph::ui::testing::labelsOf;
using morph::ui::testing::menuOf;
using morph::ui::testing::RecordingTableStateStore;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;

table::TableStateKey viewsKey() {
    return table::TableStateKey{.application = "lims",
                                .model = "Lims_SampleModel",
                                .table = "samples",
                                .qualifier = "samples",
                                .part = "views"};
}

// The view `A`: every column in its place, sorted by c1, showing the rows whose c2 is 1.
table::SavedView viewA() {
    table::ColumnLayoutValue layout;
    layout.order = {"c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9"};
    table::FilterSpec filters;
    filters.columns["c2"] = table::ColumnFilter{
        .combine = table::Combine::Any, .include = {table::FilterEntry{.eq = std::string{"1"}}}, .exclude = {}};
    return table::SavedView{.name = "A",
                            .layout = std::move(layout),
                            .sort = {table::SortKey{.column = "c1", .dir = table::SortDirection::Ascending}},
                            .filters = std::move(filters)};
}

void seedViews(TableHarness& h, RecordingTableStateStore& store) {
    table::ViewsDocument document;
    document.views = {viewA()};
    std::optional<table::SaveResult> saved;
    store.save(viewsKey(), table::writeViews(document, table::ViewsPart::Own))
        .thenDetached([&saved](table::SaveResult const& result) { saved = result; });
    h.settle();
    REQUIRE(saved.has_value());
}

// The label the views menu gives the view A.
std::string labelOfA(TableHarness& h, int table) {
    for (std::string const& label : labelsOf(entryOf(menuOf(h, table, std::nullopt), "Views").items)) {
        if (label.starts_with("A")) {
            return label;
        }
    }
    return {};
}

}  // namespace

TEST_CASE("covi views menu: a view is called modified once the sort, the layout or the filters leave it",
          "[covi][ui][table][state]") {
    auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
    TableHarness h{store};
    seedViews(h, *store);
    auto const screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    REQUIRE(labelOfA(h, table) == "A");
    chooseMenu(h, table, std::nullopt, {"Views", "A"});
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c1","direction":"asc"}])");
    CHECK(labelOfA(h, table) == "A");

    // Applying it again changes nothing.
    chooseMenu(h, table, std::nullopt, {"Views", "A"});
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c1","direction":"asc"}])");
    CHECK(labelOfA(h, table) == "A");

    SECTION("another sort") {
        h.backend.clickHeader(table, 2);
        h.settle();
        CHECK(labelOfA(h, table) == "A (modified)");
        chooseMenu(h, table, std::nullopt, {"Views", "A (modified)"});
        CHECK(labelOfA(h, table) == "A");
    }
    SECTION("another layout") {
        chooseMenu(h, table, 3, {"Hide column"});
        CHECK(labelOfA(h, table) == "A (modified)");
        chooseMenu(h, table, std::nullopt, {"Views", "A (modified)"});
        CHECK(labelOfA(h, table) == "A");
        CHECK(elementValue(*screen, "t", "visibleColumns").contains(R"("c4")"));
    }
    SECTION("other filters") {
        chooseMenu(h, table, std::nullopt, {"Clear all filters"});
        CHECK(labelOfA(h, table) == "A (modified)");
    }
}

TEST_CASE("covi views menu: a view saved under no name is not saved", "[covi][ui][table][state]") {
    auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
    TableHarness h{store};
    auto const screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, std::nullopt, {"Views", "Save view…"});
    h.backend.edit(h.byTestId("TextInput", "table_view_name"), "   ");
    h.settle();
    h.backend.click(h.byTestId("Button", "table_view_save"));
    h.settle();
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_view_dialog"), "open") == "true");
    std::size_t saves = 0;
    for (auto const& call : store->calls) {
        if (call.op == "save" && call.part == "views") {
            ++saves;
        }
    }
    CHECK(saves == 0);
}
