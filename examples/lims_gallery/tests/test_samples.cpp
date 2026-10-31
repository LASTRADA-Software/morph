// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.samples";

// The check box that selects @p sample on a card.
std::optional<int> selectBox(GalleryScreen& screen, std::string_view sample) {
    return screen.backend().find("Checkbox", "a11yName", "Select " + std::string{sample});
}

morph::ui::Key sampleKey(std::string_view sample) { return morph::ui::Key{std::string{sample}}; }

// A text given runs is recorded as its runs, so it shows the text when its runs' characters are it.
bool showsText(GalleryScreen& screen, std::string_view text) {
    if (screen.backend().find("Text", "text", text).has_value()) {
        return true;
    }
    return std::ranges::any_of(screen.backend().all("Text"), [&screen, text](int widgetId) {
        return morph::ui::testing::detail::spansCharacters(screen.backend().prop(widgetId, "spans")) == text;
    });
}

}  // namespace

TEST_CASE("samples, expanded: the table, its out-of-spec tone, and three rows selected", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK(screen.shows("Table", "samples"));
    CHECK(screen.prop("Table", "samples", "columns") ==
          "[Sample:fixed(11);sortable=true,Product · lot:stretch(6),Tests:stretch(4),Priority:fixed(6),"
          "Status:fixed(10),Due:fixed(12);sortable=true;wrap=false,Specification:fixed(12)]");
    // Sorted by due time, earliest first, 25 rows a page.
    CHECK(screen.prop("Table", "samples", "sort") == "5:Ascending");
    CHECK(screen.text("range") == "1–25 of 38");
    CHECK(showsText(screen, "Paracetamol 500 mg · L-26-0377"));
    CHECK(showsText(screen, "22 min overdue"));
    // The table draws the selection as its own check-box column, over the selected state.
    CHECK(screen.prop("Table", "samples", "selectColumn") == "true");
    CHECK(screen.prop("Table", "samples", "selectAllLabel") == "Select all on this page");
    CHECK(screen.prop("Table", "samples", "selection") == R"(["S-26-104170","S-26-104221","S-26-104176"])");
    CHECK(screen.shows("Panel", "bulk"));
    CHECK(screen.text("selected") == "3 selected on this page ·");
    // Only the count is strong, and a lot number is a mono run.
    CHECK(screen.backend().dump().contains("[3 selected;strong, on this page ·]"));
    CHECK(screen.backend().dump().contains(" · ,L-26-0377;mono]"));
    CHECK(screen.prop("Button", "select-all", "label") == "Select all 312");
    CHECK(screen.prop("Button", "head_action_register", "label") == "Register samples");
    CHECK(screen.find("Row", "pager").has_value());
}

TEST_CASE("samples, expanded: selecting rows drives the bulk bar, and changing priority applies to them",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("clear");
    CHECK(screen.waitFor([&screen] { return !screen.shows("Panel", "bulk"); }));
    int const table = screen.find("Table", "samples").value_or(0);
    screen.backend().clickRow(table, sampleKey("S-26-104198"), morph::ui::RowClick::Toggle);
    CHECK(screen.waitFor([&screen] { return screen.text("selected") == "1 selected on this page ·"; }));
    screen.click("change-priority");
    REQUIRE(screen.waitFor([&screen] { return screen.find("Select", "new-priority").has_value(); }));
    screen.choose("new-priority", "STAT");
    screen.click("action_apply");
    CHECK(screen.waitFor([&screen] { return !screen.shows("Panel", "bulk"); }));
    // Two STAT tags now: the first sample's and the one just changed.
    CHECK(screen.waitFor([&screen] { return !screen.backend().all("Badge").empty(); }));
    std::size_t stat = 0;
    for (int const badge : screen.backend().all("Badge")) {
        stat += screen.backend().prop(badge, "text") == "STAT" ? 1U : 0U;
    }
    CHECK(stat == 2);
    // The header box is Ctrl+A: every row on the page.
    CHECK(screen.backend().press(table, "Ctrl+A"));
    CHECK(screen.waitFor([&screen] { return screen.text("selected") == "25 selected on this page ·"; }));
    screen.click("select-all");
    CHECK(screen.waitFor([&screen] { return screen.text("selected") == "All 312 samples selected ·"; }));
    screen.requireNoProblems();
}

TEST_CASE("samples, expanded: the pager turns the page, and a header sorts from the first page", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    int const table = screen.find("Table", "samples").value_or(0);
    CHECK(screen.prop("Button", "previous-page", "enabled") == "false");
    // The pager's buttons are glyph-sized squares.
    CHECK(screen.prop("Button", "previous-page", "shape") == "square");
    CHECK(screen.prop("Button", "previous-page", "icon") == "nav.back");
    CHECK(screen.prop("Button", "next-page", "icon") == "nav.forward");
    CHECK(screen.prop("Button", "next-page", "shape") == "square");
    screen.click("next-page");
    CHECK(screen.waitFor([&screen] { return screen.text("range") == "26–38 of 38"; }));
    CHECK(screen.prop("Button", "next-page", "enabled") == "false");
    CHECK(screen.backend().children(table).size() == 13);
    // A click on the Due header sorts latest first and goes back to the first page.
    screen.backend().clickHeader(table, 5);
    CHECK(screen.waitFor([&screen] { return screen.text("range") == "1–25 of 38"; }));
    CHECK(screen.prop("Table", "samples", "sort") == "5:Descending");
    screen.backend().choose(screen.find("Select", "page-size").value_or(0), morph::ui::Key{std::int64_t{50}});
    CHECK(screen.waitFor([&screen] { return screen.text("range") == "1–38 of 38"; }));
    screen.requireNoProblems();
}

TEST_CASE("samples, expanded: a filter with no match shows the empty state", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    // The search and the filters are head tools on the expanded layout.
    screen.choose("head_tool_priority", "Urgent");
    CHECK(screen.waitFor([&screen] { return !showsText(screen, "Paracetamol 500 mg · L-26-0377"); }));
    CHECK(showsText(screen, "Lactose monohydrate · RM-0912"));
    screen.edit("head_tool_search", "no such sample");
    CHECK(screen.waitFor([&screen] { return screen.shows("EmptyState", "no-samples"); }));
    screen.requireNoProblems();
}

TEST_CASE("samples, compact: rows become cards with the same selection, the bar at the bottom", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK(screen.find("Column", "cards").has_value());
    CHECK_FALSE(screen.shows("Table", "samples"));
    CHECK(showsText(screen, "Paracetamol 500 mg · Assay, impurities"));
    CHECK(screen.text("selected") == "3 selected");
    CHECK(screen.find("Menu", "more-bulk").has_value());
    CHECK(screen.prop("Button", "head_action_register", "label") == "Register");
    CHECK(screen.prop("Button", "open-filters", "label") == "Filters · 0");
    // The cards follow the shared order, 25 at a time.
    CHECK(screen.prop("Select", "sort", "selected") == "\"due-asc\"");
    CHECK(screen.prop("Button", "load-more", "label") == "Load more · 25 of 38");
    auto const box = selectBox(screen, "S-26-104170");
    REQUIRE(box.has_value());
    screen.backend().toggle(box.value_or(0));
    CHECK(screen.waitFor([&screen] { return screen.text("selected") == "2 selected"; }));
    screen.click("assign");
    REQUIRE(screen.waitFor([&screen] { return screen.find("Select", "worksheet").has_value(); }));
    screen.click("action_assign");
    CHECK(screen.waitFor([&screen] { return !screen.shows("Row", "bulk"); }));
    CHECK(screen.waitFor([&screen] { return screen.backend().find("Badge", "text", "Assigned").has_value(); }));
    screen.requireNoProblems();
}

// Mutation: leave the bulk bar's `visible` off its row (the bar column keeps its strip with nothing selected), or
// draw it in a tinted panel (it is no `Row`).
TEST_CASE("samples, compact: the bulk bar is the pinned bottom bar, and takes no room while nothing is selected",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    // The search and filters are the top bar; the bulk bar is the last column on the `bar` surface.
    auto const lastBar = [&screen] {
        int found = 0;
        for (int const column : screen.backend().all("Column")) {
            if (screen.backend().prop(column, "surface") == "bar") {
                found = column;
            }
        }
        return found;
    };
    int const bar = lastBar();
    REQUIRE(bar != 0);
    CHECK(screen.shows("Row", "bulk"));
    CHECK(screen.backend().prop(bar, "visible") != "false");
    // "Clear selection" is the menu's third entry.
    screen.backend().chooseMenuEntry(screen.find("Menu", "more-bulk").value_or(0), std::vector<std::size_t>{2});
    CHECK(screen.waitFor([&screen] { return !screen.shows("Row", "bulk"); }));
    CHECK(screen.backend().prop(lastBar(), "visible") == "false");
}

// Mutation: leave `selected` off the card (no bar, and the card stays a plain client item).
TEST_CASE("samples, compact: a checked card is a selected group, and unchecking it clears the mark",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    auto const card = [&screen] { return screen.backend().find("Column", "a11yName", "S-26-104170"); };
    REQUIRE(card().has_value());
    CHECK(screen.backend().prop(card().value_or(0), "selected") == "true");
    auto const box = selectBox(screen, "S-26-104170");
    REQUIRE(box.has_value());
    screen.backend().toggle(box.value_or(0));
    CHECK(
        screen.waitFor([&screen, &card] { return screen.backend().prop(card().value_or(0), "selected") == "false"; }));
}
