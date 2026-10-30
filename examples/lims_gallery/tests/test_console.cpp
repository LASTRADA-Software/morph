// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/value.hpp>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.console";

}  // namespace

// The design's sidebar is the shell's navigator: its entries carry the app document's counts, and the screen draws
// no navigation of its own.
TEST_CASE("console, expanded: navigation, alert, figures, chart, instruments and the sample pane", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK_FALSE(screen.find("Column", "sidebar").has_value());
    CHECK(screen.prop("Button", "nav_lims-gallery.console", "selected") == "true");
    CHECK(screen.prop("Badge", "nav_badge_lims-gallery.review", "text") == "4");
    CHECK(screen.prop("Badge", "nav_badge_lims-gallery.instrument", "text") == "1");
    CHECK(screen.prop("Badge", "nav_badge_lims-gallery.instrument", "tone") == "Warn");
    CHECK(screen.prop("Badge", "head_status_0", "text") == "Connected");
    CHECK(screen.shows("Row", "shell_sidebar"));
    CHECK(screen.shows("Banner", "alert"));
    CHECK(screen.text("kpi-received") == "64");
    CHECK(screen.prop("Text", "kpi-due", "role") == "Display");
    CHECK(screen.prop("Text", "kpi-due", "tone") == "Err");
    CHECK(screen.prop("Text", "kpi-received", "tone").value_or("").empty());
    CHECK(screen.find("Chart", "chart").has_value());
    CHECK(screen.prop("Chart", "chart", "series").value_or("").ends_with("|20@19,101.36;err;+3.4SD\\, rule 1-3s]]"));
    CHECK(screen.text("chart-violation") == "Run 20: 101.4 % LC, +3.4 SD, rule 1-3s");
    CHECK(screen.find("Row", "instrument-HPLC-02").has_value());
    CHECK(screen.shows("Splitter", "work-split"));
    CHECK(screen.shows("Table", "worklist"));
    CHECK(screen.text("pane-id") == "S-26-104170");
    CHECK(screen.text("pane-line") == "Paracetamol 500 mg · lot L-26-0377 · HPLC-02");
    // A chain of custody line is a toned dot, a strong step and the plain rest.
    CHECK(screen.backend().dump().contains("[● ;tone=Ok,Sampled;strong, · QA\\, packaging line 2 · 24-Oct 06:50]"));
    CHECK(screen.backend().dump().contains(";tone=Err,Held;strong"));
    CHECK(screen.backend().find("TextInput", "a11yName", "Total impurities result").has_value());
    CHECK(screen.shows("Text", "hold-reason"));
    CHECK(screen.prop("Button", "submit", "enabled") == "false");
}

TEST_CASE("console: the pane steps through the worklist, and a held sample alone blocks submitting",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    CHECK(screen.prop("Button", "previous", "enabled") == "false");
    screen.click("next");
    CHECK(screen.waitForText("pane-id", "S-26-104198"));
    CHECK_FALSE(screen.shows("Text", "hold-reason"));
    CHECK(screen.prop("Button", "submit", "enabled") == "true");
    auto const worklist = screen.find("Table", "worklist");
    REQUIRE(worklist.has_value());
    screen.backend().activateRow(worklist.value_or(0), morph::ui::Key{std::string{"S-26-104159"}});
    CHECK(screen.waitForText("pane-id", "S-26-104159"));
    CHECK(screen.prop("Button", "submit", "enabled") == "false");
    // The sample search is a head tool on the expanded layout.
    screen.submit("head_tool_find", "S-26-104176");
    CHECK(screen.waitForText("pane-id", "S-26-104176"));
    screen.requireNoProblems();
}

TEST_CASE("console: the total follows an impurity entered in the pane", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    auto const impurityB = screen.backend().find("TextInput", "a11yName", "Impurity B result");
    REQUIRE(impurityB.has_value());
    screen.backend().submit(impurityB.value_or(0), "0.15");
    CHECK(screen.waitFor([&screen] {
        auto const total = screen.backend().find("TextInput", "a11yName", "Total impurities result");
        return total && screen.backend().prop(total.value_or(0), "text") == "0.18";
    }));
    screen.click("rerun-open");
    CHECK(screen.waitFor([&screen] { return screen.prop("Dialog", "rerun", "open") == "true"; }));
    CHECK(screen.prop("Button", "action_request", "enabled") == "false");
    screen.requireNoProblems();
}

// On compact the shell's frame puts its navigation in one menu, so the screen draws none of its own.
TEST_CASE("console, compact: menu navigation, stacked sections, a sample opens result entry", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK_FALSE(screen.find("Menu", "nav-menu").has_value());
    CHECK_FALSE(screen.shows("Row", "shell_sidebar"));
    CHECK(screen.shows("Menu", "shell_nav_menu"));
    CHECK(screen.prop("Menu", "shell_nav_menu", "items").value_or("").contains("Review and approval · 4"));
    CHECK(screen.prop("Collapsible", "chart-section", "title") == "Assay control, HPLC-02 · latest run +3.4 SD");
    CHECK(screen.find("Column", "worklist").has_value());
    CHECK(screen.prop("Grid", "kpis", "tracks") == "[stretch(1),stretch(1)]");
    screen.click("open-sample");
    CHECK(screen.waitFor([&screen] { return screen.mounted().size() == 2; }));
    CHECK(screen.mounted().back() == "lims-gallery.results");
    screen.requireNoProblems();
}
