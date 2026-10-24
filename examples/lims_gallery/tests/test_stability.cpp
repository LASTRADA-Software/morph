// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.stability";

// The check box of @p condition's timepoint @p label (`T0`, `1 m`, …).
std::optional<int> monthBox(GalleryScreen& screen, std::string_view condition, std::string_view label) {
    return screen.backend().find("Checkbox", "a11yName", std::string{condition} + " " + std::string{label});
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
    auto const longTermT0 = monthBox(screen, "25 °C", "T0");
    REQUIRE(longTermT0.has_value());
    CHECK(screen.backend().prop(*longTermT0, "checked") == "true");
    CHECK(screen.backend().prop(*monthBox(screen, "25 °C", "1 m"), "checked") == "false");
    CHECK(screen.prop("KeyValue", "summary", "items") ==
          "[Product:Paracetamol 500 mg,Lot:L-26-0381,Protocol:STP-0114 v2,Pulls:15,First pull:03-Dec-2026 (40 °C\\, 1 "
          "m),Last pull:03-Nov-2029,Containers:180 needed\\, 150 in stock]");
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
    screen.backend().toggle(*monthBox(screen, "40 °C", "1 m"));
    CHECK(screen.waitFor([&screen] {
        return screen.prop("KeyValue", "summary", "items")->find("Pulls:14,First pull:03-Feb-2027") !=
               std::string::npos;
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
