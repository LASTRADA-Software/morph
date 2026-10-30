// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.plate";

// The widget of @p kind with @p testId; throws when none is mounted.
int widget(GalleryScreen& screen, std::string_view kind, std::string_view testId) {
    auto const found = screen.find(kind, testId);
    if (!found) {
        throw std::runtime_error{"no " + std::string{kind} + " " + std::string{testId}};
    }
    return *found;
}

int well(GalleryScreen& screen, std::string const& name) { return widget(screen, "Button", "well-" + name); }

std::string wellLabel(GalleryScreen& screen, std::string const& name) {
    return screen.backend().prop(well(screen, name), "label");
}

}  // namespace

TEST_CASE("plate, expanded: the samples to place beside the 12-column plate, A3 to A8 selected", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    std::string tracks = "[fixed(3)";
    for (int column = 0; column < 12; ++column) {
        tracks += ",stretch(1);align=center";
    }
    CHECK(screen.prop("Grid", "plate", "tracks") == tracks + "]");
    // A well is a round, dense button in the mono face, on tracks that share the panel's width.
    for (std::string const name : {"A1", "A2", "B2", "B3", "D5"}) {
        CHECK(screen.backend().prop(well(screen, name), "shape") == "round");
        CHECK(screen.backend().prop(well(screen, name), "size") == "small");
        CHECK(screen.backend().prop(well(screen, name), "mono") == "true");
    }
    CHECK(wellLabel(screen, "A1") == "STD1");
    CHECK(wellLabel(screen, "A2") == "QC");
    CHECK(wellLabel(screen, "B2") == "B");
    CHECK(wellLabel(screen, "B3") == "S1");
    CHECK(wellLabel(screen, "H4") == "S67");
    CHECK(wellLabel(screen, "D5") == "·");
    // A sample well is a push button that shows its selection, not a list item.
    CHECK(screen.backend().prop(well(screen, "A3"), "selected") == "true");
    CHECK_FALSE(screen.backend().hasProp(well(screen, "A3"), "style"));
    CHECK(screen.backend().prop(well(screen, "A9"), "a11yName") == "A9, control high");
    CHECK(screen.prop("Panel", "to-place", "title") == "To place · 14");
    // The panel lists the first five and counts the rest.
    std::size_t listed = 0;
    for (int const row : screen.backend().all("Row")) {
        listed += screen.backend().prop(row, "testId") == "source" ? 1U : 0U;
    }
    CHECK(listed == 5);
    CHECK(screen.text("more") == "+ 9 more");
    CHECK(screen.text("selected") == "6 wells selected · A3–A8");
    // The title is runs: the plate's ID in the mono face.
    CHECK(screen.prop("Text", "shell_title", "spans") == "[Plate ,PL-0193;mono]");
    // The head's subtitle carries the worksheet's ID in the mono face.
    CHECK(screen.prop("Text", "head_subtitle", "spans") ==
          "[ELISA\\, host cell protein · worksheet ,WS-2026-0415;mono]");
    CHECK(screen.shows("Panel", "bulk"));
    // The legend's chips are round mono outline badges.
    CHECK(screen.find("Row", "legend").has_value());
    std::size_t chips = 0;
    for (int const badge : screen.backend().all("Badge")) {
        if (screen.backend().hasProp(badge, "shape")) {
            CHECK(screen.backend().prop(badge, "shape") == "round");
            CHECK(screen.backend().prop(badge, "mono") == "true");
            CHECK(screen.backend().prop(badge, "style") == "Outline");
            ++chips;
        }
    }
    CHECK(chips == 5);
}

TEST_CASE("plate, expanded: a sample dragged into a well leaves the samples to place", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    auto const source = screen.find("Row", "source");
    REQUIRE(source.has_value());
    CHECK(screen.backend().drag(source.value_or(0), well(screen, "D5")));
    CHECK(screen.waitFor([&screen] { return wellLabel(screen, "D5") == "S68"; }));
    CHECK(screen.prop("Panel", "to-place", "title") == "To place · 13");
    CHECK(screen.backend().prop(well(screen, "D5"), "a11yName") == "D5, sample S-26-104301");
    // A template well takes no drop.
    CHECK_FALSE(screen.backend().drag(widget(screen, "Row", "source"), well(screen, "A1")));
    screen.requireNoProblems();
}

TEST_CASE("plate, expanded: filling the selection, then clearing it", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("apply");
    CHECK(screen.waitFor([&screen] { return wellLabel(screen, "A8") == "S73"; }));
    CHECK(wellLabel(screen, "A3") == "S68");
    CHECK(screen.prop("Panel", "to-place", "title") == "To place · 8");
    CHECK_FALSE(screen.shows("Panel", "bulk"));
    screen.click("well-A3");
    screen.click("well-A4");
    CHECK(screen.waitFor([&screen] { return screen.text("selected") == "2 wells selected · A3–A4"; }));
    screen.click("clear-wells");
    CHECK(screen.waitFor([&screen] { return wellLabel(screen, "A3") == "·"; }));
    CHECK(screen.prop("Panel", "to-place", "title") == "To place · 10");
    // Auto-fill is a head action on the expanded layout, beside the template, a head tool.
    CHECK(screen.shows("Select", "head_tool_template"));
    screen.click("head_action_autoFill");
    // Ten samples fill the first ten empty wells in plate order: A3, A4, A10 to A12, D5 and H5 to H8.
    CHECK(screen.waitFor([&screen] { return wellLabel(screen, "H8") != "·"; }));
    CHECK(wellLabel(screen, "H9") == "·");
    CHECK(screen.prop("Panel", "to-place", "title") == "To place · 0");
    screen.requireNoProblems();
}

TEST_CASE("plate, expanded: the keyboard path through the wells", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    CHECK(screen.backend().press(well(screen, "A3"), "Right"));
    CHECK(screen.backend().focused() == well(screen, "A4"));
    CHECK(screen.backend().press(well(screen, "A8"), "Shift+Down"));
    CHECK(screen.waitFor([&screen] { return screen.text("selected") == "7 wells selected · A3–B8"; }));
    CHECK(screen.backend().focused() == well(screen, "B8"));
    CHECK(screen.backend().press(well(screen, "A11"), "Enter"));
    REQUIRE(screen.waitFor([&screen] { return screen.find("Select", "assign-well").has_value(); }));
    CHECK(screen.prop("Select", "assign-well", "selected") == "\"A11\"");
    CHECK(screen.prop("Select", "assign-sample", "selected") == "68");
    screen.click("action_assign");
    CHECK(screen.waitFor([&screen] { return wellLabel(screen, "A11") == "S68"; }));
    screen.requireNoProblems();
}

namespace {

// The texts of plate row @p row's lines on compact, in order: one per run of wells.
std::vector<std::string> runLines(GalleryScreen& screen, std::string const& row) {
    std::optional<int> list;
    for (char const* const kind : {"Column", "Stack", "ForEach"}) {
        list = list ? list : screen.backend().find(kind, "testId", "lines-" + row);
    }
    REQUIRE(list.has_value());
    std::vector<std::string> texts;
    std::vector<int> pending{list.value_or(0)};
    while (!pending.empty()) {
        int const node = pending.front();
        pending.erase(pending.begin());
        if (screen.backend().kindOf(node) == "Text" && screen.backend().prop(node, "testId") == "line") {
            texts.push_back(screen.backend().prop(node, "text"));
        }
        std::vector<int> const children = screen.backend().children(node);
        pending.insert(pending.begin(), children.begin(), children.end());
    }
    return texts;
}

}  // namespace

TEST_CASE("plate, compact: the wells listed by row, empty wells as one line a run", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK_FALSE(screen.find("Grid", "plate").has_value());
    CHECK(screen.prop("Collapsible", "row-A", "title") == "Row A · 1 standard, 2 controls, 9 empty");
    CHECK(screen.prop("Collapsible", "row-B", "title") == "Row B · 1 standard, 1 blank, 10 samples");
    CHECK(screen.prop("Collapsible", "row-D", "title") == "Row D · 1 standard, 10 samples, 1 empty");
    CHECK(runLines(screen, "A") == std::vector<std::string>{"Standard 1, 100 ng/mL", "Control, low", "Empty, 6 wells",
                                                            "Control, high", "Empty, 3 wells"});
    CHECK(runLines(screen, "B").at(2) == "Sample S-26-104234");
    // The run's one Assign places a sample in its first well, which leaves the rest a shorter run.
    // Every run line has an Assign, shown for an empty run alone; the first shown is row A's first empty run's.
    std::optional<int> assign;
    for (int const button : screen.backend().all("Button")) {
        if (!assign && screen.backend().prop(button, "testId") == "assign" &&
            screen.backend().prop(button, "visible") != "false") {
            assign = button;
        }
    }
    REQUIRE(assign.has_value());
    screen.backend().click(assign.value_or(0));
    REQUIRE(screen.waitFor([&screen] { return screen.find("Select", "assign-well").has_value(); }));
    CHECK(screen.prop("Select", "assign-well", "selected") == "\"A3\"");
    screen.click("action_assign");
    CHECK(screen.waitFor([&screen] { return runLines(screen, "A").at(2) == "Sample S-26-104301"; }));
    CHECK(runLines(screen, "A").at(3) == "Empty, 5 wells");
    CHECK(screen.prop("Collapsible", "row-A", "title") == "Row A · 1 standard, 2 controls, 1 sample, 8 empty");
    CHECK(screen.text("to-place-count") == "13 samples to place");
    screen.requireNoProblems();
}
