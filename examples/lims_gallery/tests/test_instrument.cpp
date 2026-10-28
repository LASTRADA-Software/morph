// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.instrument";

// The overflow menu's entries, by their place in it; a separator stands before taking it out of service.
constexpr std::size_t kRecordCalibration = 0;
constexpr std::size_t kTakeOutOfService = 4;

// The overflow: the head's own menu on expanded, the page's after the large buttons on compact.
void chooseMore(GalleryScreen& screen, std::size_t entry) {
    auto menu = screen.find("Menu", "head_menu");
    if (!menu) {
        menu = screen.find("Menu", "more");
    }
    if (!menu) {
        throw std::runtime_error{"no overflow menu is mounted"};
    }
    screen.backend().chooseMenuEntry(*menu, std::vector<std::size_t>{entry});
}

}  // namespace

TEST_CASE("instrument, expanded: the head's status and actions, the overflow menu and the record's tabs",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK(screen.text("head_subtitle") == "Instruments › EQ-0142");
    CHECK(screen.prop("Badge", "head_status_0", "text") == "In service");
    CHECK(screen.prop("Badge", "head_status_1", "text") == "Qualified: IQ, OQ, PQ");
    CHECK(screen.prop("Badge", "head_status_2", "text") == "Calibration due 27-Oct-2026, in 3 days");
    CHECK(screen.shows("Button", "head_action_maintenance"));
    CHECK(screen.shows("Button", "head_action_book"));
    // The overflow is the head's own menu, after its actions, and not in the page.
    CHECK(screen.prop("Menu", "head_menu", "items") ==
          "[Record calibration…,Print equipment label,Export history (PDF),;separator,"
          "Take out of service…;description=needs reason]");
    CHECK_FALSE(screen.find("Menu", "more").has_value());
    CHECK(screen.find("Tabs", "tabs").has_value());
    CHECK(screen.shows("Panel", "details"));
    CHECK(screen.find("Column", "qualification").has_value());
    CHECK(screen.prop("Progress", "booking-elapsed", "value") == "0.29");
}

TEST_CASE("instrument, expanded: taking it out of service asks a reason and changes its status", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    chooseMore(screen, kTakeOutOfService);
    REQUIRE(screen.waitFor([&screen] { return screen.find("TextInput", "service-reason").has_value(); }));
    CHECK(screen.prop("Button", "action_takeOut", "enabled") == "false");
    screen.edit("service-reason", "pump pressure unstable");
    REQUIRE(screen.waitFor([&screen] { return screen.prop("Button", "action_takeOut", "enabled") != "false"; }));
    screen.click("action_takeOut");
    CHECK(screen.waitFor([&screen] { return screen.prop("Badge", "head_status_0", "text") == "Out of service"; }));
    CHECK_FALSE(screen.find("TextInput", "service-reason").has_value());
    CHECK(screen.backend()
              .find("Text", "text", "Taken out of service: pump pressure unstable. By A. Meyer.")
              .has_value());
    CHECK_FALSE(screen.shows("Button", "head_action_book"));
    screen.requireNoProblems();
}

TEST_CASE("instrument, expanded: recording a calibration clears the one that was due", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    chooseMore(screen, kRecordCalibration);
    REQUIRE(screen.waitFor([&screen] { return screen.find("TextInput", "log-note").has_value(); }));
    screen.edit("log-note", "Wavelength accuracy and flow rate verified");
    REQUIRE(screen.waitFor([&screen] { return screen.prop("Button", "action_log", "enabled") != "false"; }));
    screen.click("action_log");
    CHECK(screen.waitFor(
        [&screen] { return screen.prop("Badge", "head_status_2", "text") == "Calibrated 24-Oct-2026"; }));
    CHECK(screen.backend()
              .find("Text", "text", "Calibration recorded: Wavelength accuracy and flow rate verified. By A. Meyer.")
              .has_value());
    screen.requireNoProblems();
}

TEST_CASE("instrument, compact: stacked sections, actions in the page, the sections folded", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK(screen.text("head_subtitle") == "EQ-0142");
    CHECK(screen.prop("Badge", "head_status_2", "text") == "Calibration due in 3 days");
    CHECK_FALSE(screen.shows("Button", "head_action_book"));
    CHECK(screen.shows("Button", "book"));
    CHECK(screen.shows("Button", "maintenance"));
    // Compact hides the head's menu entries, so the head draws no menu and the page's follows the large buttons.
    CHECK_FALSE(screen.find("Menu", "head_menu").has_value());
    CHECK(screen.find("Menu", "more").has_value());
    CHECK(screen.prop("KeyValue", "details", "captions") == "Above");
    CHECK(screen.prop("KeyValue", "details", "items").value_or(std::string{}).contains(
        "Serial number:[DEAB-902114;mono]"));
    CHECK(screen.prop("Collapsible", "booking-section", "open") == "true");
    CHECK(screen.prop("Collapsible", "qualification-section", "open") == "false");
    CHECK(screen.prop("Collapsible", "events-section", "title") == "Recent events · 3");

    screen.click("maintenance");
    REQUIRE(screen.waitFor([&screen] { return screen.find("TextInput", "log-note").has_value(); }));
    screen.edit("log-note", "Seals replaced");
    REQUIRE(screen.waitFor([&screen] { return screen.prop("Button", "action_log", "enabled") != "false"; }));
    screen.click("action_log");
    CHECK(screen.waitFor(
        [&screen] { return screen.prop("Collapsible", "events-section", "title") == "Recent events · 4"; }));
    screen.requireNoProblems();
}
