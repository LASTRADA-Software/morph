// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <core/tui/Rect.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <morph/ui/view.hpp>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tui/layout.hpp"

namespace layout = morph::tui::layout;
using morph::ui::Sizing;

namespace {

struct DistributeCase {
    std::string_view name;
    std::vector<layout::Item> items;
    layout::Track track;
    std::vector<int> expected;
};

struct GridCase {
    std::string_view name;
    int columns = 1;
    std::vector<layout::GridCell> cells;
    layout::Track width;
    std::vector<layout::GridSlot> slots;
    std::vector<int> rowHeights;
};

constexpr int kMaxInt = std::numeric_limits<int>::max();

}  // namespace

TEST_CASE("tui::layout::distribute", "[tui][layout]") {
    auto const testCase = GENERATE(values<DistributeCase>({
        {"fixed, content and stretch",
         {{Sizing::fixed(3), 0}, {Sizing::content(), 2}, {Sizing::stretch(1), 9}},
         {.length = 10, .gap = 0},
         {3, 2, 5}},
        {"equal weights: the remainder goes to the first",
         {{Sizing::stretch(1), 0}, {Sizing::stretch(1), 0}, {Sizing::stretch(1), 0}},
         {.length = 10, .gap = 0},
         {4, 3, 3}},
        {"weights and a gap", {{Sizing::stretch(2), 0}, {Sizing::stretch(1), 0}}, {.length = 10, .gap = 1}, {6, 3}},
        {"uneven weights: the remainder still goes to the first",
         {{Sizing::stretch(1), 0}, {Sizing::stretch(2), 0}},
         {.length = 10, .gap = 0},
         {4, 6}},
        {"overflow shrinks the last item first",
         {{Sizing::fixed(6), 0}, {Sizing::fixed(6), 0}},
         {.length = 10, .gap = 0},
         {6, 4}},
        {"overflow past one item reaches the one before",
         {{Sizing::content(), 3}, {Sizing::fixed(4), 0}, {Sizing::content(), 5}},
         {.length = 8, .gap = 1},
         {3, 3, 0}},
        {"no items", {}, {.length = 10, .gap = 0}, {}},
        {"stretch into nothing", {{Sizing::stretch(1), 4}}, {.length = 0, .gap = 0}, {0}},
        {"a negative fixed size is zero", {{Sizing::fixed(-2), 0}}, {.length = 5, .gap = 0}, {0}},
        {"a zero weight counts as one",
         {{Sizing::stretch(0), 0}, {Sizing::stretch(1), 0}},
         {.length = 4, .gap = 0},
         {2, 2}},
    }));
    INFO(testCase.name);
    CHECK(layout::distribute(testCase.items, testCase.track) == testCase.expected);
}

TEST_CASE("tui::layout::crossExtent", "[tui][layout]") {
    CHECK(layout::crossExtent(Sizing::fixed(4), 10) == 4);
    CHECK(layout::crossExtent(Sizing::fixed(12), 10) == 10);
    CHECK(layout::crossExtent(Sizing::fixed(-1), 10) == 0);
    CHECK(layout::crossExtent(Sizing::content(), 10) == 10);
    CHECK(layout::crossExtent(Sizing::stretch(3), 10) == 10);
}

TEST_CASE("tui::layout::planGrid", "[tui][layout]") {
    auto const testCase = GENERATE(values<GridCase>({
        {"two columns, a full-width third cell",
         2,
         {{.span = 1, .naturalHeight = 1, .height = {}},
          {.span = 1, .naturalHeight = 1, .height = {}},
          {.span = 2, .naturalHeight = 1, .height = {}}},
         {.length = 21, .gap = 1},
         {{.column = 0, .row = 0, .span = 1}, {.column = 1, .row = 0, .span = 1}, {.column = 0, .row = 1, .span = 2}},
         {1, 1}},
        {"a span that does not fit the rest of the row starts the next",
         2,
         {{.span = 1, .naturalHeight = 1, .height = {}}, {.span = 2, .naturalHeight = 1, .height = {}}},
         {.length = 20, .gap = 0},
         {{.column = 0, .row = 0, .span = 1}, {.column = 0, .row = 1, .span = 2}},
         {1, 1}},
        {"a span wider than the grid is clamped",
         3,
         {{.span = 5, .naturalHeight = 2, .height = {}}},
         {.length = 9, .gap = 0},
         {{.column = 0, .row = 0, .span = 3}},
         {2}},
        {"a row is as tall as its tallest cell; Fixed height wins over natural",
         2,
         {{.span = 1, .naturalHeight = 1, .height = {}},
          {.span = 1, .naturalHeight = 3, .height = {}},
          {.span = 1, .naturalHeight = 5, .height = Sizing::fixed(2)}},
         {.length = 10, .gap = 0},
         {{.column = 0, .row = 0, .span = 1}, {.column = 1, .row = 0, .span = 1}, {.column = 0, .row = 1, .span = 1}},
         {3, 2}},
        {"zero columns are one",
         0,
         {{.span = 1, .naturalHeight = 1, .height = {}}},
         {.length = 4, .gap = 0},
         {{.column = 0, .row = 0, .span = 1}},
         {1}},
    }));
    INFO(testCase.name);
    auto const plan = layout::planGrid(testCase.columns, testCase.cells, testCase.width);
    CHECK(plan.slots == testCase.slots);
    CHECK(plan.rowHeights == testCase.rowHeights);
}

TEST_CASE("tui::layout::slotArea spans columns and the gaps between them", "[tui][layout]") {
    std::vector<layout::GridCell> const cells{{.span = 1, .naturalHeight = 1, .height = {}},
                                              {.span = 1, .naturalHeight = 1, .height = {}},
                                              {.span = 2, .naturalHeight = 1, .height = {}}};
    auto const plan = layout::planGrid(2, cells, {.length = 21, .gap = 1});
    CHECK(plan.columnX == std::vector<int>{0, 11});
    CHECK(plan.columnWidths == std::vector<int>{10, 10});
    CHECK(plan.rowY == std::vector<int>{0, 2});
    CHECK(layout::slotArea(plan, plan.slots.at(1)) == core::tui::Rect{.x = 11, .y = 0, .width = 10, .height = 1});
    CHECK(layout::slotArea(plan, plan.slots.at(2)) == core::tui::Rect{.x = 0, .y = 2, .width = 21, .height = 1});
}

TEST_CASE("tui::layout::pad", "[tui][layout]") {
    CHECK(layout::pad({.x = 0, .y = 0, .width = 10, .height = 6}, 1) ==
          core::tui::Rect{.x = 1, .y = 1, .width = 8, .height = 4});
    CHECK(layout::pad({.x = 2, .y = 2, .width = 3, .height = 3}, 2) ==
          core::tui::Rect{.x = 4, .y = 4, .width = 0, .height = 0});
}

TEST_CASE("tui::layout::distribute at the edges", "[tui][layout]") {
    auto const testCase = GENERATE(values<DistributeCase>({
        {"overflow gives Stretch nothing and shrinks the last Fixed",
         {{Sizing::stretch(1), 0}, {Sizing::fixed(6), 0}, {Sizing::fixed(6), 0}},
         {.length = 10, .gap = 0},
         {0, 6, 4}},
        {"gaps alone overflow: every item is zero",
         {{Sizing::fixed(2), 0}, {Sizing::stretch(1), 0}, {Sizing::content(), 3}},
         {.length = 1, .gap = 1},
         {0, 0, 0}},
        {"a negative length is no space",
         {{Sizing::stretch(1), 0}, {Sizing::fixed(3), 0}},
         {.length = -4, .gap = 0},
         {0, 0}},
        {"a negative gap is zero",
         {{Sizing::stretch(1), 0}, {Sizing::stretch(1), 0}},
         {.length = 10, .gap = -2},
         {5, 5}},
        {"a negative natural extent is zero",
         {{Sizing::content(), -3}, {Sizing::stretch(1), 0}},
         {.length = 4, .gap = 0},
         {0, 4}},
        {"the remainder skips items that do not stretch",
         {{Sizing::stretch(1), 0}, {Sizing::fixed(2), 0}, {Sizing::stretch(1), 0}, {Sizing::stretch(1), 0}},
         {.length = 7, .gap = 0},
         {2, 2, 2, 1}},
        {"a zero-extent item still takes its gaps: a reserved slot",
         {{Sizing::stretch(1), 0}, {Sizing::fixed(0), 0}, {Sizing::stretch(1), 0}},
         {.length = 10, .gap = 1},
         {4, 0, 4}},
        {"huge weights do not overflow",
         {{Sizing::stretch(kMaxInt), 0}, {Sizing::stretch(kMaxInt), 0}},
         {.length = 11, .gap = 0},
         {6, 5}},
        {"a huge length times a weight does not overflow",
         {{Sizing::stretch(3), 0}, {Sizing::stretch(1), 0}},
         {.length = kMaxInt, .gap = 0},
         {1610612736, 536870911}},
        {"huge fixed sizes do not overflow",
         {{Sizing::fixed(kMaxInt), 0}, {Sizing::fixed(kMaxInt), 0}},
         {.length = 10, .gap = 0},
         {10, 0}},
        {"a huge gap does not overflow",
         {{Sizing::stretch(1), 0}, {Sizing::stretch(1), 0}, {Sizing::stretch(1), 0}},
         {.length = 10, .gap = kMaxInt},
         {0, 0, 0}},
    }));
    INFO(testCase.name);
    CHECK(layout::distribute(testCase.items, testCase.track) == testCase.expected);
}

TEST_CASE("tui::layout::distribute shares a huge count of items", "[tui][layout]") {
    std::vector<layout::Item> const items(10'000, layout::Item{.sizing = Sizing::stretch(1), .natural = 0});
    auto const sizes = layout::distribute(items, {.length = 25'000, .gap = 0});
    CHECK(std::ranges::count(sizes, 3) == 5'000);
    CHECK(std::all_of(sizes.begin(), sizes.begin() + 5'000, [](int size) { return size == 3; }));
    CHECK(std::all_of(sizes.begin() + 5'000, sizes.end(), [](int size) { return size == 2; }));
    std::vector<layout::Item> const spaced(50'000, layout::Item{.sizing = Sizing::stretch(1), .natural = 0});
    auto const squeezed = layout::distribute(spaced, {.length = 100, .gap = 50'000});
    CHECK(std::ranges::all_of(squeezed, [](int size) { return size == 0; }));
}

TEST_CASE("tui::layout::crossExtent of no space is zero", "[tui][layout]") {
    CHECK(layout::crossExtent(Sizing::fixed(4), -3) == 0);
    CHECK(layout::crossExtent(Sizing::content(), -3) == 0);
    CHECK(layout::crossExtent(Sizing::stretch(1), 0) == 0);
}

TEST_CASE("tui::layout::planGrid at the edges", "[tui][layout]") {
    auto const testCase = GENERATE(values<GridCase>({
        {"no cells", 2, {}, {.length = 10, .gap = 0}, {}, {}},
        {"a negative span is one",
         2,
         {{.span = -3, .naturalHeight = 1, .height = {}}, {.span = 1, .naturalHeight = 1, .height = {}}},
         {.length = 10, .gap = 0},
         {{.column = 0, .row = 0, .span = 1}, {.column = 1, .row = 0, .span = 1}},
         {1}},
        {"negative heights are zero",
         1,
         {{.span = 1, .naturalHeight = -2, .height = {}},
          {.span = 1, .naturalHeight = 4, .height = Sizing::fixed(-1)}},
         {.length = 10, .gap = 0},
         {{.column = 0, .row = 0, .span = 1}, {.column = 0, .row = 1, .span = 1}},
         {0, 0}},
        {"a row is as tall as its tallest cell, not its last",
         2,
         {{.span = 1, .naturalHeight = 3, .height = {}}, {.span = 1, .naturalHeight = 1, .height = {}}},
         {.length = 10, .gap = 0},
         {{.column = 0, .row = 0, .span = 1}, {.column = 1, .row = 0, .span = 1}},
         {3}},
        {"a zero-height cell keeps its column: a reserved slot",
         2,
         {{.span = 1, .naturalHeight = 0, .height = {}},
          {.span = 1, .naturalHeight = 2, .height = {}},
          {.span = 1, .naturalHeight = 1, .height = {}}},
         {.length = 10, .gap = 0},
         {{.column = 0, .row = 0, .span = 1}, {.column = 1, .row = 0, .span = 1}, {.column = 0, .row = 1, .span = 1}},
         {2, 1}},
    }));
    INFO(testCase.name);
    auto const plan = layout::planGrid(testCase.columns, testCase.cells, testCase.width);
    CHECK(plan.slots == testCase.slots);
    CHECK(plan.rowHeights == testCase.rowHeights);
}

TEST_CASE("tui::layout::planGrid geometry at the edges", "[tui][layout]") {
    SECTION("a negative gap is zero") {
        std::vector<layout::GridCell> const cells{{.span = 1, .naturalHeight = 1, .height = {}},
                                                  {.span = 1, .naturalHeight = 1, .height = {}},
                                                  {.span = 2, .naturalHeight = 1, .height = {}}};
        auto const plan = layout::planGrid(2, cells, {.length = 10, .gap = -3});
        CHECK(plan.gap == 0);
        CHECK(plan.columnX == std::vector<int>{0, 5});
        CHECK(plan.columnWidths == std::vector<int>{5, 5});
        CHECK(plan.rowY == std::vector<int>{0, 1});
        CHECK(layout::slotArea(plan, plan.slots.at(2)) == core::tui::Rect{.x = 0, .y = 1, .width = 10, .height = 1});
    }
    SECTION("columns narrower than their gaps are empty and still advance") {
        auto const plan = layout::planGrid(3, {}, {.length = 1, .gap = 1});
        CHECK(plan.columnWidths == std::vector<int>{0, 0, 0});
        CHECK(plan.columnX == std::vector<int>{0, 1, 2});
    }
    SECTION("edges past the int range saturate") {
        std::vector<layout::GridCell> const cells{{.span = 1, .naturalHeight = kMaxInt, .height = {}},
                                                  {.span = 1, .naturalHeight = kMaxInt, .height = {}},
                                                  {.span = 1, .naturalHeight = 1, .height = {}}};
        auto const plan = layout::planGrid(1, cells, {.length = 4, .gap = 1});
        CHECK(plan.rowY == std::vector<int>{0, kMaxInt, kMaxInt});
        std::vector<layout::GridCell> const spanning{{.span = 3, .naturalHeight = 1, .height = {}}};
        auto const wide = layout::planGrid(3, spanning, {.length = 4, .gap = kMaxInt});
        CHECK(wide.columnX == std::vector<int>{0, kMaxInt, kMaxInt});
        CHECK(layout::slotArea(wide, wide.slots.at(0)).width == kMaxInt);
    }
}

TEST_CASE("tui::layout::pad at the edges", "[tui][layout]") {
    CHECK(layout::pad({.x = 0, .y = 0, .width = 4, .height = 4}, -1) ==
          core::tui::Rect{.x = 0, .y = 0, .width = 4, .height = 4});
    CHECK(layout::pad({.x = 1, .y = 1, .width = 4, .height = 4}, kMaxInt) ==
          core::tui::Rect{.x = kMaxInt, .y = kMaxInt, .width = 0, .height = 0});
}

namespace {

/// A fixed sequence of draws, the same on every platform (the standard distributions are not), so a failing round
/// reproduces anywhere. SplitMix64.
class Draws {
public:
    /// A value in [low, high].
    int between(int low, int high) noexcept {
        _state += 0x9E3779B97F4A7C15U;
        std::uint64_t mixed = _state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9U;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBU;
        mixed ^= mixed >> 31U;
        auto const range = static_cast<std::uint64_t>(std::int64_t{high} - low + 1);
        return static_cast<int>(low + static_cast<std::int64_t>(mixed % range));
    }

private:
    std::uint64_t _state = 0;
};

int requestOf(layout::Item const& item) {
    return std::max(0, item.sizing.kind == Sizing::Kind::Fixed ? item.sizing.amount : item.natural);
}

/// The first rule `sizes` breaks as what `distribute` gives `items` in `track`; empty when it keeps them all.
std::string distributeViolation(std::vector<layout::Item> const& items, layout::Track track,
                                std::vector<int> const& sizes) {
    if (sizes.size() != items.size()) {
        return "one size per item";
    }
    if (std::ranges::any_of(sizes, [](int size) { return size < 0; })) {
        return "no negative size";
    }
    auto const gaps = items.empty()
                          ? std::int64_t{0}
                          : std::int64_t{std::max(0, track.gap)} * (static_cast<std::int64_t>(items.size()) - 1);
    std::int64_t requested = 0;
    std::int64_t weights = 0;
    for (auto const& item : items) {
        if (item.sizing.kind == Sizing::Kind::Stretch) {
            weights += std::max(1, item.sizing.amount);
        } else {
            requested += requestOf(item);
        }
    }
    auto const total = std::accumulate(sizes.begin(), sizes.end(), std::int64_t{0});
    auto const room = std::max(std::int64_t{0}, std::int64_t{track.length} - gaps);
    bool const fits = requested <= room;
    if (fits && total != (weights > 0 ? room : requested)) {
        return "the sizes add up to the space when something stretches, else to the requests";
    }
    if (!fits && total != room) {
        return "an overflow shrinks the sizes to exactly the space the gaps leave";
    }
    bool shrunk = false;
    auto size = sizes.begin();
    for (auto const& item : items) {
        bool const stretches = item.sizing.kind == Sizing::Kind::Stretch;
        int const asked = stretches ? 0 : requestOf(item);
        if (fits && stretches) {
            auto const share = (room - requested) * std::max(1, item.sizing.amount) / weights;
            if (*size != share && *size != share + 1) {
                return "a Stretch item gets its weighted share, rounded down, or one cell more";
            }
        } else if (fits && *size != asked) {
            return "an item that does not stretch gets what it asks for when everything fits";
        } else if (!fits && (*size > asked || (shrunk && *size != 0))) {
            return "an overflow never grows an item and empties every item after the first it shrinks";
        }
        shrunk = shrunk || *size < asked;
        ++size;
    }
    return {};
}

/// The first rule `plan` breaks as what `planGrid` gives `cells` in `columns` across `width`; empty when it keeps
/// them all.
std::string gridViolation(int columns, std::vector<layout::GridCell> const& cells, layout::Track width,
                          layout::GridPlan const& plan) {
    int const used = std::max(1, columns);
    if (plan.slots.size() != cells.size() || plan.columnWidths.size() != static_cast<std::size_t>(used)) {
        return "one slot per cell and one width per column";
    }
    int const gaps = width.gap * (used - 1);
    if (width.length >= gaps &&
        std::accumulate(plan.columnWidths.begin(), plan.columnWidths.end(), 0) + gaps != width.length) {
        return "the columns and their gaps fill the width";
    }
    int nextColumn = 0;
    std::vector<int> tallest;
    auto cell = cells.begin();
    for (auto const& slot : plan.slots) {
        int const span = std::clamp(cell->span, 1, used);
        bool const wraps = !tallest.empty() && nextColumn + span > used;
        if (slot.span != span || slot.column != (wraps ? 0 : nextColumn) ||
            std::cmp_not_equal(slot.row, tallest.size() - (wraps || tallest.empty() ? 0 : 1))) {
            return "a cell goes right after the one before, or starts the next row when it does not fit";
        }
        if (wraps || tallest.empty()) {
            tallest.push_back(std::max(0, cell->naturalHeight));
        } else {
            tallest.back() = std::max({tallest.back(), cell->naturalHeight, 0});
        }
        nextColumn = slot.column + slot.span;
        ++cell;
    }
    if (plan.rowHeights != tallest) {
        return "a row is as tall as its tallest cell";
    }
    std::vector<int> edges;
    int edge = 0;
    for (int const height : tallest) {
        edges.push_back(edge);
        edge += height + width.gap;
    }
    if (plan.rowY != edges) {
        return "rows are stacked with the gap between them";
    }
    return {};
}

}  // namespace

TEST_CASE("tui::layout::distribute holds its invariants on random input", "[tui][layout]") {
    Draws draws;
    for (int round = 0; round < 5'000; ++round) {
        std::vector<layout::Item> items(static_cast<std::size_t>(draws.between(0, 8)));
        for (auto& item : items) {
            int const which = draws.between(0, 2);
            int const amount = draws.between(-3, 20);
            item.sizing = which == 0 ? Sizing::fixed(amount) : Sizing::stretch(amount);
            if (which == 1) {
                item.sizing = Sizing::content();
            }
            item.natural = draws.between(-3, 20);
        }
        layout::Track const track{.length = draws.between(-5, 80), .gap = draws.between(-2, 3)};
        INFO("round " << round << ", length " << track.length << ", gap " << track.gap);
        CHECK(distributeViolation(items, track, layout::distribute(items, track)).empty());
    }
}

TEST_CASE("tui::layout::planGrid holds its invariants on random input", "[tui][layout]") {
    Draws draws;
    for (int round = 0; round < 2'000; ++round) {
        int const columns = draws.between(-1, 6);
        std::vector<layout::GridCell> cells(static_cast<std::size_t>(draws.between(0, 12)));
        for (auto& cell : cells) {
            cell = {.span = draws.between(-1, 8), .naturalHeight = draws.between(-2, 5), .height = {}};
        }
        layout::Track const width{.length = draws.between(0, 60), .gap = draws.between(0, 2)};
        INFO("round " << round);
        CHECK(gridViolation(columns, cells, width, layout::planGrid(columns, cells, width)).empty());
    }
}
