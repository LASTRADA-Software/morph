// SPDX-License-Identifier: Apache-2.0

// How a table's stretch columns share the width left to them: the four steps of the rule, where they meet, and what
// holds on any input.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <morph/ui/stretch_columns.hpp>
#include <numeric>
#include <random>
#include <vector>

namespace ui = morph::ui;
using ui::StretchColumn;

namespace {

StretchColumn column(int natural, int least, int floor, int weight = 1, bool shrink = false) {
    return StretchColumn{.natural = natural, .least = least, .floor = floor, .weight = weight, .shrink = shrink};
}

int sum(std::vector<int> const& widths) { return std::accumulate(widths.begin(), widths.end(), 0); }

}  // namespace

// Mutation: ignore `natural` in step 1 (the columns share the whole room by weight, [8, 22]).
TEST_CASE("stretch columns: with room to spare each is its natural width plus a weighted share of the spare",
          "[ui][stretch]") {
    std::vector<StretchColumn> const columns{column(10, 10, 4, 1), column(10, 10, 4, 3)};
    CHECK(ui::stretchWidths(columns, 30) ==
          std::vector<int>{13, 17});  // 2.5 and 7.5 of the spare 10: the odd unit to the first
    CHECK(ui::stretchWidths(columns, 20) == std::vector<int>{10, 10});
}

// Mutation: drop step 2 (a room under sum(natural) goes to step 3: [18, 12] becomes [12, 12] with a deficit taken from
// the last column).
TEST_CASE("stretch columns: under their natural widths the columns narrow between their least and natural widths",
          "[ui][stretch]") {
    std::vector<StretchColumn> const columns{column(30, 8, 4), column(12, 12, 4)};
    CHECK(ui::stretchWidths(columns, 30) == std::vector<int>{18, 12});
    // Weights take no part: a heavier column that cannot narrow stays at its natural width.
    std::vector<StretchColumn> const weighted{column(30, 8, 4, 1), column(12, 12, 4, 9)};
    CHECK(ui::stretchWidths(weighted, 30) == std::vector<int>{18, 12});
}

// Mutation: take the deficit from the first column first.
TEST_CASE("stretch columns: under their least widths the deficit comes from the shrinking columns, last first",
          "[ui][stretch]") {
    std::vector<StretchColumn> const three{column(12, 12, 4), column(12, 12, 4, 1, true), column(12, 12, 4, 1, true)};
    CHECK(ui::stretchWidths(three, 30) == std::vector<int>{12, 12, 6});
    std::vector<StretchColumn> const two{column(12, 12, 4, 1, true), column(12, 12, 4)};
    CHECK(ui::stretchWidths(two, 20) == std::vector<int>{8, 12});
    // A shrinking column gives up to its floor, then the others give, last first.
    std::vector<StretchColumn> const spent{column(12, 12, 10, 1, true), column(12, 12, 4)};
    CHECK(ui::stretchWidths(spent, 17) == std::vector<int>{10, 7});
}

// Mutation: leave the floors out of step 4 (each column keeps its least width).
TEST_CASE("stretch columns: under the sum of the floors every column is at its floor", "[ui][stretch]") {
    std::vector<StretchColumn> const columns{column(20, 20, 10), column(20, 20, 10)};
    CHECK(ui::stretchWidths(columns, 15) == std::vector<int>{10, 10});
    CHECK(ui::stretchWidths(columns, 0) == std::vector<int>{10, 10});
    CHECK(ui::stretchWidths(columns, -5) == std::vector<int>{10, 10});
}

TEST_CASE("stretch columns: a width never falls as the room grows, and the steps meet without a jump",
          "[ui][stretch]") {
    std::vector<StretchColumn> const columns{column(30, 8, 4, 2), column(12, 12, 4, 1, true), column(20, 5, 3, 1)};
    std::vector<int> before = ui::stretchWidths(columns, 0);
    for (int room = 1; room <= 100; ++room) {
        std::vector<int> const now = ui::stretchWidths(columns, room);
        for (std::size_t at = 0; at < columns.size(); ++at) {
            INFO("room " << room << ", column " << at);
            CHECK(now[at] >= before[at]);
        }
        // Past the floors the columns add up to the room exactly, in the first three steps.
        if (room >= 4 + 4 + 3) {
            CHECK(sum(now) == room);
        }
        before = now;
    }
}

TEST_CASE("stretch columns: no columns, and a column whose widths are out of order", "[ui][stretch]") {
    CHECK(ui::stretchWidths({}, 10).empty());
    // `least` above `natural` and `natural` under `floor` are clamped into order.
    std::vector<StretchColumn> const columns{column(5, 9, 7)};
    CHECK(ui::stretchWidths(columns, 3) == std::vector<int>{7});
    CHECK(ui::stretchWidths(columns, 20) == std::vector<int>{20});
}

TEST_CASE("stretch columns: random input stays within its bounds and fills the room", "[ui][stretch]") {
    // NOLINTNEXTLINE(bugprone-random-generator-seed, cert-msc32-c, cert-msc51-cpp): the fixed seed 7 replays the same sequence on every run, so a failing case reproduces
    std::mt19937 random{7};
    for (int round = 0; round < 500; ++round) {
        std::vector<StretchColumn> columns;
        int floors = 0;
        int leasts = 0;
        auto const count = std::uniform_int_distribution<int>{1, 6}(random);
        for (int at = 0; at < count; ++at) {
            auto const floor = std::uniform_int_distribution<int>{0, 10}(random);
            auto const least = floor + std::uniform_int_distribution<int>{0, 10}(random);
            auto const natural = least + std::uniform_int_distribution<int>{0, 20}(random);
            columns.push_back(column(natural, least, floor, std::uniform_int_distribution<int>{1, 4}(random),
                                     std::uniform_int_distribution<int>{0, 1}(random) == 1));
            floors += floor;
            leasts += least;
        }
        auto const room = std::uniform_int_distribution<int>{0, 120}(random);
        std::vector<int> const widths = ui::stretchWidths(columns, room);
        for (std::size_t at = 0; at < widths.size(); ++at) {
            CHECK(widths[at] >= columns[at].floor);
        }
        if (room >= floors) {
            CHECK(sum(widths) == room);
        }
        if (room < leasts) {
            for (std::size_t at = 0; at < widths.size(); ++at) {
                CHECK(widths[at] <= columns[at].least);
            }
        }
    }
}
