// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <morph/ui/document.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gallery_harness.hpp"
#include "lims_gallery/lims_gallery.hpp"
#include "lims_gallery/screens.hpp"

using lims_gallery::Design;
using lims_gallery::testing::GalleryScreen;
using lims_gallery::testing::kWidths;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

namespace {

[[nodiscard]] std::size_t builtDesigns() {
    return static_cast<std::size_t>(
        std::ranges::count_if(lims_gallery::designs(), [](Design const& design) { return design.make != nullptr; }));
}

}  // namespace

TEST_CASE("lims gallery: the bundle registers the index and every built design, with its theme", "[lims-gallery]") {
    auto const built = lims_gallery::bundle();
    INFO((built ? std::string{} : built.error().message()));
    REQUIRE(built.has_value());
    CHECK((*built)->screens.size() == 1 + builtDesigns());
    CHECK(lims_gallery::designs().size() == 10);
    CHECK(builtDesigns() >= 1);
}

TEST_CASE("lims gallery: the index lists every design and opens a built one", "[lims-gallery]") {
    RigMode const mode = GENERATE(RigMode::Local, RigMode::Simulated);
    INFO("mode " << morph::examples::testing::nameOf(mode));
    ScreenHarness harness{lims_gallery::localApp(), std::string{lims_gallery::kIndexScreen}, mode};
    CHECK(harness.mounted() == std::vector<std::string>{std::string{lims_gallery::kIndexScreen}});
    for (Design const& design : lims_gallery::designs()) {
        std::string const number{design.number};
        INFO("design " << number);
        CHECK(harness.find("Row", "design-" + number).has_value());
        CHECK(harness.find("Button", "open-" + number).has_value() == (design.make != nullptr));
        CHECK(harness.find("Badge", "unavailable-" + number).has_value() == (design.make == nullptr));
    }
    harness.click("open-01");
    CHECK(harness.waitFor([&harness] { return harness.mounted().size() == 2; }));
    CHECK(harness.mounted().back() == "lims-gallery.esignature");
    CHECK(harness.problems().empty());
}

TEST_CASE("lims gallery: every built design mounts at both widths without a problem", "[lims-gallery]") {
    std::string_view const width = GENERATE(kWidths.front(), kWidths.back());
    for (Design const& design : lims_gallery::designs()) {
        if (design.make == nullptr) {
            continue;
        }
        INFO("design " << design.number << " at " << width);
        GalleryScreen screen{design.id, width};
        CHECK(screen.mounted() == std::vector<std::string>{std::string{design.id}});
        screen.requireNoProblems();
    }
}

// Mutation: leave `formatting` out of the app document (the patterns are empty).
TEST_CASE("lims gallery: the app document gives every screen the gallery's date patterns", "[lims-gallery]") {
    auto const app = morph::ui::loadDocumentText(lims_gallery::appDocument());
    REQUIRE(app.has_value());
    CHECK(app->datePattern == lims_gallery::kDatePattern);
    CHECK(app->dateTimePattern == lims_gallery::kDateTimePattern);
}

// Seven designs draw their compact window on a white page; samples and console keep the grey one.
// Mutation: drop a screen's `pageSurface`, or declare the `plainPage` surface nowhere (the page surface is then
// `page` at compact, and the white-page check fails).
TEST_CASE("lims gallery: seven screens draw a white page at compact only", "[lims-gallery]") {
    auto const [screenId, white] = GENERATE(
        std::pair<std::string_view, bool>{"receipt", true}, std::pair<std::string_view, bool>{"instrument", true},
        std::pair<std::string_view, bool>{"audit", true}, std::pair<std::string_view, bool>{"stability", true},
        std::pair<std::string_view, bool>{"plate", true}, std::pair<std::string_view, bool>{"results", true},
        std::pair<std::string_view, bool>{"review", true}, std::pair<std::string_view, bool>{"samples", false},
        std::pair<std::string_view, bool>{"console", false});
    std::string const screen = "lims-gallery." + std::string{screenId};
    INFO(screen);
    GalleryScreen compact{screen, "compact"};
    CHECK(compact.prop("Column", "shell_page", "surface") == (white ? "plainPage" : "page"));
    GalleryScreen expanded{screen, "expanded"};
    CHECK(expanded.prop("Column", "shell_page", "surface") == "page");
}
