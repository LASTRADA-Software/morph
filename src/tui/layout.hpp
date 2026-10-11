// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Rect.hpp>
#include <morph/ui/view.hpp>
#include <span>
#include <vector>

/// The stack and grid solver the TUI containers arrange their children with, in terminal cells. Specified in
/// docs/spec/tui/frontend.md, "Layout".
///
/// The solver lays out every item and cell it is given; which children those are is the caller's choice. A caller
/// that keeps a hidden child's place passes it (an Item as Fixed 0, a GridCell of natural height 0): it still takes
/// its slot and the gaps beside it. One that lets a hidden child take no space leaves it out.
///
/// Negative extents, amounts, gaps and padding count as zero, and the arithmetic does not overflow: an edge or an
/// extent past the int range saturates at it. `planGrid` keeps one width per column, so its column count is bounded
/// by memory rather than by the int range.
namespace morph::tui::layout {

/// One child along a stack's main axis.
struct Item {
    ui::Sizing sizing{};  ///< How the child asks for space.
    int natural = 0;      ///< Its content extent along the axis.
};

/// The space along one axis and the gap between two neighbours on it.
struct Track {
    int length = 0;  ///< Cells available.
    int gap = 0;     ///< Cells between two neighbours.
};

/// Extents along a stack's main axis, in item order. Fixed items get their amount and Content items their natural
/// extent; Stretch items share what is left by weight (a weight below one counts as one), the cells a division
/// leaves over going one each to the first Stretch items; an overflow is taken from the last item first, down to
/// zero. Gaps are never shrunk, so items whose gaps alone overflow the track all get zero.
[[nodiscard]] std::vector<int> distribute(std::span<Item const> items, Track track);

/// A child's extent across a stack: a Fixed child's amount, at most `space`; Content and Stretch fill `space`.
[[nodiscard]] int crossExtent(ui::Sizing sizing, int space);

/// What one grid cell asks for.
struct GridCell {
    int span = 1;           ///< Columns it covers; clamped to [1, columns].
    int naturalHeight = 1;  ///< Its content height.
    ui::Sizing height{};    ///< Fixed overrides the natural height; Content and Stretch use it.
};

/// Where one cell lands.
struct GridSlot {
    int column = 0;  ///< First column.
    int row = 0;     ///< Row.
    int span = 1;    ///< Columns covered.

    bool operator==(GridSlot const&) const = default;
};

/// A solved grid: one slot per cell, in cell order, and the geometry of its columns and rows.
struct GridPlan {
    std::vector<GridSlot> slots;    ///< One per cell.
    std::vector<int> columnX;       ///< Left edge of each column.
    std::vector<int> columnWidths;  ///< Width of each column.
    std::vector<int> rowY;          ///< Top edge of each row.
    std::vector<int> rowHeights;    ///< Height of each row.
    int gap = 0;                    ///< Cells between columns and between rows; never negative.
};

/// Places cells row-major in `columns` equal columns (fewer than one counts as one). A cell whose span does not fit
/// the rest of its row starts the next row; a row is as tall as its tallest cell.
[[nodiscard]] GridPlan planGrid(int columns, std::span<GridCell const> cells, Track width);

/// The rectangle a slot of `plan` covers, gaps between its columns included.
[[nodiscard]] ::core::tui::Rect slotArea(GridPlan const& plan, GridSlot const& slot);

/// `area` shrunk by `padding` cells on every side; never negative.
[[nodiscard]] ::core::tui::Rect pad(::core::tui::Rect area, int padding);

}  // namespace morph::tui::layout
