// SPDX-License-Identifier: Apache-2.0

#include "tui/layout.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <ranges>

namespace morph::tui::layout {

namespace {

// Sums and products of cell counts run in 64 bits: a weight, a gap or a Fixed amount may be anything an int holds,
// and the products and sums of several of them do not fit one.
using Wide = std::int64_t;

/// `value` clamped into the range of an int; an edge that far out is off any screen.
int saturate(Wide value) noexcept {
    return static_cast<int>(std::clamp<Wide>(value, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
}

bool isStretch(ui::Sizing const& sizing) noexcept { return sizing.kind == ui::Sizing::Kind::Stretch; }

Wide weightOf(ui::Sizing const& sizing) noexcept { return std::max(1, sizing.amount); }

/// What an item that does not stretch asks for.
int requestOf(Item const& item) noexcept {
    return std::max(0, item.sizing.kind == ui::Sizing::Kind::Fixed ? item.sizing.amount : item.natural);
}

Wide gapsBetween(std::size_t count, int gap) noexcept {
    return count == 0 ? 0 : Wide{gap} * (static_cast<Wide>(count) - 1);
}

/// Shares `spare` among the Stretch items of `items` by weight.
void shareSpare(std::span<Item const> items, std::vector<int>& sizes, Wide spare, Wide weights) {
    Wide given = 0;
    auto size = sizes.begin();
    for (auto const& item : items) {
        if (isStretch(item.sizing)) {
            // At most `spare`, which is at most the track's length: it fits an int.
            *size = static_cast<int>(spare * weightOf(item.sizing) / weights);
            given += *size;
        }
        ++size;
    }
    // Each share is rounded down by less than one cell, so fewer cells are left over than there are Stretch items:
    // one pass hands them out.
    Wide remainder = spare - given;
    size = sizes.begin();
    for (auto const& item : items) {
        if (remainder == 0) {
            break;
        }
        if (isStretch(item.sizing)) {
            ++*size;
            --remainder;
        }
        ++size;
    }
}

/// Takes `deficit` cells off `sizes`, from the last one towards the first.
void shrinkFromLast(std::vector<int>& sizes, Wide deficit) {
    for (auto& size : sizes | std::views::reverse) {
        auto const take = std::min(Wide{size}, deficit);
        size -= static_cast<int>(take);
        deficit -= take;
    }
}

/// The leading edge of each of `extents` laid end to end with `gap` between them.
std::vector<int> edgesOf(std::span<int const> extents, int gap) {
    std::vector<int> edges;
    edges.reserve(extents.size());
    Wide position = 0;
    for (int const extent : extents) {
        edges.push_back(saturate(position));
        position += Wide{extent} + gap;
    }
    return edges;
}

int heightOf(GridCell const& cell) noexcept {
    return std::max(0, cell.height.kind == ui::Sizing::Kind::Fixed ? cell.height.amount : cell.naturalHeight);
}

}  // namespace

std::vector<int> distribute(std::span<Item const> items, Track track) {
    std::vector<int> sizes;
    sizes.reserve(items.size());
    Wide used = 0;
    Wide weights = 0;
    for (auto const& item : items) {
        if (isStretch(item.sizing)) {
            weights += weightOf(item.sizing);
            sizes.push_back(0);
            continue;
        }
        sizes.push_back(requestOf(item));
        used += sizes.back();
    }
    Wide const spare = Wide{track.length} - gapsBetween(items.size(), std::max(0, track.gap)) - used;
    if (spare > 0 && weights > 0) {
        shareSpare(items, sizes, spare, weights);
    } else if (spare < 0) {
        shrinkFromLast(sizes, -spare);
    }
    return sizes;
}

int crossExtent(ui::Sizing sizing, int space) {
    if (sizing.kind == ui::Sizing::Kind::Fixed) {
        return std::clamp(sizing.amount, 0, std::max(0, space));
    }
    return std::max(0, space);
}

GridPlan planGrid(int columns, std::span<GridCell const> cells, Track width) {
    int const count = std::max(1, columns);
    GridPlan plan;
    plan.gap = std::max(0, width.gap);
    std::vector<Item> const equal(static_cast<std::size_t>(count),
                                  Item{.sizing = ui::Sizing::stretch(1), .natural = 0});
    plan.columnWidths = distribute(equal, Track{.length = width.length, .gap = plan.gap});
    plan.columnX = edgesOf(plan.columnWidths, plan.gap);

    int column = 0;
    int row = 0;
    for (auto const& cell : cells) {
        int const span = std::clamp(cell.span, 1, count);
        // Compared as a difference: `column + span` overflows for a column count near the int range.
        bool const wraps = span > count - column;
        if (wraps) {
            ++row;
            column = 0;
        }
        plan.slots.push_back(GridSlot{.column = column, .row = row, .span = span});
        if (plan.rowHeights.empty() || wraps) {
            plan.rowHeights.push_back(heightOf(cell));
        } else {
            plan.rowHeights.back() = std::max(plan.rowHeights.back(), heightOf(cell));
        }
        column += span;
    }
    plan.rowY = edgesOf(plan.rowHeights, plan.gap);
    return plan;
}

::core::tui::Rect slotArea(GridPlan const& plan, GridSlot const& slot) {
    auto const first = static_cast<std::size_t>(slot.column);
    auto const spanned =
        plan.columnWidths | std::views::drop(first) | std::views::take(static_cast<std::size_t>(slot.span));
    Wide const width =
        std::accumulate(spanned.begin(), spanned.end(), Wide{0}) + (Wide{plan.gap} * std::max(0, slot.span - 1));
    auto const row = static_cast<std::size_t>(slot.row);
    return ::core::tui::Rect{.x = plan.columnX.at(first),
                             .y = plan.rowY.at(row),
                             .width = saturate(width),
                             .height = plan.rowHeights.at(row)};
}

::core::tui::Rect pad(::core::tui::Rect area, int padding) {
    Wide const inset = std::max(0, padding);
    return ::core::tui::Rect{.x = saturate(area.x + inset),
                             .y = saturate(area.y + inset),
                             .width = saturate(std::max<Wide>(0, area.width - (2 * inset))),
                             .height = saturate(std::max<Wide>(0, area.height - (2 * inset)))};
}

}  // namespace morph::tui::layout
