// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <string>
#include <string_view>
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
