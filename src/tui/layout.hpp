// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Rect.hpp>
#include <limits>
#include <morph/ui/view.hpp>
#include <optional>
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
    ui::Sizing sizing{};                            ///< How the child asks for space.
    int natural = 0;                                ///< Its content extent along the axis.
    int minimum = 0;                                ///< The least extent it takes.
    int maximum = std::numeric_limits<int>::max();  ///< The most extent it takes.
};

/// The bounds of one dimension: what an `Item` and `crossExtent` clamp into. A maximum below the minimum counts as
/// the minimum.
struct Bounds {
    int minimum = 0;                                ///< The least extent.
    int maximum = std::numeric_limits<int>::max();  ///< The most extent.
};

/// A dimension's bounds from a widget's layout hints; none set is unbounded.
[[nodiscard]] Bounds boundsOf(std::optional<int> minimum, std::optional<int> maximum) noexcept;

/// The space along one axis and the gap between two neighbours on it.
struct Track {
    int length = 0;  ///< Cells available.
    int gap = 0;     ///< Cells between two neighbours.
};

/// Extents along a stack's main axis, in item order. Fixed items get their amount and Content items their natural
/// extent, each clamped into its bounds; Stretch items share what is left by weight (a weight below one counts as
/// one), the cells a division leaves over going one each to the first Stretch items. A share stops at its item's
/// maximum and what it leaves goes to the Stretch items not capped, at most one pass per Stretch item; a share
/// below its item's minimum is raised to it. An overflow is taken from the last item first, down to each item's
/// minimum, and then the stack overflows. Gaps are never shrunk, so items whose gaps alone overflow the track keep
/// only their minimums.
[[nodiscard]] std::vector<int> distribute(std::span<Item const> items, Track track);

/// A child's extent across a stack: a Fixed child's amount, while Content and Stretch fill `space`; then clamped
/// into `bounds`, then into `space`.
[[nodiscard]] int crossExtent(ui::Sizing sizing, int space, Bounds bounds = {});

/// What one grid cell asks for.
struct GridCell {
    int span = 1;           ///< Columns it covers; clamped to [1, columns].
    int naturalHeight = 1;  ///< Its content height.
    ui::Sizing height{};    ///< Fixed overrides the natural height; Content and Stretch use it.
    Bounds bounds{};        ///< The height's bounds, which the requested height is clamped into.
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
    int columnGap = 0;              ///< Cells between columns; never negative.
    int rowGap = 0;                 ///< Rows between rows; never negative.
};

/// Places cells row-major in `columns` equal columns (fewer than one counts as one), `width.gap` columns apart and
/// `rowGap` rows apart. A cell whose span does not fit the rest of its row starts the next row; a row is as tall as
/// its tallest cell.
[[nodiscard]] GridPlan planGrid(int columns, std::span<GridCell const> cells, Track width, int rowGap);

/// As `planGrid` with a column count, with one `Item` per column sizing it as `distribute` sizes a stack's items;
/// no item counts as one stretch column.
[[nodiscard]] GridPlan planGrid(std::span<Item const> columns, std::span<GridCell const> cells, Track width,
                                int rowGap);

/// Where one item of a wrapping row lands.
struct WrapPlaced {
    int line = 0;   ///< The line, from 0.
    int x = 0;      ///< Its leading edge along the line.
    int width = 0;  ///< Its extent along the line.

    bool operator==(WrapPlaced const&) const = default;
};

/// A solved wrapping row: one place per item, in item order, and the number of lines.
struct WrapPlan {
    std::vector<WrapPlaced> placed;  ///< One per item.
    int lines = 0;                   ///< How many lines the items fill.
};

/// Places items along lines of `width.length` cells, `width.gap` cells apart. Each item has a basis: its natural
/// extent (a Fixed item its amount), and for a Stretch item its minimum when it has one (above 0), else its natural
/// extent; clamped into its bounds and at most the line's length. Items fill lines in order; an item starts a new line
/// when its basis, and a gap, does not fit after the items already on the line. A line holds at least one item, and an
/// item whose basis is 0 takes no gap and starts no line. On a line the Stretch items share the spare length as
/// `distribute` does; a line with none places it by `justify`. An item wider than the line sits alone, at the line's
/// length.
[[nodiscard]] WrapPlan planWrap(std::span<Item const> items, Track width, ui::Justify justify = ui::Justify::Start);

/// The rectangle a slot of `plan` covers, gaps between its columns included.
[[nodiscard]] ::core::tui::Rect slotArea(GridPlan const& plan, GridSlot const& slot);

/// `area` shrunk by `vertical` rows at the top and bottom and `horizontal` columns at the left and right; never
/// negative.
[[nodiscard]] ::core::tui::Rect pad(::core::tui::Rect area, int vertical, int horizontal);

/// The cells a spacing step (`ui::spacingStep`) is along `axis`. A cell is about twice as tall as it is wide, so a
/// step is about half as many rows as columns: vertically 0, 0, 0, 1, 1, 2 and horizontally 0, 1, 1, 2, 2, 3 for
/// steps 0 to 5. A step outside 0 to 5 is the nearest end.
[[nodiscard]] int stepCells(int step, ui::Axis axis) noexcept;

}  // namespace morph::tui::layout
