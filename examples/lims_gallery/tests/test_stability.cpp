// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.stability";

// The check box named @p name.
int checkbox(GalleryScreen& screen, std::string const& name) {
    auto const found = screen.backend().find("Checkbox", "a11yName", name);
    if (!found) {
        throw std::runtime_error{"no check box " + name};
    }
    return *found;
}

// The check box of @p condition's timepoint @p label (`T0`, `1 m`, …).
int monthBox(GalleryScreen& screen, std::string_view condition, std::string_view label) {
    return checkbox(screen, std::string{condition} + " " + std::string{label});
}

}  // namespace

TEST_CASE("stability, expanded: step 2 of the wizard, the matrix and the summary", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK(
        screen.prop("Steps", "steps", "steps") ==
        "[Product and lot:Done,Conditions and timepoints:Active,Tests per timepoint:Pending,Review and sign:Pending]");
    CHECK(screen.prop("Steps", "steps", "current") == "1");
    CHECK(screen.shows("Table", "matrix"));
    CHECK(screen.backend().prop(monthBox(screen, "25 °C", "T0"), "checked") == "true");
    CHECK(screen.backend().prop(monthBox(screen, "25 °C", "1 m"), "checked") == "false");
    CHECK(screen.text("summary-pulls") == "15");
    CHECK(screen.text("summary-first") == "03-Dec-2026 (40 °C, 1 m)");
    CHECK(screen.text("summary-last") == "03-Nov-2029");
    CHECK(screen.text("summary-containers") == "180 needed, 150 in stock");
    CHECK(screen.prop("Text", "summary-containers", "role") == "Error");
    CHECK(screen.prop("TextInput", "per-pull", "errors") ==
          "[15 pulls × 12 \\= 180 units needed\\, 150 in stock for lot L-26-0381.]");
    CHECK(screen.shows("Banner", "attention"));
    CHECK(screen.prop("Button", "continue", "label") == "Continue to tests");
}

TEST_CASE("stability, expanded: a timepoint changes the summary, and enough containers lets the wizard go on",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("continue");
    CHECK(screen.prop("Steps", "steps", "current") == "1");
    screen.backend().toggle(monthBox(screen, "40 °C", "1 m"));
    CHECK(screen.waitFor([&screen] {
        return screen.text("summary-pulls") == "14" && screen.text("summary-first") == "03-Feb-2027 (25 °C, 3 m)";
    }));
    screen.edit("per-pull", "10");
    CHECK(screen.waitFor([&screen] { return !screen.shows("Banner", "attention"); }));
    CHECK(screen.prop("TextInput", "per-pull", "errors") == "[]");
    screen.click("continue");
    CHECK(screen.waitFor([&screen] { return screen.prop("Steps", "steps", "current") == "2"; }));
    CHECK_FALSE(screen.find("Table", "matrix").has_value());
    screen.click("back");
    CHECK(screen.waitFor([&screen] { return screen.find("Table", "matrix").has_value(); }));
    screen.requireNoProblems();
}

TEST_CASE("stability, compact: one condition open at a time, every field kept", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK_FALSE(screen.shows("Table", "matrix"));
    CHECK(screen.find("Column", "conditions").has_value());
    CHECK(screen.text("shell_title") == "Conditions and timepoints");
    CHECK(screen.backend().find("Collapsible", "title", "25 °C / 60 % RH · 8 pulls").has_value());
    CHECK(screen.backend().find("Collapsible", "title", "40 °C / 75 % RH · 3 pulls").has_value());
    CHECK(screen.prop("Collapsible", "summary-section", "title") == "Summary · first pull 03-Dec-2026");
    CHECK(screen.prop("TextInput", "per-pull", "errors") == "[180 units needed\\, 150 in stock for lot L-26-0381.]");
    screen.click("add-condition");
    CHECK(screen.waitFor(
        [&screen] { return screen.backend().find("Collapsible", "title", "5 °C ± 3 °C · 0 pulls").has_value(); }));
    CHECK(screen.prop("Button", "add-condition", "enabled") == "false");
    screen.requireNoProblems();
}

TEST_CASE("stability: the lot chosen in step 1 sets the stock the containers are checked against", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("back");
    CHECK(screen.waitFor([&screen] { return screen.find("Select", "lot").has_value(); }));
    CHECK(screen.prop("Select", "lot", "selected") == "\"L-26-0381\"");
    screen.choose("lot", "L-26-0377");
    screen.click("continue");
    CHECK(screen.waitFor([&screen] { return screen.text("summary-containers") == "180 needed, 420 in stock"; }));
    CHECK(screen.prop("Text", "summary-containers", "role") == "Normal");
    CHECK_FALSE(screen.shows("Banner", "attention"));
    screen.requireNoProblems();
}

TEST_CASE("stability: tests per timepoint, then review and sign", "[lims-gallery]") {
    for (std::string_view const width : lims_gallery::testing::kWidths) {
        GalleryScreen screen{kScreen, width};
        screen.edit("per-pull", "10");
        screen.click("continue");
        REQUIRE(screen.waitFor([&screen] { return screen.find("Text", "runs").has_value(); }));
        // Five tests at each of the 15 pulls; microbial limits is not run.
        CHECK(screen.text("runs") == "5 tests, 75 test runs over 15 pulls.");
        screen.backend().toggle(checkbox(screen, "Run Microbial limits"));
        CHECK(screen.waitFor([&screen] { return screen.text("runs") == "6 tests, 77 test runs over 15 pulls."; }));
        screen.click("continue");
        REQUIRE(screen.waitFor([&screen] { return screen.find("Checkbox", "confirm").has_value(); }));
        CHECK(screen.text("review-tests") == "6 tests, 77 runs");
        CHECK(screen.text("review-containers") == "150 needed, 150 in stock");
        CHECK(screen.prop("Button", "continue", "enabled") == "false");
        screen.toggle("confirm");
        CHECK(screen.waitFor([&screen] { return screen.prop("Button", "continue", "enabled") == "true"; }));
        screen.requireNoProblems();
    }
}
