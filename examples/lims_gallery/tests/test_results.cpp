// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include "gallery_harness.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.results";

// The result input named @p name.
int resultInput(GalleryScreen& screen, std::string const& name) {
    auto const found = screen.backend().find("TextInput", "a11yName", name);
    if (!found) {
        throw std::runtime_error{"no result input " + name};
    }
    return *found;
}

bool hasBadge(GalleryScreen& screen, std::string const& text) {
    return screen.backend().find("Badge", "text", text).has_value();
}

}  // namespace

TEST_CASE("results, expanded: the worksheet grid, its calculations and the change awaiting its reason",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    // The worksheet line is the head's eyebrow, above the title, with its identifiers in the mono face.
    CHECK(screen.text("head_eyebrow") == "Worksheet WS-2026-0412 · HPLC-02 · method TM-ASSAY-07 v3");
    CHECK(screen.backend().prop(screen.find("Text", "head_eyebrow").value(), "spans").contains("WS-2026-0412;mono"));
    CHECK(screen.find("Panel", "suitability").has_value());
    CHECK(screen.shows("Table", "results"));
    // Each sample's results sit under a heading row across the worksheet.
    std::size_t headings = 0;
    for (int const row : screen.backend().all("Row")) {
        headings += screen.backend().prop(row, "heading") == "true" ? 1U : 0U;
    }
    CHECK(headings == 2);
    CHECK(screen.backend().find("Text", "text", "· lot L-26-0381").has_value());
    CHECK(screen.backend().prop(resultInput(screen, "Assay, mean result, S-26-104176"), "text") == "99.4");
    CHECK(screen.backend().prop(resultInput(screen, "Assay, mean result, S-26-104176"), "readonly") == "true");
    CHECK(screen.backend().prop(resultInput(screen, "Total impurities result, S-26-104170"), "text") == "0.24");
    CHECK(screen.backend().find("Text", "text", "RSD 0.28 %").has_value());
    CHECK(hasBadge(screen, "Out of spec: above 0.20"));
    auto const impurityB = screen.backend().find("Row", "rowKey", "\"ib2\"");
    REQUIRE(impurityB.has_value());
    CHECK(screen.backend().prop(impurityB.value_or(0), "tone") == "Err");
    CHECK(screen.text("oos") == "1 result out of specification. Submitting opens an investigation.");
    CHECK(screen.prop("Dialog", "change", "open") == "true");
    // The change line is the dialog's description, as runs: the sample's id mono, the two values strong.
    CHECK(morph::ui::testing::detail::spansCharacters(
              screen.prop("Dialog", "change", "descriptionSpans").value_or("")) ==
          "S-26-104170 · Impurity B: 0.12 % → 0.21 %");
    CHECK(screen.prop("Dialog", "change", "descriptionSpans") ==
          "[S-26-104170;mono, · Impurity B: ,0.12 %;strong, → ,0.21 %;strong]");
    CHECK(screen.prop("Button", "submit", "enabled") == "false");
}

TEST_CASE("results, expanded: undoing the change, then editing a result asks for its reason", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("action_undo");
    CHECK(screen.waitFor([&screen] { return screen.prop("Dialog", "change", "open") == "false"; }));
    CHECK(screen.backend().prop(resultInput(screen, "Impurity B result, S-26-104170"), "text") == "0.12");
    CHECK(screen.backend().prop(resultInput(screen, "Total impurities result, S-26-104170"), "text") == "0.15");
    CHECK_FALSE(hasBadge(screen, "Out of spec: above 0.20"));
    CHECK(screen.text("oos") == "");
    CHECK(screen.prop("Button", "submit", "enabled") == "true");

    screen.backend().submit(resultInput(screen, "Assay, prep 1 result, S-26-104176"), "101.0");
    CHECK(screen.waitFor([&screen] { return screen.prop("Dialog", "change", "open") == "true"; }));
    CHECK(morph::ui::testing::detail::spansCharacters(
              screen.prop("Dialog", "change", "descriptionSpans").value_or("")) ==
          "S-26-104176 · Assay, prep 1: 99.6 % LC → 101.0 % LC");
    CHECK(screen.backend().prop(resultInput(screen, "Assay, mean result, S-26-104176"), "text") == "100.1");
    CHECK(screen.prop("Button", "action_confirm", "enabled") == "false");
    screen.choose("reason", "transcription");
    screen.edit("comment", "Typed from the wrong line of the printout.");
    CHECK(screen.waitFor([&screen] { return screen.prop("Button", "action_confirm", "enabled") == "true"; }));
    screen.click("action_confirm");
    CHECK(screen.waitFor([&screen] { return screen.prop("Dialog", "change", "open") == "false"; }));
    CHECK(screen.backend().prop(resultInput(screen, "Assay, prep 1 result, S-26-104176"), "text") == "101.0");
    screen.requireNoProblems();
}

// Mutation: compute the mean with `mean`, half to even at the preparations' places -> the mean reads 99.4.
TEST_CASE("results: the assay mean is the exact mean rounded once, half up", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("action_undo");
    // 99.6 and 99.3: the exact mean 99.45 reports as 99.5; half to even, or a mean rounded first, shows 99.4.
    screen.backend().submit(resultInput(screen, "Assay, prep 2 result, S-26-104176"), "99.3");
    CHECK(screen.waitFor([&screen] {
        return screen.backend().prop(resultInput(screen, "Assay, mean result, S-26-104176"), "text") == "99.5";
    }));
    CHECK(screen.backend().find("Text", "text", "RSD 0.21 %").has_value());
}

// Mutation: round only the display, comparing the half-to-even mean -> the line shows 105.1 but is within spec.
TEST_CASE("results: the mean compared with its limits is the mean shown", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("action_undo");
    // Each edit asks for its reason before the next one.
    for (auto const& [name, value] : {std::pair{"Assay, prep 1 result, S-26-104176", "105.0"},
                                      std::pair{"Assay, prep 2 result, S-26-104176", "105.1"}}) {
        screen.backend().submit(resultInput(screen, name), value);
        REQUIRE(screen.waitFor([&screen] { return screen.prop("Dialog", "change", "open") == "true"; }));
        screen.choose("reason", "transcription");
        screen.edit("comment", "Typed from the wrong line of the printout.");
        REQUIRE(screen.waitFor([&screen] { return screen.prop("Button", "action_confirm", "enabled") == "true"; }));
        screen.click("action_confirm");
        REQUIRE(screen.waitFor([&screen] { return screen.prop("Dialog", "change", "open") == "false"; }));
    }
    // The exact mean 105.05 reports as 105.1, above the 105.0 limit.
    CHECK(screen.waitFor([&screen] {
        return screen.backend().prop(resultInput(screen, "Assay, mean result, S-26-104176"), "text") == "105.1";
    }));
    auto const mean = screen.backend().find("Row", "rowKey", "\"mean\"");
    REQUIRE(mean.has_value());
    CHECK(screen.backend().prop(mean.value_or(0), "tone") == "Err");
    CHECK(hasBadge(screen, "Out of spec: above 105.0"));
}

TEST_CASE("results: a result with more places than its specification is not read", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("action_undo");
    screen.backend().submit(resultInput(screen, "Impurity A result, S-26-104170"), "0.031");
    CHECK(screen.waitFor([&screen] { return hasBadge(screen, "Enter to 2 decimal places"); }));
    CHECK(screen.backend().prop(resultInput(screen, "Impurity A result, S-26-104170"), "errors") ==
          "[Enter to 2 decimal places.]");
    screen.requireNoProblems();
}

TEST_CASE("results, compact: one sample at a time, the reason asked inline", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK_FALSE(screen.find("Table", "results").has_value());
    CHECK(screen.text("sample") == "Sample 2 of 2 · S-26-104170");
    CHECK(screen.shows("Column", "inline-reason"));
    CHECK(screen.backend().find("Text", "text", "was 0.12 %").has_value());
    // The evaluation is a tag under the field, not a description.
    CHECK(screen.backend().find("Badge", "text", "Out of spec: above 0.20").has_value());
    CHECK(screen.backend().find("Badge", "text", "Within spec").has_value());
    CHECK(screen.backend().dump().contains("[WS-2026-0412;mono, · HPLC-02]"));
    CHECK(screen.prop("Button", "next", "enabled") == "false");
    screen.click("previous");
    CHECK(screen.waitForText("sample", "Sample 1 of 2 · S-26-104176"));
    CHECK(screen.backend().prop(resultInput(screen, "Assay, mean result, S-26-104176"), "text") == "99.4");
    screen.click("next");
    CHECK(screen.waitForText("sample", "Sample 2 of 2 · S-26-104170"));
    screen.click("confirm");
    CHECK(screen.waitFor([&screen] { return !screen.shows("Column", "inline-reason"); }));
    CHECK(screen.prop("Button", "submit", "enabled") == "true");
    screen.requireNoProblems();
}
