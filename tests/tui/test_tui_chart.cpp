// SPDX-License-Identifier: Apache-2.0
//
// The chart, plotted in character cells.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "tui/display_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using morph::tui::detail::ChartImpl;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

namespace {

// One dashed error line at the middle of 0 to 10, ticks at 0 and 10, and three points, the last marked with a note.
// The marked point's 8.6 lands 0.98 of a row below the top: on row 1, and on row 0 when the row is truncated.
ui::ChartData sample() {
    ui::ChartData data;
    data.xMin = -0.1;
    data.xMax = 2.1;
    data.yMin = 0.0;
    data.yMax = 10.0;
    data.yTicks = {{.value = 0.0, .text = "0"}, {.value = 10.0, .text = "10"}};
    data.lines = {{.value = 5.0, .label = "limit", .tone = ui::Tone::Err, .style = ui::ChartLineStyle::Dashed}};
    data.series = {{.label = {},
                    .joined = true,
                    .points = {{.x = 0.0, .y = 2.0},
                               {.x = 1.0, .y = 6.0},
                               {.x = 1.6, .y = 8.6, .tone = ui::Tone::Err, .note = "high"}}}};
    data.summary = "Three runs, the last one high";
    return data;
}

}  // namespace

// Mutation: truncate instead of rounding in `Plot::rowOf` (the marked point moves up to row 0).
TEST_CASE("tui chart: lines, ticks and points in cells, a marked point as a cross with its note", "[tui][chart]") {
    Harness harness{30, 8};
    auto const chart = harness.make<ChartImpl>(nullptr);
    chart->setData(sample());
    CHECK(harness.draw() == Rows{"10", "                       ✕ high", "", "                ●",
                                 "   ╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌limit", "", "    ●", " 0"});
    CHECK(chart->probeText() == "Three runs, the last one high");
}

// Mutation: drop the size check in `paint` (a 15 by 4 chart then plots).
TEST_CASE("tui chart: too small to plot, the chart shows its summary", "[tui][chart]") {
    Harness harness{15, 4};
    auto const chart = harness.make<ChartImpl>(nullptr);
    chart->setData(sample());
    CHECK(harness.draw() == Rows{"Three runs, the", "last one high"});
}

TEST_CASE("tui chart: its content size is 40 by 8 cells", "[tui][chart]") {
    Harness harness{60, 20};
    auto const chart = harness.make<ChartImpl>(nullptr);
    CHECK(chart->naturalSize().width == 40);
    CHECK(chart->naturalSize().height == 8);
}
