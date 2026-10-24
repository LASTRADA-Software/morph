// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

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
    CHECK(screen.prop("Grid", "plate", "columns") == "13");
    CHECK(wellLabel(screen, "A1") == "STD1");
    CHECK(wellLabel(screen, "A2") == "QC");
    CHECK(wellLabel(screen, "B2") == "B");
    CHECK(wellLabel(screen, "B3") == "S1");
    CHECK(wellLabel(screen, "H4") == "S67");
    CHECK(wellLabel(screen, "D5") == "·");
    CHECK(screen.backend().prop(well(screen, "A3"), "selected") == "true");
    CHECK(screen.backend().prop(well(screen, "A9"), "a11yName") == "A9, control high");
    CHECK(screen.prop("Panel", "to-place", "title") == "To place · 14");
    CHECK(screen.text("selected") == "6 wells selected · A3–A8");
    CHECK(screen.shows("Panel", "bulk"));
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
    screen.click("auto-fill");
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

TEST_CASE("plate, compact: the wells listed by row", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK_FALSE(screen.find("Grid", "plate").has_value());
    CHECK(screen.prop("Collapsible", "row-A", "title") == "Row A · 1 standard, 2 controls, 9 empty");
    CHECK(screen.prop("Collapsible", "row-B", "title") == "Row B · 1 standard, 1 blank, 10 samples");
    CHECK(screen.prop("Collapsible", "row-D", "title") == "Row D · 1 standard, 10 samples, 1 empty");
    CHECK(screen.find("Row", "line-A1").has_value());
    CHECK(screen.text("line-A3") == "Empty");
    CHECK(screen.text("line-B3") == "Sample S-26-104234");
    screen.click("assign-A3");
    REQUIRE(screen.waitFor([&screen] { return screen.find("Select", "assign-well").has_value(); }));
    screen.click("action_assign");
    CHECK(screen.waitFor([&screen] { return screen.text("line-A3") == "Sample S-26-104301"; }));
    CHECK(screen.prop("Collapsible", "row-A", "title") == "Row A · 1 standard, 2 controls, 1 sample, 8 empty");
    CHECK(screen.text("to-place-count") == "ELISA, host cell protein · 13 samples to place");
    screen.requireNoProblems();
}
