// SPDX-License-Identifier: Apache-2.0

// A page's mount names what it refuses, and a page takes the paper surface unless it was given one.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/page_layout.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace ui = morph::ui;
using ui::Mm;

namespace {

struct Rig {
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    ui::testing::RecordingBackend backend;
    ui::PageGeometry const a4{.width = Mm{210}, .height = Mm{297}, .margins = ui::PageMargins{Mm{10}}};
};

}  // namespace

TEST_CASE("covj page mount: a stretched page is refused by name", "[covj][ui][page]") {
    using Catch::Matchers::ContainsSubstring;
    Rig rig;
    ui::Common common;
    common.testId = "sheet";
    common.layout.alignSelf = ui::Align::Stretch;
    CHECK_THROWS_WITH(
        (ui::Mounted{rig.runtime, rig.backend,
                     ui::column({.children = {ui::page(ui::Page{.geometry = rig.a4, .common = common})}})}),
        ContainsSubstring("a page is never stretched") && ContainsSubstring("(node `sheet`)"));
    ui::Common anonymous;
    anonymous.layout.alignSelf = ui::Align::Stretch;
    CHECK_THROWS_WITH(
        (ui::Mounted{rig.runtime, rig.backend,
                     ui::column({.children = {ui::page(ui::Page{.geometry = rig.a4, .common = anonymous})}})}),
        ContainsSubstring("a page is never stretched") && !ContainsSubstring("(node"));
}

TEST_CASE("covj page mount: a page that places itself with alignSelf mounts", "[covj][ui][page]") {
    Rig rig;
    ui::Common common;
    common.layout.alignSelf = ui::Align::Center;
    CHECK_NOTHROW((ui::Mounted{rig.runtime, rig.backend,
                               ui::column({.children = {ui::page(ui::Page{.geometry = rig.a4, .common = common})}})}));
}

TEST_CASE("covj page mount: a page is on paper unless a surface was given, literal or bound", "[covj][ui][page]") {
    Rig rig;
    auto const surfaceOf = [&rig](ui::Common common) {
        ui::Mounted const mounted{
            rig.runtime, rig.backend,
            ui::column({.children = {ui::page(ui::Page{.geometry = rig.a4, .common = common})}})};
        auto const pages = rig.backend.all("Page");
        REQUIRE(pages.size() == 1);
        return rig.backend.prop(pages.front(), "surface");
    };
    CHECK(surfaceOf({}) == "paper");
    ui::Common literal;
    literal.surface = ui::Prop<std::string>{"card"};
    CHECK(surfaceOf(literal) == "card");
}
