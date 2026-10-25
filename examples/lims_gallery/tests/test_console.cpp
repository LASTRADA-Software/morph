// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/value.hpp>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.console";

}  // namespace

TEST_CASE("console, expanded: navigation, alert, figures, chart, instruments and the sample pane", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK(screen.find("Column", "sidebar").has_value());
    CHECK(screen.prop("Button", "nav-Overview", "selected") == "true");
    CHECK(screen.prop("Button", "nav-Control charts", "enabled") == "false");
    CHECK(screen.shows("Banner", "alert"));
    CHECK(screen.text("kpi-received") == "64");
    CHECK(screen.prop("Text", "kpi-due", "role") == "Error");
    CHECK(screen.find("Chart", "chart").has_value());
    CHECK(screen.prop("Chart", "chart", "series").value_or("").ends_with("|20@19,101.36;err;+3.4SD\\, rule 1-3s]]"));
    CHECK(screen.text("chart-violation") == "Run 20: 101.4 % LC, +3.4 SD, rule 1-3s");
    CHECK(screen.find("Row", "instrument-HPLC-02").has_value());
    CHECK(screen.shows("Splitter", "work-split"));
    CHECK(screen.shows("Table", "worklist"));
    CHECK(screen.text("pane-id") == "S-26-104170");
    CHECK(screen.text("pane-line") == "Paracetamol 500 mg · lot L-26-0377 · HPLC-02");
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
    screen.submit("find", "S-26-104176");
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

TEST_CASE("console, compact: menu navigation, stacked sections, a sample opens result entry", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK_FALSE(screen.find("Column", "sidebar").has_value());
    CHECK(screen.find("Menu", "nav-menu").has_value());
    CHECK(screen.prop("Collapsible", "chart-section", "title") == "Assay control, HPLC-02 · latest run +3.4 SD");
    CHECK(screen.find("Column", "worklist").has_value());
    CHECK(screen.prop("Grid", "kpis", "columns") == "2");
    screen.click("open-sample");
    CHECK(screen.waitFor([&screen] { return screen.mounted().size() == 2; }));
    CHECK(screen.mounted().back() == "lims-gallery.results");
    screen.requireNoProblems();
}
