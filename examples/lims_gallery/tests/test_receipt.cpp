// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.receipt";

}  // namespace

TEST_CASE("receipt, expanded: scan field, duplicate notice, the sample's card and the session", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK(screen.prop("TextInput", "scan", "icon") == "scan");
    CHECK(screen.prop("Banner", "duplicate", "text") ==
          "Already received: S-26-104217 was received at 09:07 by J. Ruiz. Nothing was changed.");
    CHECK(screen.text("sample-id") == "Sample S-26-104230");
    CHECK(screen.shows("Panel", "sample"));
    CHECK(screen.prop("TextInput", "type", "readonly") == "true");
    CHECK(screen.find("DateTimeInput", "receivedAt").has_value());
    CHECK(screen.find("Column", "conditions").has_value());
    CHECK(screen.find("Checkbox", "logger").has_value());
    CHECK(screen.shows("Table", "session"));
    CHECK(screen.text("last-rejection") == "S-26-104228 rejected: seal broken.");
    CHECK(screen.text("head_subtitle") == "Session since 08:30");
    CHECK(screen.prop("Badge", "head_status_0", "text") == "12 received");
    CHECK(screen.prop("Badge", "head_status_1", "text") == "1 rejected");
}

TEST_CASE("receipt, expanded: receiving moves to the next sample and counts it", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    REQUIRE(screen.prop("Button", "receive", "enabled") != "false");
    screen.click("receive");
    CHECK(screen.waitForText("sample-id", "Sample S-26-104231"));
    CHECK(screen.waitFor([&screen] { return screen.prop("Badge", "head_status_0", "text") == "13 received"; }));
    CHECK_FALSE(screen.shows("Banner", "duplicate"));
    screen.requireNoProblems();
}

TEST_CASE("receipt, expanded: a scanned duplicate is reported, an unknown id refused", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.submit("scan", "S-26-104229");
    CHECK(screen.waitFor([&screen] {
        return screen.prop("Banner", "duplicate", "text") ==
               "Already received: S-26-104229 was received at 09:39 by J. Ruiz. Nothing was changed.";
    }));
    screen.submit("scan", "S-26-999999");
    CHECK(screen.waitFor([&screen] { return screen.shows("Banner", "unknown"); }));
    CHECK_FALSE(screen.shows("Banner", "duplicate"));
    screen.submit("scan", "S-26-104232");
    CHECK(screen.waitForText("sample-id", "Sample S-26-104232"));
    screen.requireNoProblems();
}

TEST_CASE("receipt, expanded: a temperature out of range blocks receiving", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.edit("temperature", "9.1");
    CHECK(screen.waitFor([&screen] { return screen.prop("Button", "receive", "enabled") == "false"; }));
    CHECK(screen.prop("TextInput", "temperature", "errors") ==
          "[Outside 2–8 °C: reject the sample or record a deviation.]");
    screen.edit("temperature", "4.0");
    CHECK(screen.waitFor([&screen] { return screen.prop("Button", "receive", "enabled") != "false"; }));
}

TEST_CASE("receipt, expanded: rejecting asks why and records it", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("reject");
    REQUIRE(screen.waitFor([&screen] { return screen.find("Select", "reject-reason").has_value(); }));
    screen.choose("reject-reason", "container damaged");
    screen.click("action_confirm");
    CHECK(screen.waitForText("last-rejection", "S-26-104230 rejected: container damaged."));
    CHECK(screen.waitFor([&screen] { return !screen.find("Select", "reject-reason").has_value(); }));
    CHECK(screen.waitFor([&screen] { return screen.prop("Badge", "head_status_1", "text") == "2 rejected"; }));
    CHECK(screen.text("sample-id") == "Sample S-26-104231");
    screen.requireNoProblems();
}

TEST_CASE("receipt, compact: one column, every field kept, the session folded away", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK(screen.find("TextInput", "scan").has_value());
    CHECK(screen.prop("Banner", "duplicate", "text") == "Already received: S-26-104217, 09:07, J. Ruiz.");
    CHECK(screen.find("TextInput", "temperature").has_value());
    CHECK(screen.find("Column", "conditions").has_value());
    CHECK(screen.prop("Collapsible", "session-panel", "title") == "This session · 13 scans");
    CHECK(screen.prop("Badge", "head_status_0", "text") == "12");
    CHECK(screen.prop("Collapsible", "session-panel", "open") == "false");
    screen.click("receive");
    CHECK(screen.waitFor(
        [&screen] { return screen.prop("Collapsible", "session-panel", "title") == "This session · 14 scans"; }));
    screen.requireNoProblems();
}
