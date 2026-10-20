// SPDX-License-Identifier: Apache-2.0

#include "tui/layout.hpp"

#include <algorithm>
#include <array>
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

int maximumOf(Bounds bounds) noexcept { return std::max(bounds.minimum, bounds.maximum); }

/// `extent` clamped into `bounds`.
int clampInto(int extent, Bounds bounds) noexcept {
    return std::clamp(extent, std::max(0, bounds.minimum), std::max(0, maximumOf(bounds)));
}

Bounds boundsOfItem(Item const& item) noexcept { return Bounds{.minimum = item.minimum, .maximum = item.maximum}; }

/// What an item that does not stretch asks for.
int requestOf(Item const& item) noexcept {
    return clampInto(std::max(0, item.sizing.kind == ui::Sizing::Kind::Fixed ? item.sizing.amount : item.natural),
                     boundsOfItem(item));
}

Wide gapsBetween(std::size_t count, int gap) noexcept {
    return count == 0 ? 0 : Wide{gap} * (static_cast<Wide>(count) - 1);
}

/// Shares `spare` by weight among the items `sharing` marks.
void shareSpare(std::span<Item const> items, std::vector<bool> const& sharing, std::vector<int>& sizes, Wide spare,
                Wide weights) {
    Wide given = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (sharing.at(index)) {
            // At most `spare`, which is at most the track's length: it fits an int.
            sizes.at(index) = static_cast<int>(spare * weightOf(items[index].sizing) / weights);
            given += sizes.at(index);
        }
    }
    // Each share is rounded down by less than one cell, so fewer cells are left over than there are sharing items:
    // one pass hands them out.
    Wide remainder = spare - given;
    for (std::size_t index = 0; index < items.size() && remainder > 0; ++index) {
        if (sharing.at(index)) {
            ++sizes.at(index);
            --remainder;
        }
    }
}

/// Shares `spare` among the Stretch items, each stopped at its maximum; what a capped item leaves goes to the
/// others. Each pass caps at least one item or settles, so there are at most as many passes as Stretch items.
void shareCapped(std::span<Item const> items, std::vector<int>& sizes, Wide spare) {
    std::vector<bool> sharing(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        sharing.at(index) = isStretch(items[index].sizing);
    }
    for (;;) {
        Wide weights = 0;
        for (std::size_t index = 0; index < items.size(); ++index) {
            weights += sharing.at(index) ? weightOf(items[index].sizing) : 0;
        }
        if (weights == 0 || spare <= 0) {
            return;
        }
        shareSpare(items, sharing, sizes, spare, weights);
        bool capped = false;
        for (std::size_t index = 0; index < items.size(); ++index) {
            int const maximum = maximumOf(boundsOfItem(items[index]));
            if (sharing.at(index) && sizes.at(index) > maximum) {
                sizes.at(index) = maximum;
                sharing.at(index) = false;
                spare -= maximum;
                capped = true;
            }
        }
        if (!capped) {
            return;
        }
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (sharing.at(index)) {
                sizes.at(index) = 0;
            }
        }
    }
}

/// Takes `deficit` cells off `sizes`, from the last one towards the first, none below its item's minimum.
void shrinkFromLast(std::span<Item const> items, std::vector<int>& sizes, Wide deficit) {
    for (std::size_t index = sizes.size(); index-- > 0 && deficit > 0;) {
        auto const take = std::min(Wide{sizes.at(index)} - std::max(0, items[index].minimum), deficit);
        if (take > 0) {
            sizes.at(index) -= static_cast<int>(take);
            deficit -= take;
        }
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
    return clampInto(
        std::max(0, cell.height.kind == ui::Sizing::Kind::Fixed ? cell.height.amount : cell.naturalHeight),
        cell.bounds);
}

}  // namespace

Bounds boundsOf(std::optional<int> minimum, std::optional<int> maximum) noexcept {
    return Bounds{.minimum = std::max(0, minimum.value_or(0)),
                  .maximum = maximum.value_or(std::numeric_limits<int>::max())};
}

std::vector<int> distribute(std::span<Item const> items, Track track) {
    std::vector<int> sizes;
    sizes.reserve(items.size());
    Wide used = 0;
    for (auto const& item : items) {
        sizes.push_back(isStretch(item.sizing) ? 0 : requestOf(item));
        used += sizes.back();
    }
    Wide const gaps = gapsBetween(items.size(), std::max(0, track.gap));
    shareCapped(items, sizes, Wide{track.length} - gaps - used);
    // A share below its minimum is raised to it; the overflow that makes is taken back from the last items first.
    Wide total = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        sizes.at(index) = std::max(sizes.at(index), std::max(0, items[index].minimum));
        total += sizes.at(index);
    }
    if (Wide const deficit = total + gaps - Wide{track.length}; deficit > 0) {
        shrinkFromLast(items, sizes, deficit);
    }
    return sizes;
}

int crossExtent(ui::Sizing sizing, int space, Bounds bounds) {
    int const wanted = sizing.kind == ui::Sizing::Kind::Fixed ? sizing.amount : space;
    return std::clamp(clampInto(wanted, bounds), 0, std::max(0, space));
}

GridPlan planGrid(int columns, std::span<GridCell const> cells, Track width, int rowGap) {
    int const count = std::max(1, columns);
    GridPlan plan;
    plan.columnGap = std::max(0, width.gap);
    plan.rowGap = std::max(0, rowGap);
    std::vector<Item> const equal(static_cast<std::size_t>(count),
                                  Item{.sizing = ui::Sizing::stretch(1), .natural = 0});
    plan.columnWidths = distribute(equal, Track{.length = width.length, .gap = plan.columnGap});
    plan.columnX = edgesOf(plan.columnWidths, plan.columnGap);

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
    plan.rowY = edgesOf(plan.rowHeights, plan.rowGap);
    return plan;
}

::core::tui::Rect slotArea(GridPlan const& plan, GridSlot const& slot) {
    auto const first = static_cast<std::size_t>(slot.column);
    auto const spanned =
        plan.columnWidths | std::views::drop(first) | std::views::take(static_cast<std::size_t>(slot.span));
    Wide const width =
        std::accumulate(spanned.begin(), spanned.end(), Wide{0}) + (Wide{plan.columnGap} * std::max(0, slot.span - 1));
    auto const row = static_cast<std::size_t>(slot.row);
    return ::core::tui::Rect{.x = plan.columnX.at(first),
                             .y = plan.rowY.at(row),
                             .width = saturate(width),
                             .height = plan.rowHeights.at(row)};
}

::core::tui::Rect pad(::core::tui::Rect area, int vertical, int horizontal) {
    Wide const rows = std::max(0, vertical);
    Wide const columns = std::max(0, horizontal);
    return ::core::tui::Rect{.x = saturate(area.x + columns),
                             .y = saturate(area.y + rows),
                             .width = saturate(std::max<Wide>(0, area.width - (2 * columns))),
                             .height = saturate(std::max<Wide>(0, area.height - (2 * rows)))};
}

int stepCells(int step, ui::Axis axis) noexcept {
    static constexpr std::array<int, ui::kMaxSpacingStep + 1> kRows{0, 0, 0, 1, 1, 2};
    static constexpr std::array<int, ui::kMaxSpacingStep + 1> kColumns{0, 1, 1, 2, 2, 3};
    auto const index = static_cast<std::size_t>(ui::spacingStep(step));
    return axis == ui::Axis::Vertical ? kRows.at(index) : kColumns.at(index);
}

}  // namespace morph::tui::layout
