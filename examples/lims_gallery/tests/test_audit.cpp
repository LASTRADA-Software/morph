// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.audit";

// Whether a text showing @p text is mounted: a table row or a card of the trail.
// A text given runs is recorded as its runs, so it shows the text when its runs' characters are it.
bool showsText(GalleryScreen& screen, std::string_view text) {
    if (screen.backend().find("Text", "text", text).has_value()) {
        return true;
    }
    return std::ranges::any_of(screen.backend().all("Text"), [&screen, text](int widgetId) {
        return morph::ui::testing::detail::spansCharacters(screen.backend().prop(widgetId, "spans")) == text;
    });
}

}  // namespace

TEST_CASE("audit, expanded: facets beside the trail, with the active filters as chips", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK(screen.shows("Column", "facets"));
    CHECK(screen.prop("Checkbox", "type-result", "checked") == "true");
    CHECK(screen.prop("Checkbox", "type-specification", "checked") == "false");
    CHECK(screen.shows("Table", "trail"));
    CHECK(screen.prop("Table", "trail", "columns") ==
          "[When:fixed(10);valign=start;wrap=false,User:fixed(6);valign=start,Record:fixed(12);valign=start,"
          "Field:fixed(8);valign=start,Old value:stretch(3);valign=start,New value:stretch(4);valign=start,"
          "Reason:stretch(5);valign=start]");
    CHECK(showsText(screen, "WS-2026-0412"));
    CHECK(showsText(screen, "Transcription error: peak 7 integrated with the wrong baseline."));
    CHECK(screen.shows("Badge", "chip-type-result"));
    CHECK(screen.shows("Badge", "chip-action-modified"));
    CHECK(screen.shows("Badge", "chip-dates"));
    CHECK_FALSE(screen.shows("Badge", "chip-type-specification"));
    CHECK(screen.find("Row", "pager").has_value());
    CHECK(screen.prop("Button", "head_action_pdf", "label") == "Export PDF for inspection");
}

TEST_CASE("audit, expanded: a facet narrows the trail and its chip removes it", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    // The specification change is outside the checked record types until its box is checked.
    CHECK_FALSE(showsText(screen, "SPEC-ASSAY-07"));
    screen.toggle("type-specification");
    CHECK(screen.waitFor([&screen] { return showsText(screen, "SPEC-ASSAY-07"); }));
    CHECK(screen.shows("Badge", "chip-type-specification"));
    screen.choose("user", "jruiz");
    CHECK(screen.waitFor([&screen] { return !showsText(screen, "WS-2026-0412"); }));
    CHECK(showsText(screen, "S-26-104217"));
    // The search is a head tool on the expanded layout.
    screen.edit("head_tool_search", "no such record");
    CHECK(screen.waitFor([&screen] { return screen.shows("EmptyState", "no-entries"); }));
    screen.click("clear-all");
    CHECK(
        screen.waitFor([&screen] { return showsText(screen, "WS-2026-0412") && showsText(screen, "SPEC-ASSAY-07"); }));
    CHECK_FALSE(screen.shows("Badge", "chip-type-result"));
    CHECK_FALSE(screen.shows("Badge", "chip-dates"));
    screen.requireNoProblems();
}

TEST_CASE("audit, compact: entries stack as cards, filters in a drawer", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK(screen.find("Column", "entries").has_value());
    CHECK(showsText(screen, "24-Oct-2026 09:40:12"));
    CHECK(showsText(screen, "S-26-104170 · Impurity B · Result"));
    // The whole record line is mono.
    CHECK(screen.backend().dump().contains("[S-26-104170 · Impurity B · Result;mono]"));
    CHECK(showsText(screen, "Reason: F-02 out of range during defrost cycle."));
    CHECK_FALSE(screen.shows("Table", "trail"));
    CHECK(screen.prop("Button", "open-filters", "label") == "Filters · 4");
    CHECK(screen.prop("Button", "head_action_pdf", "label") == "Export PDF");
    screen.click("open-filters");
    REQUIRE(screen.waitFor([&screen] { return screen.find("Checkbox", "type-sample").has_value(); }));
    screen.toggle("type-sample");
    CHECK(screen.waitFor([&screen] { return screen.prop("Button", "open-filters", "label") == "Filters · 3"; }));
    CHECK(screen.waitFor([&screen] { return !showsText(screen, "S-26-104217 · Received at"); }));
    screen.requireNoProblems();
}

// Mutation: write the pager's arrows as default buttons, or the facet labels as muted capitals again.
TEST_CASE("audit, expanded: the pager is square glyph buttons and the facet labels are overlines", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    for (auto const& [name, icon] : {std::pair{"Previous page", "nav.back"}, std::pair{"Next page", "nav.forward"}}) {
        auto const button = screen.backend().find("Button", "a11yName", name);
        REQUIRE(button.has_value());
        CHECK(screen.backend().prop(button.value_or(-1), "shape") == "square");
        CHECK(screen.backend().prop(button.value_or(-1), "icon") == icon);
    }
    for (char const* label : {"Record type", "Action"}) {
        auto const heading = screen.backend().find("Text", "text", label);
        REQUIRE(heading.has_value());
        CHECK(screen.backend().prop(heading.value_or(-1), "role") == "Overline");
    }
}
