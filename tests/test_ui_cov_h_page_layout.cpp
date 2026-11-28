// SPDX-License-Identifier: Apache-2.0

// Page geometry: the lengths a page and its regions may hold, the box each region lays out into, and the cells a
// length is rounded to. Each case is a geometry with the reason it is refused, or the box it lays out into.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <morph/ui/page_layout.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using morph::ui::CellPitch;
using morph::ui::Mm;
using morph::ui::PageAnchor;
using morph::ui::PageGeometry;
using morph::ui::PageMargins;
using morph::ui::PageRect;
using morph::ui::RegionGeometry;
using morph::ui::RegionSpec;

PageGeometry a4(PageMargins margins = PageMargins{Mm{20}}) {
    return PageGeometry{.width = Mm{210}, .height = Mm{297}, .margins = margins};
}

}  // namespace

TEST_CASE("cov h page layout: a length is finite and fits a signed 32-bit count of micrometres", "[covh][ui][page]") {
    CHECK_THROWS_AS(Mm{2'200'000.0}, std::invalid_argument);
    CHECK_THROWS_AS(Mm{-2'200'000.0}, std::invalid_argument);
    CHECK_THROWS_AS(Mm{std::numeric_limits<double>::infinity()}, std::invalid_argument);
    CHECK_THROWS_AS(Mm{std::nan("")}, std::invalid_argument);
    CHECK(Mm{2'000'000.0}.micrometres() == 2'000'000'000);
    CHECK(Mm{-1.2345}.text() == "-1.235");
    CHECK(Mm{3.5}.text() == "3.5");
    CHECK(Mm{3}.text() == "3");
    CHECK(Mm{0.05}.text() == "0.05");
    CHECK(Mm::fromMicrometres(2'000'000'000) + Mm::fromMicrometres(2'000'000'000) ==
          Mm::fromMicrometres(std::numeric_limits<std::int32_t>::max()));
    CHECK(Mm::fromMicrometres(-2'000'000'000) - Mm::fromMicrometres(2'000'000'000) ==
          Mm::fromMicrometres(std::numeric_limits<std::int32_t>::lowest()));
    CHECK(std::is_lt(Mm{1} <=> Mm{2}));
    CHECK(std::is_eq(Mm{2} <=> Mm{2}));
    CHECK(std::is_gt(Mm{3} <=> Mm{2}));
    CHECK(PageMargins{Mm{1}} == PageMargins{Mm{1}, Mm{1}, Mm{1}, Mm{1}});
    CHECK_FALSE(PageMargins{Mm{1}} == PageMargins{Mm{1}, Mm{1}, Mm{1}, Mm{2}});
    CHECK_FALSE(PageMargins{Mm{1}} == PageMargins{Mm{2}, Mm{1}, Mm{1}, Mm{1}});
}

TEST_CASE("cov h page layout: a page is refused for a side, a margin or a box that is not there", "[covh][ui][page]") {
    struct Case {
        char const* what;
        PageGeometry page;
        std::optional<std::string> expected;
    };
    auto const sized = [](double width, double height, PageMargins margins = PageMargins{Mm{5}}) {
        return PageGeometry{.width = Mm{width}, .height = Mm{height}, .margins = margins};
    };
    for (Case const& item : std::vector<Case>{
             {"an A4 page", a4(), std::nullopt},
             {"a width under the least", sized(9, 297), "a page side is 10 mm to 5000 mm"},
             {"a width over the most", sized(5001, 297), "a page side is 10 mm to 5000 mm"},
             {"a height under the least", sized(210, 9), "a page side is 10 mm to 5000 mm"},
             {"a height over the most", sized(210, 5001), "a page side is 10 mm to 5000 mm"},
             {"a negative top margin", sized(210, 297, PageMargins{Mm{-1}, Mm{5}, Mm{5}, Mm{5}}),
              "a page margin is not negative"},
             {"a negative left margin", sized(210, 297, PageMargins{Mm{5}, Mm{5}, Mm{5}, Mm{-1}}),
              "a page margin is not negative"},
             {"margins as wide as the page", sized(210, 297, PageMargins{Mm{0}, Mm{105}, Mm{0}, Mm{105}}),
              "a page's margins leave no margin box"},
             {"margins as high as the page", sized(210, 297, PageMargins{Mm{148.5}, Mm{0}, Mm{148.5}, Mm{0}}),
              "a page's margins leave no margin box"},
         }) {
        INFO(item.what);
        CHECK(item.page.problem() == item.expected);
    }
}

TEST_CASE("cov h page layout: a region is refused for an offset, a size or a place off the sheet",
          "[covh][ui][page]") {
    struct Case {
        char const* what;
        RegionGeometry region;
        std::optional<std::string> expected;
    };
    PageGeometry const page = a4();
    for (Case const& item : std::vector<Case>{
             {"a pinned region",
              {.anchor = PageAnchor::Top, .x = {}, .y = {}, .width = {}, .height = Mm{30}},
              std::nullopt},
             {"a negative x",
              {.anchor = PageAnchor::Offset, .x = Mm{-1}, .y = {}, .width = {}, .height = {}},
              "a page region's offset is not negative"},
             {"a negative y",
              {.anchor = PageAnchor::Offset, .x = {}, .y = Mm{-1}, .width = {}, .height = {}},
              "a page region's offset is not negative"},
             {"an x on a pinned region",
              {.anchor = PageAnchor::Top, .x = Mm{1}, .y = {}, .width = {}, .height = {}},
              "a pinned page region has no offset"},
             {"a y on a pinned region",
              {.anchor = PageAnchor::Bottom, .x = {}, .y = Mm{1}, .width = {}, .height = {}},
              "a pinned page region has no offset"},
             {"a width of zero",
              {.anchor = PageAnchor::Top, .x = {}, .y = {}, .width = Mm{0}, .height = {}},
              "a page region's width and height are above zero"},
             {"a height of zero",
              {.anchor = PageAnchor::Top, .x = {}, .y = {}, .width = {}, .height = Mm{0}},
              "a page region's width and height are above zero"},
             {"a width of zero beside a height",
              {.anchor = PageAnchor::Top, .x = {}, .y = {}, .width = Mm{0}, .height = Mm{5}},
              "a page region's width and height are above zero"},
             {"an offset region with no width past the margin box",
              {.anchor = PageAnchor::Offset, .x = Mm{190}, .y = {}, .width = {}, .height = {}},
              "an offset page region with no width starts at or past the margin box's right edge"},
             {"an offset region with a width inside the margin box",
              {.anchor = PageAnchor::Offset, .x = Mm{190}, .y = {}, .width = Mm{5}, .height = {}},
              std::nullopt},
             {"a width leaving the sheet",
              {.anchor = PageAnchor::Offset, .x = Mm{190}, .y = {}, .width = Mm{30}, .height = {}},
              "a page region leaves the sheet's width"},
             {"a pinned width leaving the sheet",
              {.anchor = PageAnchor::Top, .x = {}, .y = {}, .width = Mm{200}, .height = {}},
              "a page region leaves the sheet's width"},
             {"an offset height leaving the sheet",
              {.anchor = PageAnchor::Offset, .x = {}, .y = Mm{290}, .width = {}, .height = Mm{10}},
              "a page region leaves the sheet's height"},
             {"an offset height inside the sheet",
              {.anchor = PageAnchor::Offset, .x = {}, .y = Mm{280}, .width = {}, .height = Mm{10}},
              std::nullopt},
             {"a top height leaving the sheet",
              {.anchor = PageAnchor::Top, .x = {}, .y = {}, .width = {}, .height = Mm{290}},
              "a page region leaves the sheet's height"},
             {"a bottom height taller than the sheet",
              {.anchor = PageAnchor::Bottom, .x = {}, .y = {}, .width = {}, .height = Mm{300}},
              "a page region leaves the sheet's height"},
             {"a bottom height inside the sheet",
              {.anchor = PageAnchor::Bottom, .x = {}, .y = {}, .width = {}, .height = Mm{30}},
              std::nullopt},
         }) {
        INFO(item.what);
        CHECK(item.region.problem(page) == item.expected);
    }
    CHECK(morph::ui::anchorName(PageAnchor::Top) == std::string{"Top"});
    CHECK(morph::ui::anchorName(PageAnchor::Bottom) == std::string{"Bottom"});
    CHECK(morph::ui::anchorName(PageAnchor::Offset) == std::string{"Offset"});
    CHECK(PageRect{Mm{1}, Mm{2}, Mm{3.5}, Mm{4}}.text() == "1,2,3.5,4");
}

TEST_CASE("cov h page layout: a region lays out into its box and overflows where its content does not fit",
          "[covh][ui][page]") {
    PageGeometry const page = a4();
    struct Case {
        char const* what;
        RegionSpec spec;
        PageRect rect;
        bool overflow;
    };
    for (Case const& item : std::vector<Case>{
             {"a header as high as its content",
              {.geometry = {.anchor = PageAnchor::Top, .x = {}, .y = {}, .width = {}, .height = {}},
               .contentHeight = Mm{10}},
              {.x = Mm{20}, .y = Mm{20}, .width = Mm{170}, .height = Mm{10}},
              false},
             {"a footer pinned to the bottom margin",
              {.geometry = {.anchor = PageAnchor::Bottom, .x = {}, .y = {}, .width = {}, .height = {}},
               .contentHeight = Mm{10}},
              {.x = Mm{20}, .y = Mm{267}, .width = Mm{170}, .height = Mm{10}},
              false},
             {"a fixed height the content outgrows",
              {.geometry = {.anchor = PageAnchor::Top, .x = {}, .y = {}, .width = Mm{100}, .height = Mm{10}},
               .contentHeight = Mm{12}},
              {.x = Mm{20}, .y = Mm{20}, .width = Mm{100}, .height = Mm{10}},
              true},
             {"a fixed height the content fits",
              {.geometry = {.anchor = PageAnchor::Top, .x = {}, .y = {}, .width = {}, .height = Mm{10}},
               .contentHeight = Mm{10}},
              {.x = Mm{20}, .y = Mm{20}, .width = Mm{170}, .height = Mm{10}},
              false},
             {"an offset region to the margin box's edge",
              {.geometry = {.anchor = PageAnchor::Offset, .x = Mm{50}, .y = Mm{40}, .width = {}, .height = {}},
               .contentHeight = Mm{5}},
              {.x = Mm{50}, .y = Mm{40}, .width = Mm{140}, .height = Mm{5}},
              false},
             {"an offset region past the margin box has no width",
              {.geometry = {.anchor = PageAnchor::Offset, .x = Mm{195}, .y = Mm{40}, .width = {}, .height = {}},
               .contentHeight = Mm{5}},
              {.x = Mm{195}, .y = Mm{40}, .width = Mm{0}, .height = Mm{5}},
              false},
             {"content hanging below the sheet",
              {.geometry = {.anchor = PageAnchor::Offset, .x = Mm{20}, .y = Mm{290}, .width = {}, .height = {}},
               .contentHeight = Mm{20}},
              {.x = Mm{20}, .y = Mm{290}, .width = Mm{170}, .height = Mm{20}},
              true},
             {"a footer taller than the page starts above it",
              {.geometry = {.anchor = PageAnchor::Bottom, .x = {}, .y = {}, .width = {}, .height = {}},
               .contentHeight = Mm{290}},
              {.x = Mm{20}, .y = Mm{-13}, .width = Mm{170}, .height = Mm{290}},
              true},
         }) {
        INFO(item.what);
        std::vector<RegionSpec> const specs{item.spec};
        auto const layout = morph::ui::layoutPage(page, specs, std::nullopt);
        REQUIRE(layout.regions.size() == 1);
        CHECK(layout.regions.front().rect == item.rect);
        CHECK(layout.regions.front().overflow == item.overflow);
        CHECK(layout.scale == 1.0);
    }
    auto const narrow = morph::ui::layoutPage(page, {}, Mm{105});
    CHECK(narrow.scale == 0.5);
    CHECK(narrow.displayedWidth == Mm{105});
    CHECK(morph::ui::layoutPage(page, {}, Mm{400}).scale == 1.0);
}

TEST_CASE("cov h page layout: lengths round to the nearest cell, before and after the origin", "[covh][ui][page]") {
    CellPitch const pitch{.columns = 10, .rows = 6};
    CHECK(morph::ui::columnsOf(Mm{25.4}, pitch) == 10);
    CHECK(morph::ui::columnsOf(Mm{-25.4}, pitch) == -10);
    CHECK(morph::ui::columnsOf(Mm{-1.3}, pitch) == -1);
    CHECK(morph::ui::columnsOf(Mm{1.2}, pitch) == 0);
    CHECK(morph::ui::rowsOf(Mm{25.4}, pitch) == 6);
    CHECK(morph::ui::rowsOf(Mm{-12.7}, pitch) == -3);
    PageRect const rect{.x = Mm{-25.4}, .y = Mm{25.4}, .width = Mm{50.8}, .height = Mm{25.4}};
    auto const cells = morph::ui::toCells(rect, pitch);
    CHECK(cells == morph::ui::CellRect{.column = -10, .row = 6, .width = 20, .height = 6});
    for (PageAnchor const anchor : {PageAnchor::Top, PageAnchor::Offset}) {
        auto const kept = morph::ui::toCells(rect, anchor, pitch, 4);
        CHECK(kept == morph::ui::CellRect{.column = -10, .row = 6, .width = 20, .height = 4});
    }
    auto const bottom = morph::ui::toCells(rect, PageAnchor::Bottom, pitch, 4);
    CHECK(bottom == morph::ui::CellRect{.column = -10, .row = 8, .width = 20, .height = 4});
}
