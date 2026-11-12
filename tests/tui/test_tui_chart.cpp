// SPDX-License-Identifier: Apache-2.0
//
// The chart, plotted in character cells.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
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

namespace {

std::string blocks(std::string_view glyph, int count) {
    std::string out;
    for (int index = 0; index < count; ++index) {
        out += glyph;
    }
    return out;
}

// Bars at 0 and 1 over 0 to 8 on a 30 by 10 chart: ten plot rows, so a value v has its tip 9v/8 + 1/2 rows above the
// bottom edge. The bars are 11 columns wide (0.8 of a 14-column slot), centred on columns 8 and 22.
ui::ChartData bars() {
    ui::ChartData data;
    data.xMin = -0.5;
    data.xMax = 1.5;
    data.yMin = 0.0;
    data.yMax = 8.0;
    data.barSlot = 1.0;
    data.series = {{.label = {},
                    .joined = false,
                    .style = ui::ChartStyle::Bars,
                    .points = {{.x = 0.0, .y = 8.0}, {.x = 1.0, .y = 2.5}}}};
    data.summary = "Two bars";
    return data;
}

}  // namespace

// Mutation: draw a `Bars` series as points (the rows then hold `●` and no block).
TEST_CASE("tui chart: a bars series is a run of block characters, its end cell a partial block", "[tui][chart]") {
    Harness harness{30, 10};
    auto const chart = harness.make<ChartImpl>(nullptr);
    chart->setData(bars());
    // 8 is 76 eighths up: nine full cells and a half block; 2.5 is 27 eighths: three full cells and 3 eighths.
    Rows expected{"   " + blocks("▄", 11)};
    for (int row = 1; row <= 5; ++row) {
        expected.push_back("   " + blocks("█", 11));
    }
    expected.push_back("   " + blocks("█", 11) + "   " + blocks("▃", 11));
    for (int row = 7; row <= 9; ++row) {
        expected.push_back("   " + blocks("█", 11) + "   " + blocks("█", 11));
    }
    CHECK(harness.draw() == expected);
}

// Mutation: draw a bar growing down upward, or give its end cell the covered eighths instead of the uncovered ones.
TEST_CASE("tui chart: a bar below the baseline hangs from the top edge, its end cell the uncovered block",
          "[tui][chart]") {
    Harness harness{30, 10};
    auto const chart = harness.make<ChartImpl>(nullptr);
    ui::ChartData data = bars();
    data.yMin = -8.0;
    data.yMax = 0.0;
    data.series.front().points = {{.x = 0.0, .y = -8.0}, {.x = 1.0, .y = -2.5}};
    chart->setData(data);
    Rows expected;
    for (int row = 0; row <= 2; ++row) {
        expected.push_back("   " + blocks("█", 11) + "   " + blocks("█", 11));
    }
    expected.push_back("   " + blocks("█", 11) + "   " + blocks("▅", 11));
    for (int row = 4; row <= 8; ++row) {
        expected.push_back("   " + blocks("█", 11));
    }
    expected.push_back("   " + blocks("▄", 11));
    CHECK(harness.draw() == expected);
}

// Mutation: draw the reference line over the bar (the line's row then has no block in the bar's columns).
TEST_CASE("tui chart: a reference line leaves the cells of a bar alone", "[tui][chart]") {
    Harness harness{30, 10};
    auto const chart = harness.make<ChartImpl>(nullptr);
    ui::ChartData data = bars();
    data.lines = {{.value = 4.0, .label = "", .tone = ui::Tone::Err, .style = ui::ChartLineStyle::Solid}};
    chart->setData(data);
    Rows const rows = harness.draw();
    REQUIRE(rows.size() == 10);
    CHECK(rows.at(5) == " ──" + blocks("█", 11) + blocks("─", 16));
}

// Mutation: put the cross on the tip's cell instead of the one beyond it, or leave the note out.
TEST_CASE("tui chart: a marked bar has the cross beyond its tip and its note after the bar", "[tui][chart]") {
    Harness harness{30, 10};
    auto const chart = harness.make<ChartImpl>(nullptr);
    ui::ChartData data = bars();
    data.series.front().points.at(1).tone = ui::Tone::Err;
    data.series.front().points.at(1).note = "n";
    chart->setData(data);
    Rows const rows = harness.draw();
    REQUIRE(rows.size() == 10);
    CHECK(rows.at(5) == "   " + blocks("█", 11) + std::string(8, ' ') + "✕" + std::string(6, ' ') + "n");
    CHECK(rows.at(6) == "   " + blocks("█", 11) + "   " + blocks("▃", 11));
}

// Mutation: let a series with no value at a position widen the other's bar, or drop the legend's block.
TEST_CASE("tui chart: several bars series stand side by side, a gap keeps its series' place, the legend shows a block",
          "[tui][chart]") {
    Harness harness{30, 10};
    auto const chart = harness.make<ChartImpl>(nullptr);
    ui::ChartData data = bars();
    data.series = {{.label = "a",
                    .joined = false,
                    .style = ui::ChartStyle::Bars,
                    .points = {{.x = 0.0, .y = 8.0}, {.x = 1.0, .y = 2.5}}},
                   {.label = "b",
                    .joined = false,
                    .style = ui::ChartStyle::Bars,
                    .points = {{.x = 0.0, .y = std::nullopt}, {.x = 1.0, .y = 4.0}}}};
    chart->setData(data);
    Rows const rows = harness.draw();
    REQUIRE(rows.size() == 10);
    // Two series take five columns each: a at 3 to 7 and 17 to 21, b at 22 to 26; b's empty place at 8 to 12 stays.
    CHECK(rows.at(8) == "   " + blocks("█", 5) + std::string(9, ' ') + blocks("█", 10));
    CHECK(rows.at(9) == " █ a  █ b");
}

// Mutation: put a zero-height bar's cross on the plot's bottom row (extent 0 falling back to cell 0).
TEST_CASE("tui chart: a marked bar of the baseline's value has its cross and note beside the baseline",
          "[tui][chart]") {
    Harness harness{30, 10};
    auto const chart = harness.make<ChartImpl>(nullptr);
    ui::ChartData data = bars();
    data.yMin = -4.0;
    data.yMax = 4.0;
    data.series.front().points = {{.x = 0.0, .y = 0.0, .tone = ui::Tone::Err, .note = "z"}, {.x = 1.0, .y = -1.0}};
    chart->setData(data);
    Rows const rows = harness.draw();
    REQUIRE(rows.size() >= 5);
    // The baseline is on the boundary 9 * 4 / 8 + 1/2 = 5 rows up, so the cross is in cell 5, which is row 4, on the
    // bar's middle column 8, and its note two columns after the bar's last column 13.
    CHECK(rows.at(4) == std::string(8, ' ') + "✕" + std::string(6, ' ') + "z");
}

// Mutation: draw the bars in series order (the taller bar of the later series then hides the shorter one's end block).
TEST_CASE("tui chart: where two bars share a column the shorter one is in front", "[tui][chart]") {
    Harness harness{30, 10};
    auto const chart = harness.make<ChartImpl>(nullptr);
    ui::ChartData data = bars();
    // A slot of 0.56 columns gives each of the two series one column; the positions 0 and 0.04 are columns 8 and 9,
    // so b at the first position and a at the second both stand in column 8.
    data.barSlot = 0.04;
    data.series = {{.label = "a",
                    .joined = false,
                    .style = ui::ChartStyle::Bars,
                    .points = {{.x = 0.0, .y = 1.0}, {.x = 0.04, .y = 3.0}}},
                   {.label = "b",
                    .joined = false,
                    .style = ui::ChartStyle::Bars,
                    .points = {{.x = 0.0, .y = 8.0}, {.x = 0.04, .y = 1.0}}}};
    chart->setData(data);
    Rows const rows = harness.draw();
    REQUIRE(rows.size() == 10);
    // The legend takes a row, so the plot is nine rows: 3 is 28 eighths (three cells and a half block), 8 is 68.
    CHECK(rows.at(5) == std::string(8, ' ') + "▄");
    CHECK(rows.at(6) == std::string(8, ' ') + "█");
    CHECK(rows.at(7) == std::string(7, ' ') + "▄█▄");
}
