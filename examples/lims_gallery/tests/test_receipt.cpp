// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.receipt";

// The runs of the sample heading: "Sample " and the sample's ID in the mono face.
std::string heading(std::string const& sampleId) { return "[Sample ," + sampleId + ";mono]"; }

// The runs of the last rejection: the ID in the mono face, then the rest.
std::string rejection(std::string const& sampleId, std::string const& reason) {
    return "[" + sampleId + ";mono, rejected: " + reason + ".]";
}

// Whether the text with @p testId comes to show @p runs.
bool showsRuns(GalleryScreen& screen, std::string const& testId, std::string const& runs) {
    return screen.waitFor([&screen, &testId, &runs] { return screen.prop("Text", testId, "spans") == runs; });
}

// The dump's lines.
std::vector<std::string> linesOf(std::string const& dump) {
    std::vector<std::string> lines;
    std::istringstream stream{dump};
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(line);
    }
    return lines;
}

// How far a dump line is indented.
std::size_t indentOf(std::string const& line) { return line.find_first_not_of(' '); }

}  // namespace

// Mutation: draw the session card with padding again (the table then needs `bleed`), or split the table and its
// footnote into two children of the ruled column (a second rule then stands under the last row).
TEST_CASE("receipt, expanded: the session table fills an unpadded card, sharing a child with its footnote",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    std::vector<std::string> const lines = linesOf(screen.backend().dump());
    auto const table = std::ranges::find_if(
        lines, [](std::string const& line) { return line.contains("Table#") && line.contains("testId=session"); });
    REQUIRE(table != lines.end());
    // The card has no padding, so there is nothing to bleed past.
    CHECK_FALSE(table->contains(" bleed"));
    // Sample and Outcome both stretch, so the spare width is shared between them.
    CHECK(table->contains("Sample:stretch"));
    CHECK(table->contains("Outcome:stretch"));
    // The table's parent: the nearest line above it that is indented less.
    auto parent = std::make_reverse_iterator(table);
    while (parent != lines.rend() && indentOf(*parent) >= indentOf(*table)) {
        ++parent;
    }
    REQUIRE(parent != lines.rend());
    CHECK(parent->contains("Column#"));
    CHECK_FALSE(parent->contains("divided=true"));
}

TEST_CASE("receipt, expanded: scan field, duplicate notice, the sample's card and the session", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK(screen.prop("TextInput", "scan", "icon") == "scan");
    // The session pane is a share of the row, capped at the mockup's width.
    CHECK(screen.prop("Panel", "session-panel", "layout").value_or("none").starts_with("percent(33) max=39/"));
    // The form's grids drop columns with the room the card is given: a field and a check need 25 and 32 units.
    CHECK(screen.prop("Grid", "fields", "minColumnWidth") == "25");
    CHECK(screen.prop("Grid", "checks", "minColumnWidth") == "32");
    CHECK(screen.prop("TextInput", "scan", "mono") == "true");
    // The notice's words as runs: the label strong and the ID in the mono face.
    CHECK(screen.prop("Banner", "duplicate", "spans") ==
          "[Already received:;strong, ,S-26-104217;mono, was received at 09:07 by J. Ruiz. Nothing was changed.]");
    CHECK(screen.prop("Text", "sample-id", "spans") == heading("S-26-104230"));
    CHECK(screen.shows("Panel", "sample"));
    CHECK(screen.prop("TextInput", "type", "readonly") == "true");
    CHECK(screen.find("DateTimeInput", "receivedAt").has_value());
    CHECK(screen.find("Column", "conditions").has_value());
    CHECK(screen.find("Checkbox", "logger").has_value());
    CHECK(screen.shows("Table", "session"));
    // The title is ruled off from the table, and the table and footnote share one child, so no rule stands twice.
    CHECK(screen.prop("Column", "session-rules", "divided") == "true");
    CHECK(screen.prop("Text", "session-title", "spans") == "[This session;strong]");
    CHECK(screen.prop("Panel", "session-panel", "a11yName") == "This session");
    CHECK(screen.prop("Text", "last-rejection", "spans") == rejection("S-26-104228", "seal broken"));
    CHECK(screen.text("head_subtitle") == "Session since 08:30");
    CHECK(screen.prop("Badge", "head_status_0", "text") == "12 received");
    CHECK(screen.prop("Badge", "head_status_1", "text") == "1 rejected");
}

TEST_CASE("receipt, expanded: receiving moves to the next sample and counts it", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    REQUIRE(screen.prop("Button", "receive", "enabled") != "false");
    screen.click("receive");
    CHECK(showsRuns(screen, "sample-id", heading("S-26-104231")));
    CHECK(screen.waitFor([&screen] { return screen.prop("Badge", "head_status_0", "text") == "13 received"; }));
    CHECK_FALSE(screen.shows("Banner", "duplicate"));
    screen.requireNoProblems();
}

TEST_CASE("receipt, expanded: a scanned duplicate is reported, an unknown id refused", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.submit("scan", "S-26-104229");
    CHECK(screen.waitFor([&screen] {
        return screen.prop("Banner", "duplicate", "spans") ==
               "[Already received:;strong, ,S-26-104229;mono, was received at 09:39 by J. Ruiz. Nothing was "
               "changed.]";
    }));
    screen.submit("scan", "S-26-999999");
    CHECK(screen.waitFor([&screen] { return screen.shows("Banner", "unknown"); }));
    CHECK_FALSE(screen.shows("Banner", "duplicate"));
    screen.submit("scan", "S-26-104232");
    CHECK(showsRuns(screen, "sample-id", heading("S-26-104232")));
    screen.requireNoProblems();
}

TEST_CASE("receipt, expanded: a temperature out of range blocks receiving", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    CHECK(screen.prop("TextInput", "temperature", "description") == "Required range 2–8 °C");
    CHECK(screen.prop("TextInput", "temperature", "captionHint").value_or("").empty());
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
    CHECK(showsRuns(screen, "last-rejection", rejection("S-26-104230", "container damaged")));
    CHECK(screen.waitFor([&screen] { return !screen.find("Select", "reject-reason").has_value(); }));
    CHECK(screen.waitFor([&screen] { return screen.prop("Badge", "head_status_1", "text") == "2 rejected"; }));
    CHECK(screen.prop("Text", "sample-id", "spans") == heading("S-26-104231"));
    screen.requireNoProblems();
}

TEST_CASE("receipt, compact: one column, every field kept, the session folded away", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK(screen.find("TextInput", "scan").has_value());
    CHECK(screen.prop("Banner", "duplicate", "spans") ==
          "[Already received:;strong, ,S-26-104217;mono,\\, 09:07\\, J. Ruiz.]");
    CHECK(screen.find("TextInput", "temperature").has_value());
    // The range follows the caption and its mark on the phone, which has no room for a line under the field.
    CHECK(screen.prop("TextInput", "temperature", "captionHint") == "2–8 °C");
    CHECK(screen.backend().dump().contains(" · Lot ,L-26-0381;mono]"));
    CHECK(screen.find("Column", "conditions").has_value());
    CHECK(screen.prop("Collapsible", "session-panel", "title") == "This session · 13 scans");
    CHECK(screen.prop("Badge", "head_status_0", "text") == "12");
    CHECK(screen.prop("Collapsible", "session-panel", "open") == "false");
    // Reject and Receive sit 8 px apart.
    CHECK(screen.isActionRow("Row", "footer-actions"));
    screen.click("receive");
    CHECK(screen.waitFor(
        [&screen] { return screen.prop("Collapsible", "session-panel", "title") == "This session · 14 scans"; }));
    screen.requireNoProblems();
}
