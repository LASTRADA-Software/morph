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

// Whether the key-value with @p testId holds the pair @p pair, as `RecordingBackend` formats it (`Label:value`, a
// comma in either escaped).
bool holdsPair(GalleryScreen& screen, std::string_view testId, std::string_view pair) {
    return screen.prop("KeyValue", testId, "items").value_or(std::string{}).contains(pair);
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
    CHECK(holdsPair(screen, "summary-pairs", "Pulls:15"));
    CHECK(holdsPair(screen, "summary-pairs", "First pull:[03-Dec-2026 (40 °C\\, 1 m);mono]"));
    CHECK(holdsPair(screen, "summary-pairs", "Last pull:[03-Nov-2029;mono]"));
    CHECK(holdsPair(screen, "summary-pairs", "Containers:180 needed\\, 150 in stock;tone=Err"));
    CHECK(screen.prop("TextInput", "per-pull", "errors") ==
          "[15 pulls × 12 \\= 180 units needed\\, 150 in stock for lot L-26-0381.]");
    CHECK(screen.shows("Banner", "attention"));
    CHECK(screen.prop("Button", "continue", "label") == "Continue to tests");
    // The attention banner links the field's name inside its sentence.
    CHECK(screen.prop("Banner", "attention", "actionStyle") == "link");
    CHECK(screen.prop("Banner", "attention", "actionLabel") == "Containers per pull");
    CHECK(screen.prop("Banner", "attention", "spans") == "[1 field needs attention: ,Containers per pull;action,.]");
}

TEST_CASE("stability, expanded: a timepoint changes the summary, and enough containers lets the wizard go on",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("continue");
    CHECK(screen.prop("Steps", "steps", "current") == "1");
    screen.backend().toggle(monthBox(screen, "40 °C", "1 m"));
    CHECK(screen.waitFor([&screen] {
        return holdsPair(screen, "summary-pairs", "Pulls:14") &&
               holdsPair(screen, "summary-pairs", "First pull:[03-Feb-2027 (25 °C\\, 3 m);mono]");
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

// Mutation: look the pull's date up in a table from the design's T0 (the batch E1 workaround) -> the first pull reads
// 03-Dec-2026 whatever the start.
TEST_CASE("stability: the pull dates follow the study start, a month on from 31 January clamped to February",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    auto const start = screen.backend().find("DateTimeInput", "a11yName", "Study start (T0)");
    REQUIRE(start.has_value());
    auto const instant = morph::time::DateTime::fromIso8601("2027-01-31T00:00:00Z");
    REQUIRE(instant.has_value());
    screen.backend().setDateTime(start.value_or(0), morph::time::Timestamp{instant.value_or(morph::time::DateTime{})});
    CHECK(screen.waitFor([&screen] {
        return holdsPair(screen, "summary-pairs", "First pull:[28-Feb-2027 (40 °C\\, 1 m);mono]") &&
               holdsPair(screen, "summary-pairs", "Last pull:[31-Jan-2030;mono]");
    }));
    screen.requireNoProblems();
}

TEST_CASE("stability, compact: one condition open at a time, every field kept", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK_FALSE(screen.shows("Table", "matrix"));
    CHECK(screen.find("Column", "conditions").has_value());
    CHECK(screen.text("shell_title") == "Conditions and timepoints");
    // The step line is the head's eyebrow, above the title, and not a line of the pinned top bar.
    CHECK(screen.text("head_eyebrow") == "Step 2 of 4 · Paracetamol 500 mg");
    CHECK(screen.backend().find("Panel", "title", "25 °C / 60 % RH · 8 pulls").has_value());
    CHECK(screen.backend().find("Panel", "title", "40 °C / 75 % RH · 3 pulls").has_value());
    CHECK(screen.prop("Panel", "summary-section", "title") == "Summary · first pull 03-Dec-2026");
    // The field is outlined and the message spans both fields of its row.
    CHECK(screen.prop("TextInput", "per-pull", "tone") == "Err");
    CHECK(screen.text("per-pull-error") == "180 units needed, 150 in stock for lot L-26-0381.");
    // The summary's identifiers are mono runs.
    CHECK(screen.backend().dump().contains("L-26-0381;mono"));
    CHECK(screen.backend().dump().contains("STP-0114 v2;mono"));
    // The units sit inside the inputs, not in their captions.
    CHECK(screen.prop("TextInput", "per-pull", "unit") == "units");
    CHECK(screen.prop("TextInput", "window", "unit") == "days");
    CHECK(screen.prop("TextInput", "window", "prefix") == "±");
    // A section is a collapsible panel, which reports whether it is collapsed: opening the second closes the first.
    auto const first = screen.backend().find("Panel", "title", "25 °C / 60 % RH · 8 pulls");
    auto const second = screen.backend().find("Panel", "title", "40 °C / 75 % RH · 3 pulls");
    REQUIRE((first && second));
    CHECK(screen.backend().prop(*first, "collapsed") == "false");
    CHECK(screen.backend().prop(*second, "collapsed") == "true");
    screen.backend().collapse(*second, false);
    CHECK(screen.waitFor([&] {
        return screen.backend().prop(*second, "collapsed") == "false" &&
               screen.backend().prop(*first, "collapsed") == "true";
    }));
    screen.click("add-condition");
    CHECK(screen.waitFor(
        [&screen] { return screen.backend().find("Panel", "title", "5 °C ± 3 °C · 0 pulls").has_value(); }));
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
    CHECK(screen.waitFor(
        [&screen] { return holdsPair(screen, "summary-pairs", "Containers:180 needed\\, 420 in stock]"); }));
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
        CHECK(holdsPair(screen, "review-pairs", "Tests:6 tests\\, 77 runs"));
        CHECK(holdsPair(screen, "review-pairs", "Containers:150 needed\\, 150 in stock]"));
        CHECK(screen.prop("Button", "continue", "enabled") == "false");
        screen.toggle("confirm");
        CHECK(screen.waitFor([&screen] { return screen.prop("Button", "continue", "enabled") == "true"; }));
        screen.requireNoProblems();
    }
}
