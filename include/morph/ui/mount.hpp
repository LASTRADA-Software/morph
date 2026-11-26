// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../attributes.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/scope.hpp"
#include "../reactive/signal.hpp"
#include "../table/view_change.hpp"
#include "../ui/backend.hpp"
#include "stretch_columns.hpp"
#include "table_rows.hpp"
#include "value.hpp"
#include "view.hpp"

/// @file
/// @brief `morph::ui::Mounted`: builds a view tree's widgets through a backend once, and keeps them current through
///        reactive bindings.
///
/// Specified in `docs/spec/ui/view_tree.md`, "Mount".

namespace morph::ui {

/// @brief What a mount did to the rows of its tables mounted over a `TableSource`, for tests.
struct MountStats {
    /// @brief Sets of a cell's row signal that changed its record; each re-evaluates the cell.
    std::size_t cellBinds = 0;
    /// @brief Sets of a realized row's own signal that changed its record; each re-evaluates the row's own parts.
    std::size_t rowBinds = 0;
    /// @brief Cells built, by `TableSource::cellView` or `defaultCellView`.
    std::size_t cellsCreated = 0;
    /// @brief Cells bound to another row or column: a pooled cell taken, or a cell of a row slot given another row.
    std::size_t cellsRebound = 0;
    /// @brief Cells a pass left blank for the frame's budget, to be realized in a later turn.
    std::size_t placeholders = 0;
    /// @brief `TableRows` changes applied to the realized rows.
    std::size_t viewChanges = 0;
    /// @brief Of `viewChanges`, those that replaced the whole view.
    std::size_t viewResets = 0;
};

/// @brief Which of a table's dimensions are windowed.
struct Windowing {
    /// @brief Only the rows in or near view are realized.
    bool rows = false;
    /// @brief Only the columns in or near view are realized.
    bool columns = false;

    /// @brief Member-wise equality.
    /// @param other The windowing to compare with.
    /// @return `true` when both members match.
    [[nodiscard]] bool operator==(Windowing const& other) const = default;
};

/// @brief Which dimensions of a table are windowed: rows from `threshold` rows on, or always in server mode; columns
///        above `columnThreshold` visible columns; both above `cellThreshold` cells.
/// @param virtualization              The table's limits.
/// @param rows           The rows of the view.
/// @param visibleColumns The columns shown.
/// @param serverMode     Whether the rows come from a server's pages.
/// @return What is windowed.
[[nodiscard]] inline Windowing windowingOf(Virtualization const& virtualization, std::size_t rows,
                                           std::size_t visibleColumns, bool serverMode) {
    // rows × columns > cellThreshold, compared as a quotient so no product overflows.
    bool const overCells = visibleColumns != 0 && rows > virtualization.cellThreshold / visibleColumns;
    return Windowing{.rows = serverMode || rows >= virtualization.threshold || overCells,
                     .columns = visibleColumns > virtualization.columnThreshold || overCells};
}

/// @brief The widths of a table's visible columns, in layout order, and their running offsets.
///
/// Widths are kept in a Fenwick tree, so a width's change, a column's offset and the columns at a scroll position
/// each read O(log n) nodes; no call reads every column. `columnReads` counts the reads of single columns' widths
/// (`widthOf`), which is what a pass over the window does per column.
class ColumnExtents {
public:
    /// @brief Replaces every width.
    /// @param widths The visible columns' widths, in layout order; a negative width reads as 0.
    void assign(std::span<int const> widths) {
        _widths.assign(widths.begin(), widths.end());
        for (int& width : _widths) {
            width = std::max(width, 0);
        }
        _user.assign(_widths.size(), false);
        _tree.assign(_widths.size() + 1, 0);
        for (std::size_t at = 0; at < _widths.size(); ++at) {
            add(at, _widths[at]);
        }
    }

    /// @brief Grows a column to @p width; a measured width never narrows a column, nor changes one the user set.
    /// @param column The column's place in the layout.
    /// @param width  The width measured.
    /// @return Whether the width changed.
    bool grow(std::size_t column, int width) {
        if (column >= _widths.size() || _user[column] || width <= _widths[column]) {
            return false;
        }
        add(column, width - _widths[column]);
        _widths[column] = width;
        return true;
    }

    /// @brief Sets a width the mount computed, which may be narrower than the one before; a width the user set stays.
    /// @param column The column's place in the layout.
    /// @param width  The width; a negative width reads as 0.
    /// @return Whether the width changed.
    bool fit(std::size_t column, int width) {
        width = std::max(width, 0);
        if (column >= _widths.size() || _user[column] || width == _widths[column]) {
            return false;
        }
        add(column, width - _widths[column]);
        _widths[column] = width;
        return true;
    }

    /// @brief Sets a width the user chose; measurement no longer changes it.
    /// @param column The column's place in the layout.
    /// @param width  The width.
    void set(std::size_t column, int width) {
        if (column >= _widths.size()) {
            return;
        }
        width = std::max(width, 0);
        add(column, width - _widths[column]);
        _widths[column] = width;
        _user[column] = true;
    }

    /// @brief The columns a horizontal viewport shows.
    /// @param offset   The scroll offset.
    /// @param viewport The viewport's width.
    /// @return `[first, last)`: the column under @p offset to the one past the last under the viewport's end.
    [[nodiscard]] std::pair<std::size_t, std::size_t> columnsAt(int offset, int viewport) const {
        std::size_t const count = _widths.size();
        std::size_t const first = std::min(below(std::max(offset, 0)), count);
        std::size_t last = first;
        if (viewport > 0) {
            int const end = std::max(offset, 0) + viewport - 1;
            last = std::min(below(end) + 1, count);
        }
        return {first, std::max(first, last)};
    }

    /// @brief Where a column starts: the sum of the widths before it.
    /// @param column The column's place in the layout; past the last, the total width.
    /// @return The offset.
    [[nodiscard]] int offsetOf(std::size_t column) const {
        std::size_t index = std::min(column, _widths.size());
        int sum = 0;
        for (; index > 0; index -= index & (~index + 1)) {
            sum += _tree[index];
        }
        return sum;
    }

    /// @brief A column's width; one counted read.
    /// @param column The column's place in the layout.
    /// @return The width; 0 past the last column.
    [[nodiscard]] int widthOf(std::size_t column) const {
        ++_reads;
        return column < _widths.size() ? _widths[column] : 0;
    }

    /// @brief The width of every column together.
    /// @return The total.
    [[nodiscard]] int totalWidth() const noexcept { return offsetOf(_widths.size()); }

    /// @brief The columns held.
    /// @return How many.
    [[nodiscard]] std::size_t size() const noexcept { return _widths.size(); }

    /// @brief How many single widths `widthOf` has read.
    /// @return The count.
    [[nodiscard]] std::size_t columnReads() const noexcept { return _reads; }

private:
    void add(std::size_t column, int delta) {
        for (std::size_t at = column + 1; at < _tree.size(); at += at & (~at + 1)) {
            _tree[at] += delta;
        }
    }

    // How many leading columns end at or before @p offset: the column under @p offset, by descent on the tree.
    [[nodiscard]] std::size_t below(int offset) const {
        std::size_t index = 0;
        std::size_t step = std::bit_floor(std::max<std::size_t>(_tree.size(), 1));
        int left = offset;
        for (; step > 0; step >>= 1U) {
            std::size_t const next = index + step;
            if (next < _tree.size() && _tree[next] <= left) {
                index = next;
                left -= _tree[next];
            }
        }
        return index;
    }

    std::vector<int> _widths;
    std::vector<bool> _user;
    std::vector<int> _tree;  // 1-based Fenwick tree over _widths
    mutable std::size_t _reads = 0;
};

/// @brief The columns a table realizes: the pinned ones, then those in or near the horizontal viewport.
struct ColumnWindow {
    /// @brief Places in the layout: the pinned columns first, then the scrolled range, ascending.
    std::vector<std::size_t> columns;
    /// @brief The width before the scrolled range's first column that the window does not cover.
    int leading = 0;
    /// @brief The width after the window's last column.
    int trailing = 0;

    /// @brief Member-wise equality.
    /// @param other The window to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(ColumnWindow const& other) const = default;
};

/// @brief The column window at a horizontal scroll position: the first @p pinned columns, then the columns under the
///        viewport and @p overscan more on each side. Reads no width.
/// @param extents  The visible columns' widths.
/// @param pinned   How many leading columns stay in the window.
/// @param scrollX  The scroll offset.
/// @param viewport The viewport's width.
/// @param overscan Columns kept beyond the viewport on each side.
/// @return The window.
[[nodiscard]] inline ColumnWindow columnWindow(ColumnExtents const& extents, std::size_t pinned, int scrollX,
                                               int viewport, std::size_t overscan) {
    ColumnWindow window;
    std::size_t const count = extents.size();
    pinned = std::min(pinned, count);
    auto [first, last] = extents.columnsAt(scrollX, viewport);
    first = first > overscan ? first - overscan : 0;
    last = std::min(count, last + overscan);
    first = std::max(first, pinned);
    for (std::size_t at = 0; at < pinned; ++at) {
        window.columns.push_back(at);
    }
    for (std::size_t at = first; at < last; ++at) {
        window.columns.push_back(at);
    }
    window.leading = first < last ? extents.offsetOf(first) - extents.offsetOf(pinned) : 0;
    window.trailing = extents.totalWidth() - extents.offsetOf(std::max(first, last));
    return window;
}

namespace detail {

/// @brief Appends the runs of @p bands, at @p level, and of their children one level down: one run per maximal run of
///        a band's columns that stand together in @p visible, split at the pinned edge.
/// @param bands   The bands of one level.
/// @param visible The visible column ids, in layout order.
/// @param pinned  How many of the leading visible columns are pinned.
/// @param level   The level of @p bands, 0 for the outermost.
/// @param out     The runs found so far; the new ones are appended.
// NOLINTNEXTLINE(misc-no-recursion): a band's children are bands one level down.
inline void appendBandRuns(std::span<table::BandInfo const> bands, std::span<std::string const> visible,
                           std::size_t pinned, std::size_t level, std::vector<BandRun>& out) {
    for (table::BandInfo const& band : bands) {
        std::optional<std::size_t> first;
        std::size_t count = 0;
        auto const close = [&] {
            if (first) {
                out.push_back(BandRun{.level = level,
                                      .label = band.label,
                                      .unit = band.unit,
                                      .first = *first,
                                      .count = count,
                                      .pinned = *first < pinned});
            }
            first.reset();
            count = 0;
        };
        for (std::size_t at = 0; at < visible.size(); ++at) {
            bool const own = std::ranges::contains(band.columns, visible[at]);
            // A run ends at a foreign column, and at the pinned edge.
            if (!own || (at == pinned && first)) {
                close();
            }
            if (own) {
                first = first.value_or(at);
                ++count;
            }
        }
        close();
        appendBandRuns(band.bands, visible, pinned, level + 1, out);
    }
}

/// @brief Appends the bands that enclose a column, outermost first: the band of @p bands that names it, then the one
///        of that band's children that does, and so on.
/// @param bands    The bands of one level.
/// @param columnId The column's id.
/// @param out      The bands found so far; the enclosing ones are appended.
// NOLINTNEXTLINE(misc-no-recursion): a band's children are bands one level down.
inline void enclosingBands(std::span<table::BandInfo const> bands, std::string_view columnId,
                           std::vector<table::BandInfo const*>& out) {
    for (table::BandInfo const& band : bands) {
        if (std::ranges::contains(band.columns, columnId)) {
            out.push_back(&band);
            enclosingBands(band.bands, columnId, out);
            return;
        }
    }
}

}  // namespace detail

/// @brief The parts a table's header bands are drawn as over a layout: one run per maximal run of a band's columns
///        that stand together, split at the pinned edge.
///
/// A band whose columns the layout keeps side by side gives one run; one whose columns were moved apart gives one
/// per run that stands together, so no run covers a column that is not the band's; a band with no visible column
/// gives none.
/// @param bands   The bands, outermost first.
/// @param visible The visible column ids, in layout order.
/// @param pinned  How many of the leading visible columns are pinned.
/// @return The runs, by level from 0 (the outermost), each level left to right; positions are visible places.
[[nodiscard]] inline std::vector<BandRun> bandRuns(std::span<table::BandInfo const> bands,
                                                   std::span<std::string const> visible, std::size_t pinned) {
    std::vector<BandRun> out;
    detail::appendBandRuns(bands, visible, pinned, 0, out);
    std::ranges::stable_sort(out, [](BandRun const& left, BandRun const& right) {
        return left.level != right.level ? left.level < right.level : left.first < right.first;
    });
    return out;
}

/// @brief Whether a column's header shows its unit under the bands: not when a band enclosing it shows the same one.
/// @param bands    The bands, outermost first.
/// @param columnId The column's id.
/// @param unit     The column's unit.
/// @return `false` when @p unit is not empty and an enclosing band's unit equals it; `true` otherwise.
[[nodiscard]] inline bool columnUnitShown(std::span<table::BandInfo const> bands, std::string_view columnId,
                                          std::string_view unit) {
    if (unit.empty()) {
        return true;
    }
    std::vector<table::BandInfo const*> enclosing;
    detail::enclosingBands(bands, columnId, enclosing);
    return std::ranges::none_of(enclosing, [unit](table::BandInfo const* band) { return band->unit == unit; });
}

/// @brief A column header's accessible name under the bands: each enclosing band's label, outermost first, then the
///        column's, joined by `", "` ("Mass retained, sieve 4 mm").
/// @param bands    The bands, outermost first.
/// @param columnId The column's id.
/// @param label    The column's own name.
/// @return The name; @p label alone for a column under no band.
[[nodiscard]] inline std::string bandedHeaderName(std::span<table::BandInfo const> bands, std::string_view columnId,
                                                  std::string_view label) {
    std::vector<table::BandInfo const*> enclosing;
    detail::enclosingBands(bands, columnId, enclosing);
    std::string out;
    for (table::BandInfo const* band : enclosing) {
        out += band->label + ", ";
    }
    return out + std::string{label};
}

/// @brief A span part as a document or a fixed grid gives it: the cell of `column` covering `rows` rows downwards and
///        `columns` columns to the right in the layout, itself included.
struct CellSpan {
    /// @brief The spanning cell's column id.
    std::string column;
    /// @brief How many rows the cell covers, its own included.
    std::size_t rows = 1;
    /// @brief How many columns the cell covers, its own included.
    std::size_t columns = 1;
};

/// @brief How many visible columns a span covers once the layout is applied: its column, then each next visible column
///        while it is the next declared one, up to `CellSpan::columns`. A span across a column the user moved away or
///        hid is cut there.
/// @param span     The span.
/// @param declared Every column id, in declared order.
/// @param visible  The visible column ids, in layout order.
/// @return The count, from 1; 0 when the span's own column is not visible.
[[nodiscard]] inline std::size_t cutColumns(CellSpan const& span, std::span<std::string const> declared,
                                            std::span<std::string const> visible) {
    auto const shown = std::ranges::find(visible, span.column);
    if (shown == visible.end()) {
        return 0;
    }
    auto const own = std::ranges::find(declared, span.column);
    if (own == declared.end()) {
        return 1;
    }
    auto const place = static_cast<std::size_t>(shown - visible.begin());
    auto const order = static_cast<std::size_t>(own - declared.begin());
    std::size_t count = 1;
    while (count < span.columns && place + count < visible.size() && order + count < declared.size() &&
           visible[place + count] == declared[order + count]) {
        ++count;
    }
    return count;
}

namespace detail {

/// @brief The site names a misuse in this layer is reported under, through the runtime's owner probe.
namespace site {
/// @brief An exception escaped a widget callback; the mount caught it, so it never reaches the backend.
inline constexpr char const* kCallbackThrew = "morph::ui: an exception escaped a widget callback";
/// @brief A table row's span names no column, covers no rows, is a heading's, or repeats a column of its row; it is
///        refused and its cell stays in its own row.
inline constexpr char const* kRowSpanInvalid = "morph::ui: a table row's span is not a span of the table";
/// @brief A table row's span runs past the last row or over a heading row; it is refused.
inline constexpr char const* kRowSpanOverrun =
    "morph::ui: a table row's span runs past the last row or over a heading";
/// @brief A table row's span starts in a column an earlier row's span covers; it is refused.
inline constexpr char const* kRowSpanOverlap = "morph::ui: a table row's span starts in a column another span covers";
/// @brief A table above its virtualization threshold was given spans; none is drawn.
inline constexpr char const* kRowSpanVirtualized = "morph::ui: a virtualized table draws no row spans";
}  // namespace site

/// @brief What a table's span resolve knows of one row: whether it is a heading, the spans it asked for and the ones
///        its widget was last told.
struct RowSpanEntry {
    /// @brief Whether the row is a heading row.
    bool heading = false;
    /// @brief The spans the row's binding last gave.
    std::vector<RowSpan> requested;
    /// @brief The effective spans the row's widget was last told, by column.
    std::vector<RowSpan> sent;
    /// @brief The widget `sent` was told to.
    Widget const* told = nullptr;
};

/// @brief The outcome of resolving a table's requested spans against its rows in order.
struct ResolvedSpans {
    /// @brief The effective spans of each row, in the order given, each sorted by column.
    std::vector<std::vector<RowSpan>> effective;
    /// @brief The refusals, one description per refused span, tagged with the site it is reported under.
    std::vector<std::pair<char const*, std::string>> refused;
};

/// @brief One row as the span resolve sees it.
struct SpanRow {
    /// @brief A text naming the row in a refusal.
    std::string name;
    /// @brief Whether the row is a heading.
    bool heading = false;
    /// @brief The spans the row requested.
    std::vector<RowSpan> requested;
};

/// @brief Resolves the requested spans of a table's rows, top to bottom, one column at a time.
///
/// A span is refused, alone, when it names no column, covers 0 rows or 0 columns, belongs to a heading row, repeats a
/// column of its row, starts in or covers a cell an earlier span covers, or runs past the last row or over a heading
/// row. The earlier span wins an overlap. A span covers `rows` rows from its own and `columns` visible places from its
/// column's, clipped at the last place; a span whose column is hidden covers nothing and is not refused.
/// @param rows    The rows in order.
/// @param columns How many columns the table has.
/// @param placeOf Each column's visible place, by index into the table's columns, none for a hidden column; empty:
/// every
///                column at the place of its index.
/// @return The effective spans per row and the refusals.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one refusal per rule of a span.
[[nodiscard]] inline ResolvedSpans resolveRowSpans(std::vector<SpanRow> const& rows, std::size_t columns,
                                                   std::span<std::optional<std::size_t> const> placeOf = {}) {
    ResolvedSpans out;
    out.effective.resize(rows.size());
    auto const placeOfColumn = [&placeOf](std::size_t column) -> std::optional<std::size_t> {
        if (placeOf.empty()) {
            return column;
        }
        return column < placeOf.size() ? placeOf[column] : std::nullopt;
    };
    // Until which row each visible place is covered.
    std::vector<std::size_t> coveredUntil(columns, 0);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        SpanRow const& row = rows.at(index);
        std::vector<bool> named(columns, false);
        for (RowSpan const& span : row.requested) {
            auto const refuse = [&](char const* const site, std::string_view why) {
                out.refused.emplace_back(
                    site, row.name + ": column " + std::to_string(span.column) + " " + std::string{why});
            };
            if (span.column >= columns || span.rows == 0 || span.columns == 0 || row.heading ||
                named.at(span.column)) {
                refuse(site::kRowSpanInvalid, "is not a span of this table");
                continue;
            }
            named.at(span.column) = true;
            std::optional<std::size_t> const place = placeOfColumn(span.column);
            if (!place || *place >= columns) {
                continue;
            }
            std::size_t const wide = std::min(span.columns, columns - *place);
            if (coveredUntil.at(*place) > index) {
                refuse(site::kRowSpanOverlap, "starts under another span");
                continue;
            }
            if (span.rows == 1 && wide == 1) {
                continue;
            }
            if (std::any_of(coveredUntil.begin() + static_cast<std::ptrdiff_t>(*place),
                            coveredUntil.begin() + static_cast<std::ptrdiff_t>(*place + wide),
                            [index](std::size_t until) { return until > index; })) {
                refuse(site::kRowSpanOverlap, "covers a cell another span covers");
                continue;
            }
            std::size_t const end = index + span.rows;
            bool const crossesHeading =
                end <= rows.size() && std::any_of(rows.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                                                  rows.begin() + static_cast<std::ptrdiff_t>(end),
                                                  [](SpanRow const& covered) { return covered.heading; });
            if (end > rows.size() || crossesHeading) {
                refuse(site::kRowSpanOverrun, "runs past the last row or over a heading");
                continue;
            }
            out.effective.at(index).push_back(span);
            std::fill(coveredUntil.begin() + static_cast<std::ptrdiff_t>(*place),
                      coveredUntil.begin() + static_cast<std::ptrdiff_t>(*place + wide), end);
        }
        std::ranges::sort(out.effective.at(index), {}, &RowSpan::column);
    }
    return out;
}

/// @brief Called for each newly mounted row with its widget and key; a Table tells its widget the row's key.
using RowHook = std::function<void(reactive::Scope&, Node const&, Widget&, Key const&)>;

struct KeyedRow;

/// @brief What a table is told around each pass that changes its rows: before it starts, and after it has brought the
///        rows into the snapshot's order, with the rows in their container's order.
struct RowsObserver {
    /// @brief Called inside the rows Effect, tracked, before each pass: what it reads re-runs the pass, so a change
    ///        that arrives with a change of rows is seen in the same run, never in an Effect of its own.
    std::function<void()> track;
    /// @brief Called before a pass that mounts, moves or removes rows.
    std::function<void()> begin;
    /// @brief Called after the pass, with the table's rows: realized rows first, in the container's order.
    std::function<void(std::vector<KeyedRow> const&)> end;
};

/// @brief Whether a row's widget is realized, and if not, why -- so the windowing pass of `reconcile` never
///        confuses a row that has no widget by design (its view was a null node) with one whose widget is simply
///        not realized right now, which it should realize the next time the row enters the visible range.
enum class RowWidgetState : std::uint8_t {
    Realized,    ///< `widgetScope`/`widget` are set.
    Dewindowed,  ///< The row's view is not null, but its widget is not realized: out of the visible range.
    NullView,    ///< The row's view is a null node; it never had a widget and never will while that stays true.
};

/// @brief One mounted row of a ForEach or Table.
///
/// A row's state (its slot, and the signal it reads) and its widget are owned by two separate scopes so a
/// virtualized table can release a row's widget -- the expensive part of mounting a row -- while its key is still
/// in the data, without losing the row's place: `stateScope` is made once, when the key first appears, and lives
/// until the key is gone; `widgetScope` comes and goes with whether the row is realized. A non-virtualized
/// `ForEach`/`Table` (below its `Virtualization::threshold`, or on a backend that never dewindows a row) always
/// has `widgetScope` set alongside `stateScope`, so this costs nothing when windowing never applies.
struct KeyedRow {
    /// @brief The row's key.
    Key key;
    /// @brief Owns the row's slot; made once, for as long as the key is in the data.
    std::unique_ptr<reactive::Scope> stateScope;
    /// @brief The row's slot, owned by `stateScope`.
    RowSlot* slot = nullptr;
    /// @brief Owns the row's widget and its bindings; null while the row is dewindowed or has no view.
    std::unique_ptr<reactive::Scope> widgetScope;
    /// @brief The row's widget, owned by `widgetScope`; null exactly when `widgetScope` is null.
    Widget* widget = nullptr;
    /// @brief Which of the three states above `widget` is in.
    RowWidgetState widgetState = RowWidgetState::NullView;
    /// @brief This row's position in view order, as of the most recent `reconcile` pass -- not its position in
    ///        `KeyedRows::list`, which `reorder` groups realized rows ahead of dewindowed ones within (so the
    ///        container only ever holds the widgets that exist), and so no longer matches view order once any row
    ///        is dewindowed. `applyWindow` compares this against the visible range; `reconcile` is the only writer.
    std::size_t viewPosition = 0;
};

/// @brief What the backend's own scroll position asks a virtualized table to realize next.
struct VisibleRange {
    /// @brief The view index of the first row the backend wants realized.
    std::size_t first = 0;
    /// @brief How many rows from `first` the backend wants realized; 0 (the default) asks for nothing, which a
    ///        table below its `Virtualization::threshold` never overrides, since its windowing Effect only runs on
    ///        a backend that calls `setOnVisibleRangeChanged` in the first place.
    std::size_t count = 0;

    /// @brief Memberwise equality, so an unchanged range notifies nobody.
    /// @return Whether both `first` and `count` match.
    bool operator==(VisibleRange const&) const = default;
};

/// @brief The mounted rows of one ForEach or Table, in their container's order.
struct KeyedRows {
    KeyedRows() = default;
    /// @brief Unmounts the rows last first, as a container's children are; a vector's own destruction order differs
    ///        between standard libraries.
    ~KeyedRows() {
        while (!list.empty()) {
            list.pop_back();
        }
    }
    KeyedRows(KeyedRows const&) = delete;
    KeyedRows& operator=(KeyedRows const&) = delete;
    KeyedRows(KeyedRows&&) = delete;
    KeyedRows& operator=(KeyedRows&&) = delete;

    /// @brief The rows; those with a widget are in the container's order.
    std::vector<KeyedRow> list;
};

/// @brief Which entries of a permutation lie on one longest increasing run.
///
/// @p sequence holds, in current order, each widget's target position; the widgets on the run are already in
/// order relative to each other and stay where they are, and every other one is moved once.
/// @param sequence A permutation of `0 .. n-1`.
/// @return Indexed by target position: whether that widget stays.
[[nodiscard]] inline std::vector<bool> stableTargets(std::vector<std::size_t> const& sequence) {
    constexpr auto kNone = static_cast<std::size_t>(-1);
    std::vector<std::size_t> tails;  // tails.at(n - 1): where the smallest tail of a run of length n sits
    std::vector<std::size_t> previous(sequence.size(), kNone);
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        auto const place = std::ranges::lower_bound(tails, sequence.at(i), {},
                                                    [&sequence](std::size_t entry) { return sequence.at(entry); });
        auto const length = static_cast<std::size_t>(place - tails.begin());
        if (length > 0) {
            previous.at(i) = tails.at(length - 1);
        }
        if (length == tails.size()) {
            tails.push_back(i);
        } else {
            tails.at(length) = i;
        }
    }
    std::vector<bool> stable(sequence.size(), false);
    for (std::size_t cursor = tails.empty() ? kNone : tails.back(); cursor != kNone; cursor = previous.at(cursor)) {
        stable.at(sequence.at(cursor)) = true;
    }
    return stable;
}

/// @brief Brings a container's children from their current order into the target order with `moveChild`,
///        moving only the children outside a longest run already in order.
/// @param container The container.
/// @param current Its children, in their current order. Kept equal to the container's order after every move, so
///        it is still that order when a move throws.
/// @param target The same children, in the order wanted.
inline void reorderChildren(ContainerWidget& container, std::vector<Widget*>& current,
                            std::vector<Widget*> const& target) {
    std::unordered_map<Widget const*, std::size_t> targetOf;
    for (std::size_t i = 0; i < target.size(); ++i) {
        targetOf.emplace(target.at(i), i);
    }
    std::vector<std::size_t> sequence;
    sequence.reserve(current.size());
    for (Widget const* const child : current) {
        sequence.push_back(targetOf.at(child));
    }
    std::vector<bool> const stable = stableTargets(sequence);
    auto const indexOf = [&current](Widget const* child) {
        return static_cast<std::size_t>(std::ranges::find(current, child) - current.begin());
    };
    // From the back: each moved child goes directly before its successor in the target order, which is already
    // in place, so one move per unstable child suffices.
    for (std::size_t position = target.size(); position-- > 0;) {
        if (stable.at(position)) {
            continue;
        }
        Widget* const moving = target.at(position);
        std::size_t const from = indexOf(moving);
        std::size_t destination = current.size() - 1;
        if (position + 1 < target.size()) {
            std::size_t const before = indexOf(target.at(position + 1));
            destination = from < before ? before - 1 : before;
        }
        if (destination == from) {
            continue;
        }
        container.moveChild(*moving, destination);
        current.erase(current.begin() + static_cast<std::ptrdiff_t>(from));
        current.insert(current.begin() + static_cast<std::ptrdiff_t>(destination), moving);
    }
}

/// @brief Where one Tabs keeps its pages: a slot per tab, made the first time the tab is selected and kept, hidden,
///        after.
struct Pages {
    /// @brief Each page's slot; null until the tab is first selected.
    std::vector<SlotWidget*> pageSlots;
    /// @brief The page shown now, if any; may be past the last page, and then none is shown.
    std::optional<std::size_t> shown;
    /// @brief The tab the bar highlights, as far as the mount knows: the last one it set, or the user's pick since.
    std::optional<std::size_t> highlighted;
    /// @brief The Tabs' `onSelect`, run as a widget callback; empty when the Tabs has none.
    std::function<void(std::size_t)> onSelect;
};

/// @brief Builds widgets for nodes and wires their props and callbacks.
///
/// One per `Mounted`. It must outlive every scope it mounted into: a Switch, Tabs, Dialog, ForEach or Table binding
/// calls back into it to mount content later.
class Mounter {
public:
    /// @param runtime The runtime bindings are made in. Borrowed.
    /// @param backend Makes the widgets. Borrowed.
    Mounter(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND) noexcept
        : _rt{&runtime}, _backend{&backend} {}

    /// @brief Mounts @p node under @p parent; @p scope owns its widgets and bindings.
    ///
    /// A node's widget is adopted first, then its bindings, then its children, so destroying the scope destroys
    /// bindings before their widget and children before their parent.
    /// @param scope Owns everything mounted.
    /// @param node The node; null mounts nothing.
    /// @param parent The container to append to, or null for a root.
    /// @return The node's widget, or null for a null node.
    Widget* mount(reactive::Scope& scope, Node const& node, ContainerWidget* parent) {
        if (!node) {
            return nullptr;
        }
        return std::visit(
            [&](auto const& spec) -> Widget* {
                if constexpr (requires { spec.common.layout.alignSelf; }) {
                    checkPlacement(spec.common, parent);
                    checkShare(spec.common, parent);
                }
                Widget& widget = this->mountKind(scope, spec, parent);
                link(scope, widget, parent, takesFocus(spec));
                return &widget;
            },
            node->kind);
    }

    /// @brief Moves keyboard focus along the focus chain from the mounted widget whose `focusId` is @p focusId.
    /// @param focusId The name.
    /// @param forward Whether to the next control, or the previous one.
    /// @return Whether a control took focus.
    bool focusAlong(std::string_view focusId, bool forward) {
        auto const found = _focusable.lower_bound(focusId);
        if (found == _focusable.end() || found->first != focusId) {
            return false;
        }
        return focusAlong(*found->second, forward);
    }

    /// @brief Moves keyboard focus along the focus chain from @p from: the mounted widgets that take focus (a text
    ///        input, a button, a check box, a select, a menu, a date-time input, a slider, a file picker, a table, a
    ///        tabs, a badge that can be removed), shown and enabled with every container around them, in document
    ///        order, walked when asked.
    /// @param from    The widget focus leaves.
    /// @param forward Whether to the next control, or the previous one.
    /// @return Whether a control took focus; `false` when none lies that way.
    bool focusAlong(Widget const& from, bool forward) {
        // The chain of the mount the widget is in: Tab in a dialog stays in the dialog.
        Widget const* top = &from;
        for (auto found = _chain.find(top); found != _chain.end() && found->second.parent != nullptr;
             found = _chain.find(top)) {
            top = found->second.parent;
        }
        bool const rooted = std::ranges::contains(_chainRoots, top);
        std::vector<std::pair<Widget*, bool>> order;
        for (Widget* const root : _chainRoots) {
            if (root == top || !rooted) {
                chainOrder(*root, true, order);
            }
        }
        auto const hit = std::ranges::find(order, &from, [](auto const& entry) { return entry.first; });
        if (hit == order.end()) {
            return false;
        }
        auto const index = static_cast<std::size_t>(hit - order.begin());
        for (std::size_t step = 1; step < order.size(); ++step) {
            if (!forward && step > index) {
                break;
            }
            std::size_t const next = forward ? index + step : index - step;
            if (next >= order.size()) {
                break;
            }
            if (order.at(next).second) {
                try {
                    order.at(next).first->focus();
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Focus is a request: a backend that cannot move it leaves it where it was.
                }
                return true;
            }
        }
        return false;
    }

    /// @brief Mounts @p node as `mount` does, as a mount of its own: the first `autofocus` widget it mounts, in
    ///        document order, takes keyboard focus once the whole of it has mounted.
    /// @param scope Owns everything mounted.
    /// @param node The node; null mounts nothing.
    /// @param parent The container to append to, or null for a root.
    /// @return The node's widget, or null for a null node.
    Widget* mountRoot(reactive::Scope& scope, Node const& node, ContainerWidget* parent) {
        return mountPass([&] { return mount(scope, node, parent); });
    }

    /// @brief What the mount did to the rows of its source tables.
    /// @return The counters.
    [[nodiscard]] MountStats const& stats() const noexcept { return _stats; }

    /// @brief The column extents of the table over a `TableSource` mounted last, for tests.
    /// @return They, or null when no such table is mounted.
    [[nodiscard]] ColumnExtents const* columnExtents() const noexcept { return _extents.lock().get(); }

    /// @brief Moves keyboard focus to the mounted widget whose `focusId` is @p focusId.
    ///
    /// Of several such widgets (rows of a list), the one mounted first is taken. A backend that cannot move focus
    /// leaves it where it was; the request was still made.
    /// @param focusId The name.
    /// @return Whether a mounted widget has that name.
    bool focus(std::string_view focusId) {
        // lower_bound, not find: of equal names, find may answer any; lower_bound answers the first inserted.
        auto const found = _focusable.lower_bound(focusId);
        if (found == _focusable.end() || found->first != focusId) {
            return false;
        }
        try {
            found->second->focus();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Focus is a request: a backend that cannot move it leaves it where it was.
        }
        return true;
    }

    /// @brief Moves the text cursor of the mounted text input whose `focusId` is @p focusId to @p offset, focuses it
    ///        and asks it to reveal the cursor's line.
    ///
    /// Of several such widgets, the one mounted first is taken. An offset inside a UTF-8 sequence moves back to the
    /// start of that code point.
    /// @param focusId The name.
    /// @param offset  A byte offset into the text it shows.
    /// @return Whether a mounted text input has that name.
    bool placeCursor(std::string_view focusId, std::size_t offset) {
        auto const found = _focusable.lower_bound(focusId);
        if (found == _focusable.end() || found->first != focusId) {
            return false;
        }
        auto* const field = dynamic_cast<TextInputWidget*>(found->second);
        if (field == nullptr) {
            return false;
        }
        field->setCursor(offset);
        try {
            field->focus();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Focus is a request: a backend that cannot move it leaves it where it was.
        }
        // After the focus: a scroll that follows focus has moved to the field as a whole by now, and the cursor's
        // line is what is then brought into view.
        field->revealCursor();
        return true;
    }

private:
    // Runs @p body with @p overlay as the overlay an ActionBar mounts into the footer of, and puts the previous one
    // back after it.
    template <typename F>
    auto underOverlay(ContainerWidget* overlay, F const& body) -> std::invoke_result_t<F const&> {
        class Restore {
        public:
            Restore(Mounter& self, ContainerWidget* next) : _self{&self}, _saved{std::exchange(self._overlay, next)} {}
            Restore(Restore const&) = delete;
            Restore& operator=(Restore const&) = delete;
            Restore(Restore&&) = delete;
            Restore& operator=(Restore&&) = delete;
            ~Restore() { _self->_overlay = _saved; }

        private:
            Mounter* _self;
            ContainerWidget* _saved;
        };
        Restore const restore{*this, overlay};
        return body();
    }

    // The footer of the overlay whose content is being mounted, or null outside any.
    [[nodiscard]] ContainerWidget* overlayFooter() const {
        if (auto* const dialog = dynamic_cast<DialogWidget*>(_overlay)) {
            return &dialog->footer();
        }
        if (auto* const drawer = dynamic_cast<DrawerWidget*>(_overlay)) {
            return &drawer->footer();
        }
        return nullptr;
    }

    // One mount: the root, or content, a page or a row a binding mounts later. The outermost pass focuses the first
    // autofocus widget mounted during it, in document order, after everything in it has mounted; a pass nested in
    // another (content a binding mounts while the root mounts) leaves that to the outer one. A pass that throws gives
    // back the candidate it found, because its caller destroys what it mounted.
    template <typename F>
    auto mountPass(F const& body) -> std::invoke_result_t<F const&> {
        Widget* const before = _autofocus;
        ++_passDepth;
        try {
            auto result = body();
            --_passDepth;
            if (_passDepth == 0) {
                if (Widget* const target = std::exchange(_autofocus, nullptr); target != nullptr) {
                    try {
                        target->focus();
                    } catch (...) {  // NOLINT(bugprone-empty-catch)
                        // Focus is a request: a backend that cannot move it leaves it where it was.
                    }
                }
            }
            return result;
        } catch (...) {
            _autofocus = before;
            --_passDepth;
            throw;
        }
    }

    // A constant is applied once; a binding becomes an equality-gated Computed plus an Effect, made in that order
    // after the widget, so the Effect dies first. Returns the Computed, or null for a constant.
    template <typename T, typename Apply>
    reactive::Computed<T>* bind(reactive::Scope& scope, Prop<T> const& prop, Apply apply) {
        if (!prop.isBound()) {
            apply(prop.constant());
            return nullptr;
        }
        auto& value = scope.make<reactive::Computed<T>>(*_rt, prop.binding());
        scope.effect([&value, apply = std::move(apply)] { apply(value.get()); });
        return &value;
    }

    // Runs a widget callback, and returns whether it completed.
    // - Inside widgetEvent: one batch, and a flush that would start meanwhile is re-posted, so a remount never
    //   destroys a widget whose native handler is on the stack.
    // - Untracked: a backend may call a handler from inside a setter, and so from inside the binding Effect that
    //   called it; that Effect must not subscribe to what the handler reads.
    // - An exception is reported and stops here: the backend that called the handler need not be exception-safe.
    //   Writes made before it still flush when the batch closes.
    template <typename F>
    static bool runCallback(reactive::Runtime& runtime, F const& body) {
        return runtime.widgetEvent([&] {
            return runtime.untracked([&] {
                try {
                    body();
                    return true;
                } catch (...) {
                    runtime.core()->report(site::kCallbackThrew);
                    return false;
                }
            });
        });
    }

    template <typename... Args>
    [[nodiscard]] std::function<void(Args...)> event(std::function<void(Args...)> const& handler) const {
        if (!handler) {
            return {};
        }
        // The closure owns a copy of the handler and frees it when it dies; the analyzer loses the copy's heap block
        // in libc++'s std::function and reports it as leaked.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
        return [runtime = _rt, handler](Args... args) {
            static_cast<void>(runCallback(*runtime, [&] { handler(std::move(args)...); }));
        };
    }

    /// @brief A controlled input's re-assertion: what the widget shows as far as the mount knows, what its slot
    ///        says now, and how to show a value.
    /// @tparam T The value type.
    template <typename T>
    struct Reassertion {
        /// @brief What the widget shows: the last value the mount set, or the last value the user asked for since.
        ///        Owned by the scope, made before this, so it outlives it.
        std::optional<T>* shown;
        /// @brief What the slot says now.
        std::function<T()> read;
        /// @brief Shows a value, and records it in `*shown`.
        std::function<void(T const&)> apply;
    };

    // What a controlled input's event calls after the application's handler, with the value the user asked for.
    //
    // The widget shows the request already: it is recorded as shown. Then one turn is posted to the owner; it reads
    // the slot untracked and shows it when the widget shows anything else. The turn is posted after the event's
    // widgetEvent closed, so it runs after the flush the event caused, whose bindings record what they set: an
    // accepted request costs no setter call, a refused one snaps back, and a transformed one, which the binding has
    // shown already, costs nothing more. The check is owned by @p scope and held weakly by the event and the turn,
    // so neither does anything once the content is gone.
    template <typename T, typename Read, typename Apply>
    [[nodiscard]] std::function<void(T const&)> reasserter(reactive::Scope& scope, std::optional<T>& shown, Read read,
                                                           Apply apply) {
        auto const check = std::make_shared<Reassertion<T>>(
            Reassertion<T>{.shown = &shown, .read = std::move(read), .apply = std::move(apply)});
        scope.make<std::shared_ptr<Reassertion<T>>>(check);
        return [runtime = _rt, weak = std::weak_ptr{check}](T const& requested) {
            if (auto const live = weak.lock()) {
                *live->shown = requested;
            } else {
                return;
            }
            // Called from a widget handler, which must not throw: a post that fails leaves the widget showing the
            // user's value until the slot next changes.
            try {
                runtime->owner().post([runtime, weak] {
                    if (auto const live = weak.lock()) {
                        runtime->untracked([&live] { showSlot(*live); });
                    }
                });
            } catch (...) {  // NOLINT(bugprone-empty-catch): see above
            }
        };
    }

    // Shows the slot's value if the widget shows anything else. A slot that throws is its binding's failure, which
    // the binding's Effect reports; the widget keeps what the user did.
    template <typename T>
    static void showSlot(Reassertion<T> const& check) noexcept {
        try {
            T const current = check.read();
            if (!(check.shown->has_value() && **check.shown == current)) {
                check.apply(current);
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch): reported by the binding's Effect, see above
        }
    }

    // A controlled input's value: bound like any property, recording what it shows, and not sending what it already
    // shows; a slot is also re-asserted after each user event (see `reasserter`). Returns what the event calls, or
    // nothing for a constant, which is not a slot: the widget keeps what the user did to it.
    template <typename T, typename Apply>
    [[nodiscard]] std::function<void(T const&)> controlled(reactive::Scope& scope, Prop<T> const& prop, Apply apply) {
        auto& shown = scope.make<std::optional<T>>();
        // A value the widget already shows, the user's accepted edit above all, is not sent again.
        auto show = [&shown, apply = std::move(apply)](T const& value) {
            if (shown.has_value() && *shown == value) {
                return;
            }
            apply(value);
            shown = value;
        };
        reactive::Computed<T> const* const value = bind(scope, prop, show);
        if (value == nullptr) {
            return {};
        }
        return reasserter<T>(scope, shown, [value] { return value->get(); }, std::move(show));
    }

    // An input's event: the application's handler, run as `event` runs it, then the re-assertion of the request.
    // Installed whenever either exists, so a slot without a handler is read-only rather than editable.
    // The first argument is what the user asked for; any others (a table's kind of change) only reach the handler.
    template <typename T, typename Arg, typename... Rest>
    [[nodiscard]] std::function<void(Arg, Rest...)> inputEvent(std::function<void(Arg, Rest...)> const& handler,
                                                               std::function<void(T const&)> reassert) const {
        if (!reassert) {
            return event(handler);
        }
        return [inner = event(handler), reassert = std::move(reassert)](Arg arg, Rest... rest) {
            T const requested(arg);
            if (inner) {
                inner(std::move(arg), std::move(rest)...);
            }
            reassert(requested);
        };
    }

    // The loader's rules for bounds, for a view tree built in C++: no negative bound, no minimum above its maximum,
    // and no fixed amount outside its bounds, which would leave it no room.
    static void checkLayout(Common const& common) {
        LayoutHints const& hints = common.layout;
        auto const refuse = [&common](std::string const& what) {
            throw std::logic_error{"morph::ui::mount: " + what + " on the node" +
                                   (common.testId.empty() ? std::string{} : " `" + common.testId + "`")};
        };
        auto const check = [&refuse](Sizing sizing, std::optional<int> minimum, std::optional<int> maximum,
                                     std::string const& axis) {
            if (minimum.value_or(0) < 0 || maximum.value_or(0) < 0) {
                refuse("a negative " + axis + " bound");
            }
            if (minimum && maximum && *minimum > *maximum) {
                refuse("a minimum " + axis + " above its maximum");
            }
            if (sizing.kind == Sizing::Kind::Fixed &&
                (sizing.amount < minimum.value_or(sizing.amount) || sizing.amount > maximum.value_or(sizing.amount))) {
                refuse("a fixed " + axis + " outside its bounds");
            }
        };
        check(hints.width, hints.minWidth, hints.maxWidth, "width");
        check(hints.height, hints.minHeight, hints.maxHeight, "height");
        if (hints.width.kind == Sizing::Kind::Percent && (hints.width.amount < 1 || hints.width.amount > 100)) {
            refuse("a percent width outside 1 to 100");
        }
        if (hints.height.kind == Sizing::Kind::Percent) {
            refuse("a percent height; a percent sizes a width");
        }
    }

    // The loader's rule for `percent`, for a view tree built in C++: a share of its container's width is a width along
    // a row that does not wrap, a horizontal `forEach` that does not wrap, or a horizontal splitter's pane.
    void checkShare(Common const& common, ContainerWidget const* parent) const {
        if (common.layout.width.kind == Sizing::Kind::Percent && !_shareParents.contains(parent)) {
            std::string const named = common.testId.empty() ? std::string{} : " (node `" + common.testId + "`)";
            throw std::logic_error{
                "morph::ui::mount: `percent` sizes a width along a row or a horizontal splitter; "
                "nothing here takes a share" +
                named};
        }
    }

    // A grid track's or a table column's width is no share.
    static void checkNoShare(std::span<GridTrack const> tracks) {
        if (std::ranges::any_of(tracks,
                                [](GridTrack const& track) { return track.width.kind == Sizing::Kind::Percent; })) {
            throw std::logic_error{
                "morph::ui::mount: a grid track is sized by the grid; `percent` sizes a width "
                "along a row or a horizontal splitter"};
        }
    }
    static void checkNoShare(std::span<TableColumn const> columns) {
        if (std::ranges::any_of(
                columns, [](TableColumn const& column) { return column.width.kind == Sizing::Kind::Percent; })) {
            throw std::logic_error{
                "morph::ui::mount: a table column is sized by the table; `percent` sizes a width "
                "along a row or a horizontal splitter"};
        }
    }

    // A dialog or a drawer is sized by the frontend, not by its container: Stretch sizing on one would silently
    // change how every *other* sibling in its own parent layout is justified. Covers both the document path's own
    // computed (non-literal) `sizing` -- resolved to a Sizing only here, at mount time, since the document-level
    // checkSizing validates only a literal one -- and a C++ author calling ui::dialog()/ui::drawer() directly, both
    // of which reach this same mountKind overload before either backend ever sees the result.
    static void checkOverlayLayout(Common const& common, std::string_view kind) {
        LayoutHints const& hints = common.layout;
        auto const refuse = [&common, kind](std::string const& axis) {
            throw std::logic_error{"morph::ui::mount: a " + std::string{kind} +
                                   " is sized by the frontend, not by its container; its " + axis + " cannot stretch" +
                                   (common.testId.empty() ? std::string{} : " (node `" + common.testId + "`)")};
        };
        if (hints.width.kind == Sizing::Kind::Stretch) {
            refuse("width");
        }
        if (hints.width.kind == Sizing::Kind::Percent) {
            throw std::logic_error{"morph::ui::mount: a " + std::string{kind} +
                                   " is sized by the frontend, not by its container; its width takes no share"};
        }
        if (hints.height.kind == Sizing::Kind::Stretch) {
            refuse("height");
        }
    }

    // The loader's rule for `alignSelf`, for a view tree built in C++: only a stack's or a grid's child has a cross
    // axis to be placed on.
    static void checkPlacement(Common const& common, ContainerWidget const* parent) {
        if (common.layout.alignSelf && dynamic_cast<StackWidget const*>(parent) == nullptr &&
            dynamic_cast<GridWidget const*>(parent) == nullptr) {
            std::string const named = common.testId.empty() ? std::string{} : " `" + common.testId + "`";
            throw std::logic_error{
                "morph::ui::mount: `alignSelf` places a node in its row, column or grid, and the node" + named +
                " is not in one"};
        }
    }

    void applyCommon(reactive::Scope& scope, Widget& widget, Common const& common) {
        // The focus chain passes over a hidden or disabled widget and what is inside it.
        ChainFlags const* const flags = common.visible.isBound() || !common.visible.constant() ||
                                                common.enabled.isBound() || !common.enabled.constant()
                                            ? &scope.make<ChainFlags>(_chainFlags, &widget)
                                            : nullptr;
        if (common.visible.isBound() || !common.visible.constant()) {
            bind(scope, common.visible, [&widget, flags](bool shown) {
                flags->set(true, shown);
                widget.setVisible(shown);
            });
        }
        if (common.enabled.isBound() || !common.enabled.constant()) {
            bind(scope, common.enabled, [&widget, flags](bool enabled) {
                flags->set(false, enabled);
                widget.setEnabled(enabled);
            });
        }
        if (common.layout != LayoutHints{}) {
            checkLayout(common);
            widget.setLayout(common.layout);
        }
        if (common.dragKey.isBound() || common.dragKey.constant().has_value()) {
            bind(scope, common.dragKey, [&widget](std::optional<Key> const& key) { widget.setDragKey(key); });
        }
        if (common.onDrop) {
            applyDrop(widget, common);
        }
        if (common.a11y.name.isBound() || !common.a11y.name.constant().empty()) {
            bind(scope, common.a11y.name, [&widget](std::string const& name) { widget.setAccessibleName(name); });
        }
        if (!common.a11y.role.empty()) {
            widget.setAccessibleRole(common.a11y.role);
        }
        if (!common.testId.empty()) {
            widget.setTestId(common.testId);
        }
        if (common.tooltip.isBound() || !common.tooltip.constant().empty()) {
            bind(scope, common.tooltip, [&widget](std::string const& text) { widget.setTooltip(text); });
        }
        if (common.surface.isBound() || !common.surface.constant().empty()) {
            bind(scope, common.surface, [&widget](std::string const& surface) { widget.setSurface(surface); });
        }
        if (!common.keys.empty()) {
            applyKeys(widget, common.keys);
        }
        if (common.autofocus && _autofocus == nullptr) {
            _autofocus = &widget;
        }
        if (common.focusId.isBound()) {
            // Registered under the name it has now, and again under each new one.
            auto& focusable = scope.make<Focusable>(_focusable, std::nullopt);
            bind(scope, common.focusId, [this, &focusable, &widget](std::string const& name) {
                focusable.reset(name.empty() ? std::nullopt : std::optional{_focusable.emplace(name, &widget)});
            });
        } else if (!common.focusId.constant().empty()) {
            scope.make<Focusable>(_focusable, _focusable.emplace(common.focusId.constant(), &widget));
        }
    }

    using FocusMap = std::multimap<std::string, Widget*, std::less<>>;

    // One widget of the focus chain's tree: the container `mount` appended it to, its children in their order, and
    // whether it takes focus. A container a node did not mount (a table's row) has an entry while it has children,
    // and no parent: what is inside it is not on the chain.
    struct ChainEntry {
        Widget const* parent = nullptr;
        std::vector<Widget*> children;
        bool takesFocus = false;
        bool linked = false;
    };

    // Whether a widget is shown and enabled, as its `Common` says now, while it has a bound or non-default one.
    struct ChainFlags {
        ChainFlags(std::unordered_map<Widget const*, std::pair<bool, bool>>& owner, Widget const* widget)
            : flags{&owner}, of{widget} {
            flags->insert_or_assign(of, std::pair{true, true});
        }
        ChainFlags(ChainFlags const&) = delete;
        ChainFlags& operator=(ChainFlags const&) = delete;
        ChainFlags(ChainFlags&&) = delete;
        ChainFlags& operator=(ChainFlags&&) = delete;
        ~ChainFlags() { flags->erase(of); }

        void set(bool visible, bool value) const {
            auto& entry = flags->at(of);
            (visible ? entry.first : entry.second) = value;
        }

        std::unordered_map<Widget const*, std::pair<bool, bool>>* flags;
        Widget const* of;
    };

    // A mounted widget's place in the chain's tree, owned by its scope after the widget and its content, so it is let
    // go before them.
    struct ChainLink {
        ChainLink(Mounter& owner, Widget* widget) noexcept : mounter{&owner}, of{widget} {}
        ChainLink(ChainLink const&) = delete;
        ChainLink& operator=(ChainLink const&) = delete;
        ChainLink(ChainLink&&) = delete;
        ChainLink& operator=(ChainLink&&) = delete;
        ~ChainLink() { mounter->unlink(of); }

        Mounter* mounter;
        Widget* of;
    };

    // A container whose content is not on the chain, registered while it is mounted.
    struct OffChain {
        OffChain(std::unordered_set<Widget const*>& owner, Widget const* container) : set{&owner}, of{container} {
            set->insert(of);
        }
        OffChain(OffChain const&) = delete;
        OffChain& operator=(OffChain const&) = delete;
        OffChain(OffChain&&) = delete;
        OffChain& operator=(OffChain&&) = delete;
        ~OffChain() { set->erase(of); }

        std::unordered_set<Widget const*>* set;
        Widget const* of;
    };

    // Which nodes take focus: the controls a user tabs through.
    template <typename Spec>
    [[nodiscard]] static bool takesFocus(Spec const& spec) {
        if constexpr (std::is_same_v<Spec, Badge>) {
            return static_cast<bool>(spec.onRemove);
        } else {
            return std::is_same_v<Spec, TextInput> || std::is_same_v<Spec, Button> || std::is_same_v<Spec, Checkbox> ||
                   std::is_same_v<Spec, Select> || std::is_same_v<Spec, Menu> || std::is_same_v<Spec, DateTimeInput> ||
                   std::is_same_v<Spec, Slider> || std::is_same_v<Spec, FilePicker> || std::is_same_v<Spec, Table> ||
                   std::is_same_v<Spec, Tabs>;
        }
    }

    // Records a widget a node mounted as its container's last child. A widget mounted twice over (a cell's content,
    // mounted for its accented or marked cell) keeps its first place.
    void link(reactive::Scope& scope, Widget& widget, ContainerWidget const* parent, bool focusable) {
        // A source table's cells, and an editor in one, are inside the table, which is one stop of the chain.
        if (parent != nullptr && _offChain.contains(parent)) {
            return;
        }
        ChainEntry& entry = _chain[&widget];
        if (entry.linked) {
            entry.takesFocus = entry.takesFocus || focusable;
            return;
        }
        entry.linked = true;
        entry.parent = parent;
        entry.takesFocus = focusable;
        if (parent != nullptr) {
            _chain[parent].children.push_back(&widget);
        } else {
            _chainRoots.push_back(&widget);
        }
        scope.make<ChainLink>(*this, &widget);
    }

    void unlink(Widget* widget) noexcept {
        auto const found = _chain.find(widget);
        if (found == _chain.end()) {
            return;
        }
        Widget const* const parent = found->second.parent;
        found->second.linked = false;
        found->second.parent = nullptr;
        if (found->second.children.empty()) {
            _chain.erase(found);
        }
        std::erase(_chainRoots, widget);
        if (parent == nullptr) {
            return;
        }
        if (auto const above = _chain.find(parent); above != _chain.end()) {
            std::erase(above->second.children, widget);
            if (!above->second.linked && above->second.children.empty()) {
                _chain.erase(above);
            }
        }
    }

    // Brings a container's children in the chain's tree to the order the mount moved them to.
    void reorderChain(Widget const& container, std::vector<Widget*> const& order) {
        auto const found = _chain.find(&container);
        if (found == _chain.end()) {
            return;
        }
        std::vector<Widget*>& children = found->second.children;
        std::vector<Widget*> next;
        next.reserve(children.size());
        for (Widget* const child : order) {
            if (std::ranges::contains(children, child)) {
                next.push_back(child);
            }
        }
        for (Widget* const child : children) {
            if (!std::ranges::contains(next, child)) {
                next.push_back(child);
            }
        }
        children = std::move(next);
    }

    // The widgets that take focus under @p widget, in document order, each with whether it is shown and enabled with
    // every container around it.
    // NOLINTNEXTLINE(misc-no-recursion): a container's children are walked in turn.
    void chainOrder(Widget& widget, bool shown, std::vector<std::pair<Widget*, bool>>& out) const {
        bool here = shown;
        if (auto const flags = _chainFlags.find(&widget); flags != _chainFlags.end()) {
            here = here && flags->second.first && flags->second.second;
        }
        auto const found = _chain.find(&widget);
        if (found == _chain.end()) {
            return;
        }
        if (found->second.takesFocus) {
            out.emplace_back(&widget, here);
        }
        for (Widget* const child : found->second.children) {
            chainOrder(*child, here, out);
        }
    }

    // A container whose children may be sized by a share of its width, registered while it is mounted.
    struct ShareParent {
        ShareParent(std::set<ContainerWidget const*>& owner, ContainerWidget const* added)
            : parents{&owner}, parent{added} {
            parents->insert(parent);
        }
        ShareParent(ShareParent const&) = delete;
        ShareParent& operator=(ShareParent const&) = delete;
        ShareParent(ShareParent&&) = delete;
        ShareParent& operator=(ShareParent&&) = delete;
        ~ShareParent() { parents->erase(parent); }

        std::set<ContainerWidget const*>* parents;
        ContainerWidget const* parent;
    };

    // A widget's entry in the focus map, owned by the widget's scope after the widget, so it leaves the map before
    // the widget is destroyed.
    struct Focusable {
        Focusable(FocusMap& owner, std::optional<FocusMap::iterator> added) noexcept : map{&owner}, entry{added} {}
        Focusable(Focusable const&) = delete;
        Focusable& operator=(Focusable const&) = delete;
        Focusable(Focusable&&) = delete;
        Focusable& operator=(Focusable&&) = delete;
        ~Focusable() { reset(std::nullopt); }

        // Forgets the entry held and holds @p next instead.
        void reset(std::optional<FocusMap::iterator> next) noexcept {
            if (entry) {
                map->erase(*entry);
            }
            entry = next;
        }

        FocusMap* map;
        std::optional<FocusMap::iterator> entry;
    };

    // No predicate accepts every key; a predicate that throws refuses the drop.
    void applyDrop(Widget& widget, Common const& common) {
        std::function<bool(Key const&)> accepts = [](Key const&) { return true; };
        if (common.accepts) {
            accepts = [runtime = _rt, predicate = common.accepts](Key const& key) {
                bool accepted = false;
                static_cast<void>(runCallback(*runtime, [&] { accepted = predicate(key); }));
                return accepted;
            };
        }
        widget.setDropHandler(std::move(accepts), event(common.onDrop));
    }

    // One setKeys for all the chords; a chord listed twice runs its first binding, as a widget callback.
    void applyKeys(Widget& widget, std::vector<KeyBinding> const& keys) {
        std::vector<std::string> chords;
        chords.reserve(keys.size());
        for (KeyBinding const& binding : keys) {
            chords.push_back(binding.chord);
        }
        widget.setKeys(chords, [runtime = _rt, keys](std::string const& chord) {
            auto const bound = std::ranges::find(keys, chord, &KeyBinding::chord);
            if (bound != keys.end() && bound->onPress) {
                static_cast<void>(runCallback(*runtime, bound->onPress));
            }
        });
    }

    void mountChildren(reactive::Scope& scope, std::vector<Node> const& children, ContainerWidget& container) {
        for (Node const& child : children) {
            static_cast<void>(mount(scope, child, &container));
        }
    }

    // The alignment only when it is not the default, as for a button's emphasis, so a stack that says nothing more
    // costs no call.
    Widget& mountStack(reactive::Scope& scope, Axis axis, std::vector<Node> const& children, int gap,
                       Common const& common, ContainerWidget* parent, Align align = Align::Stretch,
                       Justify justify = Justify::Start, bool wrap = false, bool divided = false, Padding padding = {},
                       std::optional<Prop<bool>> const& selected = std::nullopt, bool holder = false) {
        StackWidget& widget = scope.adopt(_backend->createStack(parent, axis));
        if (holder) {
            widget.setPassesBleed(true);
        }
        if (axis == Axis::Horizontal && !wrap && dynamic_cast<TableWidget const*>(parent) == nullptr) {
            scope.make<ShareParent>(_shareParents, &widget);
        }
        if (wrap) {
            widget.setWrap(true);
        }
        if (divided) {
            widget.setDivided(true);
        }
        if (Padding const inset = clampedPadding(padding); inset != Padding{}) {
            widget.setPadding(inset);
        }
        applyCommon(scope, widget, common);
        if (selected) {
            bind(scope, *selected, [&widget](bool chosen) { widget.setSelected(chosen); });
        }
        widget.setGap(spacingStep(gap));
        if (align != Align::Stretch) {
            widget.setAlign(align);
        }
        if (justify != Justify::Start) {
            widget.setJustify(justify);
        }
        if (startsHidden(common) && dynamic_cast<TableWidget const*>(parent) == nullptr) {
            mountWhenShown(scope, children, widget, common.visible);
        } else {
            mountChildren(scope, children, widget);
        }
        return widget;
    }

    // Whether a node's `visible` is false as it mounts.
    static bool startsHidden(Common const& common) {
        return common.visible.isBound() ? !common.visible.evaluate() : !common.visible.constant();
    }

    // The children of a stack that mounts hidden: its own widget exists, and they mount the first time `visible`
    // becomes true, into a scope of their own one level deeper than the stack's. They stay mounted after that, and
    // hiding the stack again only hides it. A `visible` that is a constant `false` never shows, so they never mount.
    void mountWhenShown(reactive::Scope& scope, std::vector<Node> const& children, ContainerWidget& container,
                        Prop<bool> const& visible) {
        if (!visible.isBound()) {
            return;
        }
        reactive::Scope& content = scope.child();
        bool& mounted = scope.make<bool>(false);
        bind(scope, visible, [this, &content, &container, &mounted, children](bool shown) {
            if (!shown || mounted) {
                return;
            }
            mounted = true;
            _rt->untracked([&] {
                mountPass([&] {
                    mountChildren(content, children, container);
                    return 0;
                });
            });
        });
    }

    // A path through a menu tree: an index at each level from the top.
    using MenuPath = std::vector<std::size_t>;

    // What a menu sent its widget: the entries, and for each the path of the item it shows, so a reported path maps
    // back to the item that was on screen even after a binding has hidden or shown another.
    struct ShownMenu {
        std::vector<MenuEntry> entries;
        std::map<MenuPath, MenuPath> sources;

        bool operator==(ShownMenu const&) const = default;
    };

    // Where a level of a menu is: its path among the entries sent, and among the items.
    struct MenuLevelAt {
        MenuPath shown;
        MenuPath source;
    };

    // The visible entries of one level as the widget shows them, each binding read once, recording each one's item
    // path under its shown path in @p sources. A separator that hidden entries leave first, last or next to another
    // separator is left out.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static std::vector<MenuEntry> shownLevel(std::vector<MenuItem> const& items,
                                                           MenuLevelAt const& where,
                                                           std::map<MenuPath, MenuPath>& sources) {
        std::vector<MenuEntry> entries;
        auto const record = [&](std::size_t source) {
            MenuPath shownPath = where.shown;
            shownPath.push_back(entries.size() - 1);
            MenuPath sourcePath = where.source;
            sourcePath.push_back(source);
            sources.insert_or_assign(std::move(shownPath), std::move(sourcePath));
        };
        for (std::size_t source = 0; source < items.size(); ++source) {
            MenuItem const& item = items.at(source);
            if (item.separator) {
                if (!entries.empty() && !entries.back().separator) {
                    entries.push_back(MenuEntry{.separator = true});
                    record(source);
                }
                continue;
            }
            if (!item.visible.evaluate()) {
                continue;
            }
            MenuLevelAt child = where;
            child.shown.push_back(entries.size());
            child.source.push_back(source);
            entries.push_back(MenuEntry{.label = item.label.evaluate(),
                                        .icon = item.icon,
                                        .keys = item.keys,
                                        .checked = item.checked.evaluate(),
                                        .enabled = item.enabled.evaluate(),
                                        .items = shownLevel(item.items, child, sources),
                                        .description = item.description.evaluate()});
            record(source);
        }
        if (!entries.empty() && entries.back().separator) {
            MenuPath last = where.shown;
            last.push_back(entries.size() - 1);
            sources.erase(last);
            entries.pop_back();
        }
        return entries;
    }

    // The visible entries as the widget shows them; called inside the menu's one binding.
    [[nodiscard]] static ShownMenu shownOf(std::vector<MenuItem> const& items) {
        ShownMenu shown;
        shown.entries = shownLevel(items, MenuLevelAt{}, shown.sources);
        return shown;
    }

    // Whether any entry, at any depth, has a bound label, description, check mark, enablement or visibility.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static bool anyBound(std::vector<MenuItem> const& items) {
        return std::ranges::any_of(items, entryBound);
    }

    // Whether this entry, or one in its submenu, has a bound label, description, check mark, enablement or
    // visibility.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static bool entryBound(MenuItem const& item) {
        return item.label.isBound() || item.description.isBound() || item.checked.isBound() ||
               item.enabled.isBound() || item.visible.isBound() || anyBound(item.items);
    }

    // The item a path through the shown entries names, or null when it names none or a separator.
    [[nodiscard]] static MenuItem const* itemAt(std::vector<MenuItem> const& items,
                                                std::map<MenuPath, MenuPath> const& sources, MenuPath const& path) {
        auto const found = sources.find(path);
        if (found == sources.end()) {
            return nullptr;
        }
        std::vector<MenuItem> const* level = &items;
        MenuItem const* item = nullptr;
        for (std::size_t const index : found->second) {
            if (index >= level->size()) {
                return nullptr;
            }
            item = &level->at(index);
            level = &item->items;
        }
        return item == nullptr || item->separator ? nullptr : item;
    }

    /// An input's label and whether it shows it: only when it has a label or shows one, so an input without a label
    /// is told nothing.
    template <class Widget>
    void applyCaption(reactive::Scope& scope, Widget& widget, Prop<std::string> const& label, bool shown) {
        if (!shown && !label.isBound() && label.constant().empty()) {
            return;
        }
        bind(scope, label, [&widget, shown](std::string const& text) { widget.setCaption(text, shown); });
    }

    // Whether a node's spans are given: bound, or a constant with runs.
    [[nodiscard]] static bool hasSpans(Prop<std::vector<TextSpan>> const& spans) {
        return spans.isBound() || !spans.constant().empty();
    }

    // Like applyCommon, a constant default calls no setter.
    void applyField(reactive::Scope& scope, FieldWidget& widget, FieldState const& field) {
        if (field.readonly.isBound() || field.readonly.constant()) {
            bind(scope, field.readonly, [&widget](bool readonly) { widget.setReadOnly(readonly); });
        }
        if (field.required.isBound() || field.required.constant()) {
            bind(scope, field.required, [&widget](bool required) { widget.setRequired(required); });
        }
        if (field.errors.isBound() || !field.errors.constant().empty()) {
            bind(scope, field.errors, [&widget](std::vector<std::string> const& errors) { widget.setErrors(errors); });
        }
        if (field.stale.isBound() || field.stale.constant()) {
            bind(scope, field.stale, [&widget](bool stale) { widget.setStale(stale); });
        }
        if (field.description.isBound() || !field.description.constant().empty()) {
            bind(scope, field.description,
                 [&widget](std::string const& description) { widget.setDescription(description); });
        }
        if (field.labelHint.isBound() || !field.labelHint.constant().empty()) {
            bind(scope, field.labelHint, [&widget](std::string const& hint) { widget.setCaptionHint(hint); });
        }
        if (field.tone.isBound() || field.tone.constant() != Tone::Neutral) {
            bind(scope, field.tone, [&widget](Tone tone) { widget.setTone(tone); });
        }
    }

    Widget& mountKind(reactive::Scope& scope, Text const& spec, ContainerWidget* parent) {
        if (spec.field.description.isBound() || !spec.field.description.constant().empty()) {
            throw std::logic_error{"morph::ui: a text is not described; put the line in the text"};
        }
        if (spec.field.labelHint.isBound() || !spec.field.labelHint.constant().empty()) {
            throw std::logic_error{"morph::ui: a text has no caption to hint after; put the hint in the text"};
        }
        TextWidget& widget = scope.adopt(_backend->createText(parent));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        // Spans replace the text, so a text that has them is never told its plain text.
        if (hasSpans(spec.spans)) {
            bind(scope, spec.spans, [&widget](std::vector<TextSpan> const& spans) { widget.setSpans(spans); });
        } else {
            bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
        }
        bind(scope, spec.role, [&widget](TextRole role) { widget.setRole(role); });
        if (spec.align) {
            widget.setAlign(*spec.align);
        }
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Button const& spec, ContainerWidget* parent) {
        if (spec.style == ButtonStyle::Push && spec.child != nullptr) {
            throw std::logic_error{"morph::ui: only a list entry holds content; a push button has a label"};
        }
        ButtonWidget& widget = scope.adopt(_backend->createButton(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        if (spec.emphasis != ButtonEmphasis::Normal) {
            widget.setEmphasis(spec.emphasis);
        }
        if (!spec.icon.empty()) {
            widget.setIcon(spec.icon);
        }
        if (spec.style == ButtonStyle::Item) {
            widget.setStyle(spec.style);
        }
        if (spec.shape != ControlShape::Default) {
            widget.setShape(spec.shape);
        }
        if (spec.size != ButtonSize::Normal) {
            widget.setSize(spec.size);
        }
        if (spec.mono) {
            widget.setMono(true);
        }
        if (spec.weight != 0) {
            widget.setWeight(spec.weight);
        }
        if (spec.selected.isBound() || spec.selected.constant()) {
            bind(scope, spec.selected, [&widget](bool selected) { widget.setSelected(selected); });
        }
        widget.setOnClick(event(spec.onClick));
        if (spec.child != nullptr) {
            static_cast<void>(mount(scope, spec.child, &widget.content()));
        }
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, TextInput const& spec, ContainerWidget* parent) {
        bool const hasUnit = spec.unit.isBound() || !spec.unit.constant().empty();
        bool const hasPrefix = spec.prefix.isBound() || !spec.prefix.constant().empty();
        bool const hasDecimals = spec.decimals.isBound() || spec.decimals.constant() >= 0;
        if ((hasUnit || hasPrefix || hasDecimals || !spec.icon.empty()) && spec.mode == TextInputMode::Multiline) {
            throw std::logic_error{
                "morph::ui: a multiline text input holds no number, so it takes no unit, prefix, places or icon"};
        }
        if (spec.align != Align::Start && spec.mode == TextInputMode::Multiline) {
            throw std::logic_error{"morph::ui: a multiline text input's text starts at the leading edge"};
        }
        if (spec.emphasis != InputEmphasis::Normal && spec.mode == TextInputMode::Multiline) {
            throw std::logic_error{"morph::ui: a multiline text input's size is its own; it takes no emphasis"};
        }
        TextInputWidget& widget = scope.adopt(_backend->createTextInput(parent, spec.mode));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        auto reassert = controlled(scope, spec.value, [&widget](std::string const& value) { widget.setText(value); });
        bind(scope, spec.placeholder, [&widget](std::string const& text) { widget.setPlaceholder(text); });
        // An input without a unit or places is not told any, so a backend sees only what a field declares.
        if (hasUnit) {
            bind(scope, spec.unit, [&widget](std::string const& unit) { widget.setUnit(unit); });
        }
        if (hasPrefix) {
            bind(scope, spec.prefix, [&widget](std::string const& prefix) { widget.setPrefix(prefix); });
        }
        if (hasDecimals) {
            bind(scope, spec.decimals, [&widget](int decimals) { widget.setDecimals(decimals); });
        }
        if (!spec.icon.empty()) {
            widget.setIcon(spec.icon);
        }
        if (spec.emphasis != InputEmphasis::Normal) {
            widget.setEmphasis(spec.emphasis);
        }
        if (spec.mono) {
            widget.setMono(true);
        }
        if (!spec.wrap) {
            widget.setWrap(false);
        }
        if (spec.align != Align::Start) {
            widget.setAlign(spec.align);
        }
        applyCaption(scope, widget, spec.label, spec.showLabel);
        widget.setOnChange(inputEvent(spec.onChange, reassert));
        // An empty submit handler tells the backend the field does not submit, so Enter goes on to its ancestors'
        // chords (a dialog's default) once the field has committed.
        widget.setOnSubmit(spec.onSubmit ? inputEvent(spec.onSubmit, std::move(reassert))
                                         : std::function<void(std::string)>{});
        widget.setOnCommit(event(spec.onCommit));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Checkbox const& spec, ContainerWidget* parent) {
        CheckboxWidget& widget = scope.adopt(_backend->createCheckbox(parent));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        widget.setOnToggle(inputEvent(
            spec.onToggle, controlled(scope, spec.checked, [&widget](bool checked) { widget.setChecked(checked); })));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Select const& spec, ContainerWidget* parent) {
        SelectWidget& widget = scope.adopt(_backend->createSelect(parent, spec.style));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        bind(scope, spec.options, [&widget](std::vector<SelectOption> const& options) { widget.setOptions(options); });
        widget.setOnSelect(inputEvent(
            spec.onSelect,
            controlled(scope, spec.selected, [&widget](std::optional<Key> const& key) { widget.setSelected(key); })));
        widget.setOnOpen(event(spec.onOpen));
        applyCaption(scope, widget, spec.label, spec.showLabel);
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Menu const& spec, ContainerWidget* parent) {
        MenuWidget& widget = scope.adopt(_backend->createMenu(parent, spec.style));
        applyCommon(scope, widget, spec.common);
        if (spec.style == MenuStyle::Button) {
            bind(scope, spec.label, [&widget, icon = spec.icon, emphasis = spec.emphasis](std::string const& label) {
                widget.setTrigger(MenuTrigger{.label = label, .icon = icon, .emphasis = emphasis});
            });
        }
        std::vector<MenuItem> const& items = spec.items;
        // One prop for the whole tree: a bound label, description, mark, enablement or visibility anywhere re-sends
        // every entry, which keeps setItems the one setter. A reported path is read against what was last sent.
        auto sent = std::make_shared<std::map<MenuPath, MenuPath>>();
        Prop<ShownMenu> const shown = anyBound(items) ? Prop<ShownMenu>([items = items] { return shownOf(items); })
                                                      : Prop<ShownMenu>(shownOf(items));
        bind(scope, shown, [&widget, sent](ShownMenu const& menu) {
            *sent = menu.sources;
            widget.setItems(menu.entries);
        });
        widget.setOnActivate(event(
            std::function<void(std::vector<std::size_t>)>{[items = items, sent](std::vector<std::size_t> const& path) {
                MenuItem const* const item = itemAt(items, *sent, path);
                if (item != nullptr && item->onSelect) {
                    item->onSelect();
                }
            }}));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Column const& spec, ContainerWidget* parent) {
        return mountStack(scope, Axis::Vertical, spec.children, spec.gap, spec.common, parent, spec.align,
                          spec.justify, false, spec.divided, spec.padding, spec.selected, spec.holder);
    }

    Widget& mountKind(reactive::Scope& scope, Row const& spec, ContainerWidget* parent) {
        if ((spec.tone || spec.label || spec.heading || spec.spans) && dynamic_cast<TableWidget*>(parent) == nullptr) {
            throw std::logic_error{
                "morph::ui: a row's tone, label, heading and spans are a table row's; this row is not in a table"};
        }
        if (spec.selected && dynamic_cast<TableWidget*>(parent) != nullptr) {
            throw std::logic_error{"morph::ui: a table row's selection is the table's; this row gives its own"};
        }
        return mountStack(scope, Axis::Horizontal, spec.children, spec.gap, spec.common, parent, spec.align,
                          spec.justify, spec.wrap, spec.divided && !spec.wrap, spec.padding, spec.selected);
    }

    Widget& mountKind(reactive::Scope& scope, Grid const& spec, ContainerWidget* parent) {
        GridWidget& widget = scope.adopt(_backend->createGrid(parent));
        applyCommon(scope, widget, spec.common);
        checkNoShare(spec.tracks);
        if (spec.tracks.empty()) {
            widget.setColumns(spec.columns);
        } else {
            widget.setTracks(spec.tracks);
        }
        if (spec.minColumnWidth > 0 && spec.tracks.empty()) {
            widget.setMinColumnWidth(spec.minColumnWidth);
        }
        widget.setGap(spacingStep(spec.gap));
        if (spec.align != Align::Stretch) {
            widget.setAlign(spec.align);
        }
        for (GridCell const& cell : spec.cells) {
            Widget* const child = mount(scope, cell.node, &widget);
            if (child != nullptr && cell.span != 1) {
                widget.setSpan(*child, cell.span);
            }
        }
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Spacer const& spec, ContainerWidget* parent) {
        SpacerWidget& widget = scope.adopt(_backend->createSpacer(parent));
        applyCommon(scope, widget, spec.common);
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Panel const& spec, ContainerWidget* parent) {
        PanelWidget& widget = scope.adopt(_backend->createPanel(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        if (Widget* const header = mount(scope, spec.header, &widget); header != nullptr) {
            widget.setHeader(*header);
        }
        widget.setPadding(spacingStep(spec.padding));
        widget.setCollapsible(spec.collapsible);
        if (spec.collapsible) {
            widget.setOnToggle(inputEvent(spec.onToggle, controlled(scope, spec.collapsed, [&widget](bool collapsed) {
                                              widget.setCollapsed(collapsed);
                                          })));
        }
        static_cast<void>(mount(scope, spec.child, &widget));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Scroll const& spec, ContainerWidget* parent) {
        ScrollWidget& widget = scope.adopt(_backend->createScroll(parent, spec.axis));
        applyCommon(scope, widget, spec.common);
        static_cast<void>(mount(scope, spec.child, &widget));
        return widget;
    }

    // Content a binding mounts (a Switch case, Dialog content), all or nothing: it is built in a scope of its own,
    // which the caller takes only once the mount completed, so content that throws partway leaves nothing behind.
    // Built untracked, so the binding's Effect depends on its own prop alone. The scope is one level deeper than the
    // one that owns the binding, so the owner's Effects run before the content's.
    [[nodiscard]] std::unique_ptr<reactive::Scope> mountContent(std::size_t depth, Node const& node,
                                                                ContainerWidget& host) {
        return mountPass([&] {
            auto content = std::make_unique<reactive::Scope>(*_rt, depth);
            _rt->untracked([&] { static_cast<void>(mount(*content, node, &host)); });
            return content;
        });
    }

    // reset() tears the old case down, newest first, before the new one is built. The Computed behind a bound
    // selector is equality-gated, so an unchanged key never gets here. A case that fails to mount leaves nothing for
    // its key; the next change of the key mounts again.
    Widget& mountKind(reactive::Scope& scope, Switch const& spec, ContainerWidget* parent) {
        SlotWidget& widget = scope.adopt(_backend->createSlot(parent));
        applyCommon(scope, widget, spec.common);
        auto& content = scope.make<std::unique_ptr<reactive::Scope>>();
        bind(scope, spec.selector,
             [this, &widget, &content, depth = scope.depth() + 1, cases = spec.cases, fallback = spec.fallback,
              overlay = _overlay](Key const& key) {
                 content.reset();
                 auto const chosen = std::ranges::find(cases, key, &SwitchCase::key);
                 Node const& node = chosen == cases.end() ? fallback : chosen->node;
                 if (node) {
                     content = underOverlay(overlay, [&] { return mountContent(depth, node, widget); });
                 }
             });
        return widget;
    }

    // Every page is owned by `pageScope`, a child scope made before the binding, so pages are destroyed newest first,
    // after the binding that shows them and before the tab bar. A scope fixes that order; a vector's element
    // destruction order differs between standard libraries.
    //
    // A bound index is not equality-gated: the Effect reads the binding itself, not a Computed. After a failed page,
    // the application's onSelect writes the index back inside this Effect's own run; a Computed would keep the failed
    // index and find the user's next pick of that tab unchanged, while the Effect's dependency on the binding's own
    // signals sees it as the change it is. The cost is that showPage also runs when something the binding reads
    // changes but the index does not: for a page that is mounted it only re-sends the highlight, and for a page that
    // failed it tries the mount again, once per run.
    Widget& mountKind(reactive::Scope& scope, Tabs const& spec, ContainerWidget* parent) {
        TabsWidget& widget = scope.adopt(_backend->createTabs(parent));
        applyCommon(scope, widget, spec.common);
        if (spacingStep(spec.inset) != 0) {
            widget.setInset(spacingStep(spec.inset));
        }
        if (spec.pageInset) {
            widget.setPageInset(spacingStep(*spec.pageInset));
        }
        if (!spec.stripSurface.empty()) {
            widget.setStripSurface(spec.stripSurface);
        }
        // One prop over every caption, as a menu's entries: a bound caption re-sends them all. The count never
        // changes, so the widget keeps its highlight, the shown page and the focus across a relabel.
        std::vector<Prop<std::string>> captions;
        captions.reserve(spec.tabs.size());
        for (Tab const& tab : spec.tabs) {
            captions.push_back(tab.label);
        }
        auto labelsOf = [captions] {
            std::vector<std::string> labels;
            labels.reserve(captions.size());
            for (Prop<std::string> const& caption : captions) {
                labels.push_back(caption.evaluate());
            }
            return labels;
        };
        Prop<std::vector<std::string>> const labels = std::ranges::any_of(captions, &Prop<std::string>::isBound)
                                                          ? Prop<std::vector<std::string>>(std::move(labelsOf))
                                                          : Prop<std::vector<std::string>>(labelsOf());
        bind(scope, labels, [&widget](std::vector<std::string> const& shown) { widget.setTabs(shown); });
        if (spec.moreLabel.isBound() || spec.moreLabel.constant() != "More") {
            bind(scope, spec.moreLabel, [&widget](std::string const& label) { widget.setMoreLabel(label); });
        }
        auto& pageScope = scope.child();
        auto& pages = scope.make<Pages>();
        pages.pageSlots.resize(spec.tabs.size(), nullptr);
        // A bound index is controlled: after a pick, the bar highlights the page that is shown, so a pick the
        // document refuses moves the highlight back. A constant index is not a slot, and the bar keeps the pick.
        std::function<void(std::size_t const&)> reassert;
        if (spec.selected.isBound()) {
            reassert = reasserter<std::size_t>(
                scope, pages.highlighted,
                [&pages] { return pages.shown.value_or(std::numeric_limits<std::size_t>::max()); },
                [&widget, &pages](std::size_t index) { highlight(widget, pages, index); });
        }
        widget.setOnSelect(inputEvent(spec.onSelect, reassert));
        pages.onSelect = inputEvent(spec.onSelect, std::move(reassert));
        auto show = [this, &widget, &pageScope, &pages, pageSpecs = spec.tabs, overlay = _overlay](std::size_t index) {
            _rt->untracked([&] { showPage(widget, pageScope, pages, pageSpecs, index, overlay); });
        };
        if (!spec.selected.isBound()) {
            show(spec.selected.constant());
        } else {
            scope.effect([binding = spec.selected.binding(), show = std::move(show)] { show(binding()); });
        }
        return widget;
    }

    // A page is mounted the first time its tab is selected and only hidden afterwards, so its widgets keep their
    // state (focus, scroll position, half-typed text) across tab switches. A new page is complete before anything
    // else changes: one that fails to mount leaves the previous page shown and selected, and the bar's highlight,
    // which the user's pick may already have moved, goes back to it. The next change of the index mounts again.
    //
    // Only the mount runs under the tabs' overlay, and that overlay is put back before a failure is reported:
    // reporting it may destroy the Mounted, and with it this Mounter.
    void showPage(TabsWidget& widget, reactive::Scope& pageScope, Pages& pages, std::vector<Tab> const& pageSpecs,
                  std::size_t index, ContainerWidget* overlay) {
        bool const exists = index < pageSpecs.size();
        bool const isNew = exists && pages.pageSlots.at(index) == nullptr;
        if (isNew) {
            try {
                underOverlay(overlay, [&] { mountPage(widget, pageScope, pages, pageSpecs.at(index).node, index); });
            } catch (...) {
                if (pages.shown.has_value()) {
                    restoreSelection(widget, pages, *pages.shown);
                }
                throw;
            }
        }
        if (pages.shown != index) {
            if (pages.shown.has_value() && *pages.shown < pages.pageSlots.size()) {
                if (SlotWidget* const previous = pages.pageSlots.at(*pages.shown); previous != nullptr) {
                    previous->setVisible(false);
                }
            }
            if (exists && !isNew) {
                pages.pageSlots.at(index)->setVisible(true);
            }
        }
        pages.shown = index;
        highlight(widget, pages, index);
    }

    // Moves the bar's highlight, and records it.
    static void highlight(TabsWidget& widget, Pages& pages, std::size_t index) {
        widget.setSelected(index);
        pages.highlighted = index;
    }

    // After a page failed to mount: puts the bar's highlight back on the page still shown, and tells the application
    // through `onSelect`, so its index matches the widget and selecting the failed tab again is a change that mounts
    // again.
    //
    // onSelect runs last, from a copy: it is a widget callback, which may destroy the Mounted and with it `pages`,
    // the widget and the Effect that called this. Nothing after it touches any of them; the caller only rethrows.
    //
    // Nothing here may replace the failure being reported: the callback's wrapper reports what the callback throws,
    // and anything else thrown here is dropped.
    static void restoreSelection(TabsWidget& widget, Pages& pages, std::size_t shown) noexcept {
        try {
            highlight(widget, pages, shown);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // The page's failure is the one reported; the bar keeps whatever highlight it had.
        }
        try {
            std::function<void(std::size_t)> const onSelect = pages.onSelect;
            if (onSelect) {
                onSelect(shown);
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Only copying the wrapper can throw here; the application then keeps the failed index.
        }
    }

    // The page is built in a scope of its own and handed to `pageScope` once complete, so one that fails to mount
    // leaves nothing behind.
    void mountPage(TabsWidget& widget, reactive::Scope& pageScope, Pages& pages, Node const& node, std::size_t index) {
        auto page = mountPass([&] {
            auto built = std::make_unique<reactive::Scope>(*_rt, pageScope.depth());
            SlotWidget& slot = built->adopt(_backend->createSlot(&widget));
            // A page fills the area under the strip, pads itself by the node's `pageInset` and scrolls itself while
            // its content overflows, as a workspace's page does.
            slot.setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
            slot.setScrollable(true);
            static_cast<void>(mount(*built, node, &slot));
            return std::pair{std::move(built), &slot};
        });
        pageScope.adopt(std::move(page.first));
        pages.pageSlots.at(index) = page.second;
    }

    // The content is built before the overlay opens and torn down before it closes. Content that fails to mount
    // under a bound `open` leaves the dialog closed and calls `onDismiss`, so the application's `open` follows the
    // widget and opening it again is a change that mounts again.
    //
    // A bound `open` is not equality-gated, for the reason a Tabs index is not: onDismiss writes `open` inside this
    // Effect's own run, which a Computed would miss, and then find the next opening unchanged. An Effect that runs
    // with `open` unchanged re-sends `setOpen`, and mounts content only if none is mounted: once per run while `open`
    // stays true over content that failed.
    Widget& mountKind(reactive::Scope& scope, Dialog const& spec, ContainerWidget* parent) {
        checkOverlayLayout(spec.common, "dialog");
        DialogWidget& widget = scope.adopt(_backend->createDialog(parent));
        applyCommon(scope, widget, spec.common);
        if (spec.sheet.isBound() || spec.sheet.constant()) {
            bind(scope, spec.sheet, [&widget](bool sheet) { widget.setSheet(sheet); });
        }
        return mountOverlay(scope, spec, widget);
    }

    Widget& mountKind(reactive::Scope& scope, Drawer const& spec, ContainerWidget* parent) {
        checkOverlayLayout(spec.common, "drawer");
        DrawerWidget& widget = scope.adopt(_backend->createDrawer(parent, spec.side));
        applyCommon(scope, widget, spec.common);
        return mountOverlay(scope, spec, widget);
    }

    // A Dialog or a Drawer: the title, the dismissal, and content that exists only while open.
    template <typename Spec, typename OverlayWidget>
    Widget& mountOverlay(reactive::Scope& scope, Spec const& spec, OverlayWidget& widget) {
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        // Runs replace the plain description, so an overlay that has them is never told its plain one.
        if (hasSpans(spec.descriptionSpans)) {
            bind(scope, spec.descriptionSpans,
                 [&widget](std::vector<TextSpan> const& spans) { widget.setDescriptionSpans(spans); });
        } else if (spec.description.isBound() || !spec.description.constant().empty()) {
            bind(scope, spec.description,
                 [&widget](std::string const& description) { widget.setDescription(description); });
        }
        bindOverlayChrome(scope, spec, widget);
        auto& content = scope.make<std::unique_ptr<reactive::Scope>>();
        auto& shownOpen = scope.make<std::optional<bool>>();
        // A bound `open` is controlled: a dismissal the document refuses opens the dialog again, over the content it
        // still holds. Content that failed to mount is not there to show, so the dialog stays closed for it.
        std::function<void()> onDismiss = event(spec.onDismiss);
        if (spec.open.isBound()) {
            auto reassert =
                reasserter<bool>(scope, shownOpen, spec.open.binding(), [&widget, &content, &shownOpen](bool open) {
                    if (!open || content != nullptr) {
                        widget.setOpen(open);
                        shownOpen = open;
                    }
                });
            onDismiss = [inner = std::move(onDismiss), reassert = std::move(reassert)] {
                if (inner) {
                    inner();
                }
                reassert(false);
            };
        }
        widget.setOnDismiss(onDismiss);
        auto show = [this, &widget, &content, &shownOpen, depth = scope.depth() + 1, child = spec.child](
                        bool open, std::function<void()> const& dismiss) {
            if (!open) {
                content.reset();
            } else if (content == nullptr) {
                try {
                    content = underOverlay(&widget, [&] { return mountContent(depth, child, widget); });
                } catch (...) {
                    dismissFailed(dismiss);
                    throw;
                }
            }
            widget.setOpen(open);
            shownOpen = open;
        };
        if (!spec.open.isBound()) {
            // A constant has no application state to bring back in line: content that fails throws out of the mount.
            show(spec.open.constant(), {});
        } else {
            scope.effect([binding = spec.open.binding(), show = std::move(show), onDismiss = std::move(onDismiss)] {
                show(binding(), onDismiss);
            });
        }
        return widget;
    }

    // An overlay's notes, close control and dismiss label: each is told only when the document sets it, so a backend
    // keeps its own default for the rest.
    template <typename Spec, typename OverlayWidget>
    void bindOverlayChrome(reactive::Scope& scope, Spec const& spec, OverlayWidget& widget) {
        if (spec.headNote.isBound() || !spec.headNote.constant().empty()) {
            bind(scope, spec.headNote, [&widget](std::string const& note) { widget.setHeadNote(note); });
        }
        if (spec.footerNote.isBound() || !spec.footerNote.constant().empty()) {
            bind(scope, spec.footerNote, [&widget](std::string const& note) { widget.setFooterNote(note); });
        }
        if (spec.closeButton.isBound() || spec.closeButton.constant()) {
            bind(scope, spec.closeButton, [&widget](bool shown) { widget.setCloseButton(shown); });
        }
        if (spec.dismissLabel.isBound() || spec.dismissLabel.constant() != "Close") {
            bind(scope, spec.dismissLabel, [&widget](std::string const& label) { widget.setDismissLabel(label); });
        }
    }

    // After a Dialog's content failed to mount: tells the application through `onDismiss` that the dialog is closed.
    // It runs last, from a copy: it is a widget callback, which may destroy the Mounted and with it the dialog and
    // the Effect that called this; the caller only rethrows after it. Nothing thrown here may replace the failure
    // being reported.
    static void dismissFailed(std::function<void()> const& onDismiss) noexcept {
        try {
            std::function<void()> const dismiss = onDismiss;
            if (dismiss) {
                dismiss();
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Only copying the wrapper can throw here; the application then keeps `open` true.
        }
    }

    Widget& mountKind(reactive::Scope& scope, ForEach const& spec, ContainerWidget* parent) {
        StackWidget& widget = scope.adopt(_backend->createStack(parent, spec.axis));
        // A horizontal ForEach directly in an action bar is part of the row of actions, so its rows are the action
        // gap apart, whatever gap the ForEach says.
        bool const actionRow = spec.axis == Axis::Horizontal && _actionBar != nullptr && parent == _actionBar;
        if (actionRow) {
            widget.setActionRow();
        }
        if (spec.axis == Axis::Horizontal && !spec.wrap) {
            scope.make<ShareParent>(_shareParents, &widget);
        }
        if (spec.wrap && spec.axis == Axis::Horizontal) {
            widget.setWrap(true);
        }
        if (spec.divided && spec.axis == Axis::Vertical) {
            widget.setDivided(true);
        }
        applyCommon(scope, widget, spec.common);
        widget.setGap(spacingStep(actionRow ? 2 : spec.gap));
        mountRows(scope, spec.model, widget, {}, nullptr);
        return widget;
    }

    // What a table remembers to resolve its rows' spans: each row's request, the rows in order, and the table's
    // conditions. The resolve runs after a reconcile has brought the rows into order and when one row's spans or the
    // sort change on their own; a row's binding that fires inside a reconcile only stores its request.
    struct SpanTable {
        struct Placed {
            Key key;
            Widget* widget = nullptr;
        };
        std::unordered_map<Key, RowSpanEntry> rows;
        std::vector<Placed> order;
        std::size_t columns = 0;
        std::size_t threshold = 0;
        // Each column's visible place, from the arrangement; empty before the first.
        std::vector<std::optional<std::size_t>> placeOf;
        std::size_t reconciling = 0;
        bool sorted = false;
        bool anyAsked = false;
        // Whether the table is windowed whatever its realized rows count: a table over a source above its threshold.
        bool windowed = false;
        std::set<std::string> lastRefused;
    };

    // The resolve of a table's spans: what the widget is told, by the difference from what it last was, and the
    // refusals, reported when their set changes.
    void resolveSpans(TableWidget& widget, SpanTable& table) {
        if (table.reconciling != 0 || !table.anyAsked) {
            return;
        }
        std::vector<SpanRow> rows;
        rows.reserve(table.order.size());
        for (SpanTable::Placed const& placed : table.order) {
            auto const found = table.rows.find(placed.key);
            SpanRow row{.name =
                            "row " + std::visit([](auto const& key) { return std::format("{}", key); }, placed.key)};
            if (found != table.rows.end()) {
                row.heading = found->second.heading;
                row.requested = found->second.requested;
            }
            rows.push_back(std::move(row));
        }
        bool const virtualized = table.windowed || table.order.size() >= table.threshold;
        ResolvedSpans resolved;
        if (table.sorted) {
            resolved.effective.resize(rows.size());
        } else if (virtualized) {
            resolved.effective.resize(rows.size());
            if (std::ranges::any_of(rows, [](SpanRow const& row) {
                    return std::ranges::any_of(
                        row.requested, [](RowSpan const& span) { return span.rows >= 2 || span.columns >= 2; });
                })) {
                resolved.refused.emplace_back(site::kRowSpanVirtualized, "the table is virtualized");
            }
        } else {
            resolved = resolveRowSpans(rows, table.columns, table.placeOf);
        }
        std::set<std::string> refused;
        for (auto const& [reason, text] : resolved.refused) {
            refused.insert(std::string{reason} + " | " + text);
        }
        if (refused != table.lastRefused) {
            std::set<char const*> sites;
            for (auto const& [reason, text] : resolved.refused) {
                sites.insert(reason);
            }
            for (char const* const reason : sites) {
                _rt->core()->report(reason);
            }
            table.lastRefused = std::move(refused);
        }
        for (std::size_t index = 0; index < table.order.size(); ++index) {
            SpanTable::Placed const& placed = table.order.at(index);
            auto const found = table.rows.find(placed.key);
            if (placed.widget == nullptr || found == table.rows.end()) {
                continue;
            }
            RowSpanEntry& entry = found->second;
            if (entry.told != placed.widget) {
                entry.told = placed.widget;
                entry.sent.clear();
            }
            if (resolved.effective.at(index) != entry.sent) {
                entry.sent = resolved.effective.at(index);
                widget.setRowSpans(*placed.widget, entry.sent);
            }
        }
    }

    // A caller's menu entry as a menu that is open holds it: what choosing it runs, and its submenu's entries.
    struct OpenItem {
        Action onSelect;
        std::vector<OpenItem> items;
    };

    // Reads @p items once, as a `Menu` shows them: a hidden entry is left out, and so is a separator it leaves at
    // either end of its level or next to another. Each entry shown is appended to @p shown, and what it runs to @p
    // kept at the same place.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    static void readMenuItems(std::vector<MenuItem> const& items, std::vector<MenuEntry>& shown,
                              std::vector<OpenItem>& kept) {
        for (MenuItem const& item : items) {
            if (item.separator) {
                if (!shown.empty() && !shown.back().separator) {
                    shown.push_back(MenuEntry{.separator = true});
                    kept.emplace_back();
                }
                continue;
            }
            if (!item.visible.evaluate()) {
                continue;
            }
            MenuEntry entry{.label = item.label.evaluate(),
                            .icon = item.icon,
                            .keys = item.keys,
                            .checked = item.checked.evaluate(),
                            .enabled = item.enabled.evaluate(),
                            .description = item.description.evaluate()};
            OpenItem open{.onSelect = item.onSelect, .items = {}};
            readMenuItems(item.items, entry.items, open.items);
            shown.push_back(std::move(entry));
            kept.push_back(std::move(open));
        }
        if (!shown.empty() && shown.back().separator) {
            shown.pop_back();
            kept.pop_back();
        }
    }

    // The entry a path names among @p kept; null when the path leaves them.
    [[nodiscard]] static OpenItem const* openItemAt(std::vector<OpenItem> const& kept,
                                                    std::span<std::size_t const> path) {
        OpenItem const* found = nullptr;
        std::vector<OpenItem> const* level = &kept;
        for (std::size_t const index : path) {
            if (index >= level->size()) {
                return nullptr;
            }
            found = &level->at(index);
            level = &found->items;
        }
        return found;
    }

    // The menus a table has open, as last read: a header's, with the caller's entries after the built-in ones, and a
    // cell's. A choice is routed by what the menu held when it opened.
    struct OpenMenus {
        std::optional<std::size_t> headerColumn;
        // Where the caller's entries start in the header's menu, past the built-in entries and their separator.
        std::size_t headerOffset = 0;
        std::size_t builtIn = 0;
        std::vector<OpenItem> header;
        std::optional<std::pair<Key, std::size_t>> cell;
        std::vector<OpenItem> cellItems;
    };

    // What a user configures a table with, whatever its rows come from: menus, moves, resizes, filters, groups and
    // copy. A value left at its empty constant is not told, so a table that configures nothing is told nothing.
    // @p focusCell focuses a cell whose menu opens; empty for a table whose focus the mount does not keep.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one seam per configuring callback.
    void bindTableConfig(reactive::Scope& scope, Table const& spec, TableWidget& widget,
                         std::function<void(Key const&, std::size_t)> focusCell = {}) {
        auto const open = std::make_shared<OpenMenus>();
        if (spec.menu || spec.headerEntries) {
            // The entries are read when a menu opens, as a callback is: untracked, an exception reported. While the
            // header is disabled every built-in entry of a column's menu is, since each sorts, filters or arranges.
            widget.setMenu(
                [runtime = _rt, entries = spec.menu, extra = spec.headerEntries, enabled = spec.headerEnabled,
                 open](std::optional<std::size_t> column) {
                    std::vector<MenuEntry> shown;
                    static_cast<void>(runCallback(*runtime, [&] {
                        if (entries) {
                            shown = entries(column);
                        }
                        if (!column) {
                            return;
                        }
                        if (!enabled.evaluate()) {
                            for (MenuEntry& entry : shown) {
                                entry.enabled = false;
                            }
                        }
                        open->headerColumn = column;
                        open->builtIn = shown.size();
                        open->header.clear();
                        if (!extra) {
                            return;
                        }
                        std::vector<MenuEntry> added;
                        readMenuItems(extra(*column), added, open->header);
                        if (added.empty()) {
                            return;
                        }
                        if (!shown.empty()) {
                            shown.push_back(MenuEntry{.separator = true});
                        }
                        open->headerOffset = shown.size();
                        std::ranges::move(added, std::back_inserter(shown));
                    }));
                    return shown;
                },
                [runtime = _rt, chosen = event(spec.onMenu), open](std::optional<std::size_t> column,
                                                                   std::vector<std::size_t> path) {
                    bool const builtIn = !column || open->headerColumn != column || path.empty() ||
                                         path.front() < open->builtIn || open->header.empty();
                    if (builtIn) {
                        if (chosen) {
                            chosen(column, std::move(path));
                        }
                        return;
                    }
                    if (path.front() < open->headerOffset) {
                        return;
                    }
                    path.front() -= open->headerOffset;
                    OpenItem const* const item = openItemAt(open->header, path);
                    if (item != nullptr && item->onSelect && item->items.empty()) {
                        Action const run = item->onSelect;
                        static_cast<void>(runCallback(*runtime, run));
                    }
                });
        }
        if (spec.cellMenu) {
            widget.setCellMenu(
                [runtime = _rt, entries = spec.cellMenu, open, focusCell](Key const& row, std::size_t column) {
                    std::vector<MenuEntry> shown;
                    static_cast<void>(runCallback(*runtime, [&] {
                        if (focusCell) {
                            focusCell(row, column);
                        }
                        open->cell = std::pair{row, column};
                        open->cellItems.clear();
                        readMenuItems(entries(row, column), shown, open->cellItems);
                    }));
                    return shown;
                },
                [runtime = _rt, open](Key const& row, std::size_t column, std::vector<std::size_t> const& path) {
                    if (open->cell != std::pair{row, column}) {
                        return;
                    }
                    OpenItem const* const item = openItemAt(open->cellItems, path);
                    if (item != nullptr && item->onSelect && item->items.empty()) {
                        Action const run = item->onSelect;
                        static_cast<void>(runCallback(*runtime, run));
                    }
                });
            widget.setCellMenuKeys(spec.cellMenuKeys);
        }
        if (spec.onColumnMove) {
            widget.setOnColumnMove(event(spec.onColumnMove));
        }
        if (spec.onColumnResize) {
            widget.setOnColumnResize(event(spec.onColumnResize));
        }
        if (spec.onFilter) {
            widget.setOnFilter(event(spec.onFilter));
        }
        if (spec.filtered.isBound() || !spec.filtered.constant().empty()) {
            bind(scope, spec.filtered,
                 [&widget](std::vector<std::size_t> const& columns) { widget.setFiltered(columns); });
        }
        if (spec.columnGroups.isBound() || !spec.columnGroups.constant().empty()) {
            bind(scope, spec.columnGroups,
                 [&widget](std::vector<TableColumnGroup> const& groups) { widget.setColumnGroups(groups); });
        }
        if (spec.onGroupFilter) {
            widget.setOnGroupFilter(event(spec.onGroupFilter));
        }
        if (spec.onCopy) {
            widget.setOnCopy(event(spec.onCopy));
        }
    }

    // A selection of the view and the status line's entries. While `selectionView` is engaged the backend marks every
    // row but its keys; when it disengages the selection is given again, since the one the backend showed before the
    // view may equal the one it ends on, which would not be sent again.
    void bindSelectionView(reactive::Scope& scope, Table const& spec, TableWidget& widget, bool& viewing) {
        if (spec.onStatusEntry) {
            widget.setOnStatusEntry(event(spec.onStatusEntry));
        }
        if (!spec.selectionView.isBound() && !spec.selectionView.constant()) {
            return;
        }
        bind(scope, spec.selectionView,
             [this, &widget, &viewing, selection = spec.selection](std::optional<std::vector<Key>> const& except) {
                 if (except) {
                     viewing = true;
                     widget.setSelectionView(*except);
                     return;
                 }
                 if (!viewing) {
                     return;
                 }
                 viewing = false;
                 widget.setSelection(selection.isBound() ? _rt->untracked([&] { return selection.binding()(); })
                                                         : selection.constant());
             });
    }

    // Rows first, then the selection. The widget marks a selected key whenever its row arrives, so the order only
    // shapes the log: the first setSelection comes after the rows and already marks them.
    Widget& mountKind(reactive::Scope& scope, Table const& spec, ContainerWidget* parent) {
        if (spec.source) {
            return mountSourceTable(scope, spec, parent);
        }
        TableWidget& widget = scope.adopt(_backend->createTable(parent));
        applyCommon(scope, widget, spec.common);
        checkNoShare(spec.columns);
        widget.setColumns(spec.columns);
        widget.setShowHeader(spec.showHeader);
        widget.setSelectionMode(spec.selectionMode);
        if (spec.selectColumn) {
            widget.setSelectColumn(true);
            bind(scope, spec.selectAllLabel, [&widget](std::string const& label) { widget.setSelectAllLabel(label); });
        }
        widget.setOnActivate(event(spec.onActivate));
        if (spec.onSelectAll) {
            widget.setOnSelectAll(event(spec.onSelectAll));
        }
        bool const sortable = std::ranges::any_of(spec.columns, &TableColumn::sortable);
        reactive::Computed<std::vector<TableSort>> const* sortValue = nullptr;
        if (sortable) {
            sortValue =
                bind(scope, spec.sort, [&widget](std::vector<TableSort> const& chain) { widget.setSort(chain); });
            widget.setOnSort(event(spec.onSort));
        }
        bindTableConfig(scope, spec, widget);
        auto& spans = scope.make<SpanTable>();
        spans.columns = spec.columns.size();
        spans.threshold = spec.virtualize.threshold;
        // The sort counts where it is wired: a table with no sortable column shows no sort. It is read inside the
        // rows Effect, not in an Effect of its own, so that two Effects cannot resolve the spans in either order,
        // one of them against the wrong rows. A sort bound to a server query can still clear before the rows reorder.
        spans.sorted = sortable && sortValue == nullptr && !spec.sort.constant().empty();
        mountRows(
            scope, spec.rows, widget,
            [this, &widget, &spans](reactive::Scope& rowScope, Node const& node, Widget& rowWidget, Key const& key) {
                widget.setRowKey(rowWidget, key);
                // A row's tone binds in the row's scope, after its key and whether it is a heading.
                Row const* const view = node ? std::get_if<Row>(&node->kind) : nullptr;
                if (view != nullptr && view->heading) {
                    widget.setRowHeading(rowWidget);
                }
                if (view != nullptr && (view->heading || view->spans)) {
                    spans.rows[key].heading = view->heading;
                    if (view->spans) {
                        bind(rowScope, *view->spans, [this, &widget, &spans, key](std::vector<RowSpan> const& asked) {
                            spans.rows[key].requested = asked;
                            spans.anyAsked = spans.anyAsked || !asked.empty();
                            resolveSpans(widget, spans);
                        });
                    }
                }
                if (view != nullptr && view->tone) {
                    bind(rowScope, *view->tone, [&widget, &rowWidget](std::optional<Tone> const& tone) {
                        widget.setRowTone(rowWidget, tone);
                    });
                }
                if (view != nullptr && view->label) {
                    bind(rowScope, *view->label,
                         [&widget, &rowWidget](std::string const& label) { widget.setRowLabel(rowWidget, label); });
                }
            },
            &spec.virtualize,
            RowsObserver{
                .track =
                    [&spans, sortValue] {
                        if (sortValue != nullptr) {
                            spans.sorted = !sortValue->get().empty();
                        }
                    },
                .begin = [&spans] { ++spans.reconciling; },
                .end =
                    [this, &widget, &spans](std::vector<KeyedRow> const& rows) {
                        --spans.reconciling;
                        spans.order.clear();
                        for (KeyedRow const& row : rows) {
                            spans.order.push_back(
                                {.key = row.key,
                                 .widget = row.widgetState == RowWidgetState::Realized ? row.widget : nullptr});
                        }
                        std::unordered_set<Key> present;
                        for (KeyedRow const& row : rows) {
                            present.insert(row.key);
                        }
                        std::erase_if(spans.rows,
                                      [&present](auto const& entry) { return !present.contains(entry.first); });
                        resolveSpans(widget, spans);
                    }});
        // While the selection is of the view the backend marks it, and the key list it holds is not sent over it.
        auto& viewing = scope.make<bool>(false);
        widget.setOnSelectionChange(
            inputEvent(spec.onSelectionChange,
                       controlled(scope, spec.selection, [&widget, &viewing](std::vector<Key> const& keys) {
                           if (!viewing) {
                               widget.setSelection(keys);
                           }
                       })));
        bindSelectionView(scope, spec, widget, viewing);
        return widget;
    }

    // Runs a pass that changes @p rows inside the observer's begin and end. The end runs also when the pass throws, so
    // a failed row never leaves the table counting a pass that is over.
    [[nodiscard]] static auto observedPass(RowsObserver observer, KeyedRows const& rows) {
        return [observer = std::move(observer), &rows](auto&& pass) {
            if (observer.begin) {
                observer.begin();
            }
            try {
                pass();
            } catch (...) {
                if (observer.end) {
                    observer.end(rows.list);
                }
                throw;
            }
            if (observer.end) {
                observer.end(rows.list);
            }
        };
    }

    // The reconcile of a windowed table's rows Effect: below the threshold every row is realized, above it only the
    // rows in @p window and its overscan are.
    void reconcileWindowed(std::size_t depth, ForEachSession& session, KeyedRows& rows, ContainerWidget& container,
                           TableWidget& table, std::vector<Key> const& keys, RowHook const& onRow,
                           VisibleRange const& window, Virtualization const& setting, bool windowed) {
        if (keys.size() < setting.threshold) {
            reconcile(depth, session, rows.list, container, keys, onRow);
            if (windowed) {
                // Rows released while the table was windowed come back: below the threshold every row is realized.
                applyWindow(depth, rows.list, container, {.first = 0, .count = rows.list.size()}, 0, onRow);
            }
            return;
        }
        std::pair<VisibleRange, std::size_t> const windowAndOverscan{window, setting.overscan};
        reconcile(depth, session, rows.list, container, keys, onRow, &windowAndOverscan);
        // The kept rows have new view positions, and reconcile realizes only new keys: apply the window again so the
        // realized rows are the ones now at the window's view positions.
        applyWindow(depth, rows.list, container, window, setting.overscan, onRow);
        // Only above threshold: a backend's own selection-filtering logic may need to know a key still exists even
        // without a realized row for it right now (backend.hpp's setExistingKeys). Below threshold every key already
        // has a realized row, so this would add nothing.
        table.setExistingKeys(keys);
    }

    // Tells the backend which rows are realized once @p keyCount is at or above the threshold, and that the window is
    // over once the count has fallen back below it; @p windowed is whether the backend was last told the rows are
    // windowed, and is updated.
    static void publishRealizedWindow(TableWidget& table, KeyedRows const& rows, std::size_t keyCount,
                                      Virtualization const& setting, bool& windowed) {
        if (keyCount >= setting.threshold) {
            auto const [first, count] = realizedExtent(rows.list);
            table.setRealizedWindow(first, count, rows.list.size());
            windowed = true;
        } else if (windowed) {
            table.setRealizedWindow(0, rows.list.size(), rows.list.size());
            windowed = false;
        }
    }

    // A table over a `TableSource`: the rows in and near view, each built from the source's view index, and in each
    // row the cells of the columns in or near view.
    struct SourceTable {
        // Which pool a cell goes back to: the default cell's pool of one role, or a column's own pool.
        using PoolKey = std::pair<bool, std::size_t>;

        // Where an accented cell's accent is told: the row widget and the column it is attached at (no row while it is
        // pooled or being moved), and the accent last evaluated, sent again whenever it is attached.
        struct Accent {
            Widget* row = nullptr;
            std::size_t column = 0;
            CellAccent last;
        };

        // Where a marked cell's marks are told, as for an accent.
        struct Marks {
            Widget* row = nullptr;
            std::size_t column = 0;
            CellMarks last;
        };

        // One cell: its scope owns its view, its bindings and its widget; its signals are the record it shows and,
        // for the default cell, the column it shows.
        struct Cell {
            std::unique_ptr<reactive::Scope> scope;
            reactive::Signal<Value>* row = nullptr;
            reactive::Signal<std::size_t>* column = nullptr;
            // Engaged for a cell whose view is an `AccentedCell`; owned by `scope`.
            Accent* accent = nullptr;
            // Engaged for a cell whose view is a `MarkedCell`; owned by `scope`.
            Marks* marks = nullptr;
            Widget* widget = nullptr;
            PoolKey pool;
        };
        // A cell in a column of its row: `fresh` once bound to the row the slot now shows.
        struct Placed {
            Cell cell;
            bool fresh = false;
        };
        // A realized row: a keyed row, or a placeholder for a row not fetched yet. Members go in reverse: the pooled
        // and placed cells, then the row's look, then the stack and the row's signal.
        struct Realized {
            std::unique_ptr<reactive::Scope> scope;
            std::unique_ptr<reactive::Scope> look;
            std::int64_t index = 0;
            // The index the backend was last told (`setRowIndex`); -1 before.
            std::int64_t toldIndex = -1;
            std::optional<Key> key;
            Widget* widget = nullptr;
            reactive::Signal<Value>* row = nullptr;
            bool heading = false;
            // Whether the row is a summary row (`TableRowLook::summary`).
            bool summary = false;
            Widget* headingWidget = nullptr;
            // The stack's children in their order, so a reorder knows where each is.
            std::vector<Widget*> children;
            std::map<std::size_t, CellLoad> loads;
            std::map<std::size_t, Placed> cells;
            std::map<PoolKey, std::vector<Cell>> pool;
            // An open cell editor, a child of this row drawn over the cell at `editorColumn`: last, so it goes first.
            std::unique_ptr<reactive::Scope> editor;
            Widget* editorWidget = nullptr;
            std::size_t editorColumn = 0;
        };

        SourceTable() = default;
        // The rows go last first, as a container's children do.
        ~SourceTable() {
            // The rows' widgets go one by one, and the backend measures the table as they do: `closing` ends every
            // window pass that asks, since the span table and whatever else the mount made after this one is gone.
            closing = true;
            focusLocate.request_stop();
            while (!realized.empty()) {
                realized.pop_back();
            }
        }
        SourceTable(SourceTable const&) = delete;
        SourceTable& operator=(SourceTable const&) = delete;
        SourceTable(SourceTable&&) = delete;
        SourceTable& operator=(SourceTable&&) = delete;

        TableSource source;
        TableWidget* widget = nullptr;
        std::vector<TableColumn> columns;
        Virtualization virtualize;
        std::size_t depth = 0;
        // The rows' spans, resolved over the realized rows; drawn only while every row is realized.
        SpanTable* spans = nullptr;
        // The header bands, and the runs the backend was last told of them.
        std::vector<table::BandInfo> bands;
        std::optional<std::vector<BandRun>> bandsTold;
        // The rows the backend last reported in view; a count of 0 until it reports.
        VisibleRange range;
        // In view order; each widget is the container's child at its place.
        std::vector<Realized> realized;

        // The arrangement, the visible columns in layout order, and each column's place among them.
        TableArrangement arrangement;
        std::vector<std::size_t> visible;
        // Whether the backend was told the visible columns: once at mount, then on every change.
        bool visibleTold = false;
        std::vector<std::optional<std::size_t>> placeOf;
        std::shared_ptr<ColumnExtents> extents = std::make_shared<ColumnExtents>();
        // What the backend measured of each column (an index into `columns`), the largest of each width reported; it
        // outlives an arrangement, so a column laid out again starts from it.
        std::vector<ColumnMeasure> measures;
        // The places of the visible stretch columns whose width the user did not set, and whether the widths they get
        // are owed a recomputation: the room, a measure or another column's width changed.
        std::vector<std::size_t> stretchPlaces;
        bool stretchDirty = true;
        // The backend's measure of the table, as of the last arrangement.
        TableMeasure measure;
        // The horizontal viewport, once the backend reported one.
        bool horizontal = false;
        int scrollX = 0;
        int viewport = 0;
        // The column window: indices into `columns`, the pinned ones first; what the backend was last told of it.
        std::vector<std::size_t> window;
        std::size_t pinnedShown = 0;
        bool windowDirty = true;
        bool windowedColumns = false;
        std::vector<std::string> fetched;
        std::optional<std::pair<std::optional<std::int64_t>, std::size_t>> counts;

        // Focus: a row key and a column, held here, and the view row it is at; a key not known yet while the row
        // is not fetched.
        bool focused = false;
        std::optional<Key> focusKey;
        std::int64_t focusIndex = 0;
        // The focused row left the view: focus goes to the row now at its index. `focusWas` keeps the key it left,
        // so a key move from it that arrives after the row left still takes focus to the row's new key.
        bool focusLeft = false;
        std::optional<Key> focusWas;
        // The focused index came from the rows' answer to where the key is, which is of the view as it is now, while
        // the rows held may not have taken in the change that put the key there yet: the index is trusted only once a
        // realized row holds the key, and the backend is never told an index whose row holds another key.
        bool focusUnconfirmed = false;
        // The latest `KeyMoves` generation applied.
        std::uint64_t keyMovesApplied = 0;
        // A reset moved the focused or highlighted key where no realized row has it yet: each pass looks for it
        // until a row has it, or the user moves focus or the highlight changes. Focus also asks the rows where its
        // key is, an ask `focusLocate` stops, and their answer ends its search.
        bool focusSearching = false;
        bool highlightSearching = false;
        ::core::async::StopSource focusLocate;
        // The rows answered where the focused key is, and the backend is not told yet: it is told once the view's
        // count is known and holds the index, which a server's rows learn only from the first page after a reset.
        bool focusUntold = false;
        // The view's row count at the end of the last pass: an index at or past it names no row a change can move.
        std::size_t lastCount = 0;
        std::size_t focusColumn = 0;
        std::function<void(std::optional<CellRef>)> onFocusCell;
        // A grid's focus (`Table::gridFocus`): the user's moves go to `onGridFocus`, and focus is shown where the
        // grid says.
        bool grid = false;
        std::function<void(GridFocus)> onGridFocus;
        // The cell `Table::editing` names, and the editor's builder; the editor is open on a realized row of its key.
        std::optional<CellRef> editWanted;
        std::function<Node(CellRef const&)> editor;
        // A table that selects cells (`Table::cellSelection`): the range by its corners, and the rectangle the backend
        // was last told, so a pass that leaves it where it was tells nothing.
        bool cells = false;
        std::optional<CellRange> cellRange;
        std::optional<std::array<std::size_t, 4>> cellRect;
        bool cellRectTold = false;
        // The highlighted row, followed by key.
        std::optional<Key> highlightKey;
        std::int64_t highlightIndex = 0;

        bool mounted = false;
        bool continuing = false;
        // Whether the table is being destroyed.
        bool closing = false;
        // Whether a window pass is running, and whether a call came in during it.
        bool realizing = false;
        bool realizeAgain = false;
        // Expires with the table, so a change the rows deliver after it is gone does nothing.
        std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    };

    // A cell's load state: from the row's unloaded and failed levels and the level of the cell's column.
    [[nodiscard]] static CellLoad cellLoadOf(SourceTable const& table, table::UnloadedLevels const& levels,
                                             std::size_t column) {
        if (column >= table.source.levels.size() || table.source.levels.at(column).empty()) {
            return CellLoad::Loaded;
        }
        std::string const& level = table.source.levels.at(column);
        if (std::ranges::contains(levels.failed, level)) {
            return CellLoad::Failed;
        }
        return std::ranges::contains(levels.levels, level) ? CellLoad::Unloaded : CellLoad::Loaded;
    }

    [[nodiscard]] static SourceTable::PoolKey poolOf(SourceTable const& table, std::size_t column) {
        bool const own = !table.source.defaultCellView ||
                         (column < table.source.ownCell.size() ? table.source.ownCell.at(column) : true);
        if (own) {
            return {false, column};
        }
        return {true, static_cast<std::size_t>(table.columns.at(column).role)};
    }

    // Whether the columns are laid out by the mount: told to the backend by `setColumnWindow`, and each cell placed
    // by `setCellColumn`.
    [[nodiscard]] static bool arranged(SourceTable const& table) {
        return table.horizontal || table.arrangement != TableArrangement{};
    }

    // A content column's width: its widest cell and its header once either is measured, the backend's estimate before.
    [[nodiscard]] static int contentWidthOf(ColumnMeasure const& known, TableMeasure const& measure) {
        int const measured = std::max(known.natural, known.header);
        return measured > 0 ? measured : measure.contentEstimate;
    }

    // Takes a new arrangement: the visible columns, their places, and their widths, the declared ones and the user's.
    static void arrange(SourceTable& table, TableArrangement arrangement) {
        table.arrangement = std::move(arrangement);
        std::vector<std::size_t> const before = std::exchange(table.visible, {});
        table.placeOf.assign(table.columns.size(), std::nullopt);
        auto const add = [&table](std::size_t column) {
            if (column < table.columns.size() && !table.placeOf.at(column)) {
                table.placeOf.at(column) = table.visible.size();
                table.visible.push_back(column);
            }
        };
        if (table.arrangement.visible.empty()) {
            for (std::size_t column = 0; column < table.columns.size(); ++column) {
                add(column);
            }
        } else {
            for (std::size_t const column : table.arrangement.visible) {
                add(column);
            }
        }
        if (table.visible != before || !table.visibleTold) {
            table.visibleTold = true;
            table.widget->setVisibleColumns(table.visible);
        }
        // Widths in the backend's measure: a fixed column's units and a user's width scaled by it, with the padding a
        // column takes beyond its content, a content column from the backend's estimate. Monospace characters are
        // rounded up, so a column of n of them never cuts the last.
        TableMeasure const measure = table.widget->tableMeasure();
        table.measure = measure;
        table.measures.resize(table.columns.size());
        auto const scaled = [&measure, &table](std::size_t column, int units) {
            bool const mono = table.columns.at(column).role == TextRole::Mono;
            double const content = units * (mono ? measure.monoUnit : measure.unit);
            return (mono ? static_cast<int>(std::ceil(content)) : static_cast<int>(std::lround(content))) +
                   measure.padding;
        };
        std::vector<int> widths;
        widths.reserve(table.visible.size());
        for (std::size_t const column : table.visible) {
            Sizing const& width = table.columns.at(column).width;
            ColumnMeasure const& known = table.measures.at(column);
            widths.push_back(width.kind == Sizing::Kind::Fixed ? scaled(column, width.amount)
                                                               : contentWidthOf(known, measure));
        }
        table.extents->assign(widths);
        table.stretchPlaces.clear();
        for (std::size_t place = 0; place < table.visible.size(); ++place) {
            std::size_t const column = table.visible.at(place);
            if (table.columns.at(column).width.kind == Sizing::Kind::Stretch &&
                !table.arrangement.widths.contains(column)) {
                table.stretchPlaces.push_back(place);
            }
        }
        table.stretchDirty = true;
        if (table.spans != nullptr) {
            table.spans->placeOf = table.placeOf;
        }
        for (auto const& [column, width] : table.arrangement.widths) {
            if (column < table.placeOf.size()) {
                if (auto const place = table.placeOf.at(column)) {
                    table.extents->set(*place, scaled(column, width));
                }
            }
        }
        table.windowDirty = true;
    }

    // The ids every `visible` call names: the always-fetched ones, then each window column's id and what its cell
    // reads.
    [[nodiscard]] static std::vector<std::string> fetchedOf(SourceTable const& table) {
        std::vector<std::string> ids = table.source.alwaysFetched;
        auto const add = [&ids](std::string const& columnId) {
            if (!columnId.empty() && !std::ranges::contains(ids, columnId)) {
                ids.push_back(columnId);
            }
        };
        for (std::size_t const column : table.window) {
            add(table.columns.at(column).id);
            if (column < table.source.cellReads.size()) {
                for (std::string const& columnId : table.source.cellReads.at(column)) {
                    add(columnId);
                }
            }
        }
        return ids;
    }

    // The rows `visible` is told: the reported range. Before any report, a window over a server's rows shows its
    // first `overscan` rows, and one over a document's rows the empty view a keyed list starts from.
    [[nodiscard]] static VisibleRange shownOf(SourceTable const& table) {
        return table.range.count > 0 || !table.source.alwaysWindowed
                   ? table.range
                   : VisibleRange{.first = 0, .count = table.virtualize.overscan};
    }

    // The rows focus takes as in view: the reported range, or the first `overscan` rows before any report, whatever
    // the rows, so a page moves by the rows a view would show and a row past them is scrolled to.
    [[nodiscard]] static VisibleRange focusRangeOf(SourceTable const& table) {
        return table.range.count > 0 ? table.range : VisibleRange{.first = 0, .count = table.virtualize.overscan};
    }

    static void tellVisible(SourceTable& table) {
        VisibleRange const shown = shownOf(table);
        table.source.rows->visible(static_cast<std::int64_t>(shown.first), static_cast<std::int64_t>(shown.count),
                                   table.fetched);
    }

    // Brings the column window up to date with the scroll, the arrangement, the widths and the rows' count, and tells
    // the backend and the rows what changed. Reads one width per column of the window, and none outside it.
    static void refreshColumns(SourceTable& table, std::size_t rows) {
        if (table.stretchDirty) {
            fitStretch(table);
        }
        bool const windowed =
            table.horizontal && windowingOf(table.virtualize, rows, table.visible.size(), /*serverMode*/ true).columns;
        if (!table.windowDirty && windowed == table.windowedColumns) {
            return;
        }
        table.windowDirty = false;
        table.windowedColumns = windowed;
        ColumnExtents const& extents = *table.extents;
        std::size_t pinned = std::min(table.arrangement.pinned, table.visible.size());
        // Pinned columns wider than half the viewport leave the last of them to scroll with the rest; the
        // arrangement keeps its count.
        if (table.horizontal) {
            while (pinned > 0 && extents.offsetOf(pinned) > table.viewport / 2) {
                --pinned;
            }
        }
        std::vector<std::size_t> places;
        if (windowed) {
            places =
                columnWindow(extents, pinned, table.scrollX, table.viewport, table.virtualize.columnOverscan).columns;
        } else {
            places.resize(table.visible.size());
            std::ranges::iota(places, std::size_t{0});
        }
        std::vector<std::size_t> window;
        window.reserve(places.size() + 1);
        for (std::size_t const place : places) {
            window.push_back(table.visible.at(place));
        }
        tellBands(table, places, pinned);
        // The focused column stays realized whatever the scroll.
        std::optional<std::size_t> const focusPlace = table.focused && table.focusColumn < table.placeOf.size()
                                                          ? table.placeOf.at(table.focusColumn)
                                                          : std::nullopt;
        if (focusPlace && !std::ranges::contains(window, table.focusColumn)) {
            window.push_back(table.focusColumn);
            places.push_back(*focusPlace);
        }
        bool const changed = window != table.window || pinned != table.pinnedShown;
        table.window = std::move(window);
        table.pinnedShown = pinned;
        if (arranged(table)) {
            std::vector<int> offsets;
            std::vector<int> widths;
            offsets.reserve(places.size());
            widths.reserve(places.size());
            for (std::size_t const place : places) {
                offsets.push_back(extents.offsetOf(place));
                widths.push_back(extents.widthOf(place));
            }
            table.widget->setColumnWindow(table.window, offsets, widths, extents.totalWidth(), table.pinnedShown);
        }
        if (changed) {
            std::vector<std::string> fetched = fetchedOf(table);
            if (fetched != table.fetched) {
                table.fetched = std::move(fetched);
                if (table.mounted) {
                    tellVisible(table);
                }
            }
        }
    }

    // The place of the first column fully in view, past the pinned ones: a width change left of it moves the scroll
    // offset by as much, so the columns in view stay where they are.
    [[nodiscard]] static std::size_t firstFullPlace(SourceTable const& table) {
        ColumnExtents const& extents = *table.extents;
        std::size_t firstFull = extents.columnsAt(table.scrollX, 1).first;
        if (extents.offsetOf(firstFull) < table.scrollX) {
            ++firstFull;
        }
        return std::max(firstFull, table.pinnedShown);
    }

    // Gives the stretch columns the width the other columns leave, by the rules of `stretchWidths`: each column's
    // natural width is the larger of its header and its widest cell measured so far, so a table over rows it has not
    // realized fits the rows it has seen. A table whose viewport the backend has not reported keeps the estimate.
    static void fitStretch(SourceTable& table) {
        table.stretchDirty = false;
        if (!table.horizontal || table.stretchPlaces.empty()) {
            return;
        }
        ColumnExtents& extents = *table.extents;
        int const shrinkFloor = table.measure.shrinkFloor;
        std::vector<StretchColumn> columns;
        columns.reserve(table.stretchPlaces.size());
        std::int64_t held = 0;
        for (std::size_t const place : table.stretchPlaces) {
            TableColumn const& column = table.columns.at(table.visible.at(place));
            ColumnMeasure const& known = table.measures.at(table.visible.at(place));
            int const natural = std::max(known.header, known.natural);
            columns.push_back(
                StretchColumn{.natural = natural,
                              .least = column.wrap ? std::max(known.header, known.least) : natural,
                              .floor = column.shrink ? std::min(known.header, shrinkFloor) : known.header,
                              .weight = column.width.amount,
                              .shrink = column.shrink});
            held += extents.offsetOf(place + 1) - extents.offsetOf(place);
        }
        std::int64_t const room = std::int64_t{table.viewport} - (extents.totalWidth() - held);
        std::vector<int> const widths = stretchWidths(
            columns, static_cast<int>(std::clamp<std::int64_t>(room, 0, std::numeric_limits<int>::max())));
        std::size_t const firstFull = firstFullPlace(table);
        int moved = 0;
        bool changed = false;
        for (auto const [place, width] : std::views::zip(table.stretchPlaces, widths)) {
            int const before = extents.offsetOf(place + 1) - extents.offsetOf(place);
            if (extents.fit(place, width)) {
                changed = true;
                if (place >= table.pinnedShown && place < firstFull) {
                    moved += width - before;
                }
            }
        }
        if (!changed) {
            return;
        }
        table.windowDirty = true;
        if (moved != 0) {
            table.scrollX = std::max(table.scrollX + moved, 0);
            table.widget->setHorizontalOffset(table.scrollX);
        }
    }

    // The band runs over the visible columns, cut to the column window's places: a band exists for the columns in the
    // window. The backend is told only when they change.
    static void tellBands(SourceTable& table, std::vector<std::size_t> const& places, std::size_t pinned) {
        if (table.bands.empty()) {
            return;
        }
        std::vector<std::string> ids;
        ids.reserve(table.visible.size());
        for (std::size_t const column : table.visible) {
            ids.push_back(table.columns.at(column).id);
        }
        std::vector<bool> inWindow(table.visible.size(), false);
        for (std::size_t const place : places) {
            inWindow.at(place) = true;
        }
        std::vector<BandRun> runs;
        for (BandRun const& run : bandRuns(table.bands, ids, pinned)) {
            std::optional<std::size_t> first;
            for (std::size_t at = run.first; at <= run.first + run.count; ++at) {
                bool const shown = at < run.first + run.count && inWindow.at(at);
                if (shown && !first) {
                    first = at;
                } else if (!shown && first) {
                    BandRun part = run;
                    part.first = *first;
                    part.count = at - *first;
                    runs.push_back(std::move(part));
                    first.reset();
                }
            }
        }
        if (!table.bandsTold || *table.bandsTold != runs) {
            table.widget->setBands(runs);
            table.bandsTold = std::move(runs);
        }
    }

    // Builds a cell for @p column of @p row, showing @p record. All or nothing.
    [[nodiscard]] SourceTable::Cell makeCell(SourceTable& table, SourceTable::Realized& row, std::size_t column,
                                             Value const& record) {
        SourceTable::Cell cell;
        cell.pool = poolOf(table, column);
        cell.scope = std::make_unique<reactive::Scope>(*_rt, table.depth + 1);
        reactive::Scope& scope = *cell.scope;
        cell.row = &scope.make<reactive::Signal<Value>>(*_rt, record);
        Node made;
        if (cell.pool.first) {
            cell.column = &scope.make<reactive::Signal<std::size_t>>(*_rt, column);
            made = table.source.defaultCellView(*cell.column, *cell.row);
        } else {
            made = table.source.cellView(column, *cell.row);
        }
        Node const& node = scope.make<Node>(std::move(made));
        auto* const stack = dynamic_cast<ContainerWidget*>(row.widget);
        // A marked cell's content is the cell, which may be accented; an accented cell mounts its content as the
        // cell. The marks and the accent are bound in the cell's scope and told once the cell is attached to a row
        // (`realizeCell`), so a cell built for a pool tells nothing yet.
        MarkedCell const* const marked = node ? std::get_if<MarkedCell>(&node->kind) : nullptr;
        Node const& inner = marked != nullptr ? marked->content : node;
        AccentedCell const* const accented = inner ? std::get_if<AccentedCell>(&inner->kind) : nullptr;
        Widget* const widget = inner ? mount(scope, accented != nullptr ? accented->content : inner, stack)
                                     : &scope.adopt(_backend->createSpacer(stack));
        if (marked != nullptr) {
            auto& marks = scope.make<SourceTable::Marks>();
            cell.marks = &marks;
            bind(scope, marked->marks, [&table, &marks](CellMarks const& value) {
                marks.last = value;
                if (marks.row != nullptr) {
                    table.widget->setCellMarks(*marks.row, marks.column, value);
                }
            });
        }
        if (accented != nullptr) {
            auto& accent = scope.make<SourceTable::Accent>();
            cell.accent = &accent;
            bind(scope, accented->accent, [&table, &accent](CellAccent const& value) {
                accent.last = value;
                if (accent.row != nullptr) {
                    table.widget->setCellAccent(*accent.row, accent.column, value);
                }
            });
        }
        cell.widget = widget;
        row.children.push_back(widget);
        ++_stats.cellsCreated;
        return cell;
    }

    // Sets a cell's record, counting a set that changed it.
    void setRecord(reactive::Signal<Value>& signal, Value const& record) {
        if (signal.peek() != record) {
            ++_stats.cellBinds;
            signal.set(record);
        }
    }

    // Gives a slot the row @p ref: its key, its record, its look. Its cells still show the row they showed, until
    // each is rebound; the cells of a row-scoped column (`TableSource::rowScopedCells`) of another key are dropped,
    // to be built for this one.
    void assignRow(SourceTable& table, SourceTable::Realized& slot, std::int64_t index, TableRowRef const& ref) {
        if (slot.key && *slot.key != ref.key) {
            closeEditor(table, slot);
            dropRowScoped(table, slot);
        }
        slot.index = index;
        slot.key = ref.key;
        if (slot.row->peek() != ref.row) {
            ++_stats.rowBinds;
            slot.row->set(ref.row);
        }
        if (slot.headingWidget != nullptr) {
            std::erase(slot.children, slot.headingWidget);
            slot.headingWidget = nullptr;
        }
        slot.look.reset();
        slot.look = std::make_unique<reactive::Scope>(*_rt, table.depth + 1);
        reactive::Scope& lookScope = *slot.look;
        TableRowLook look;
        if (table.source.rowView) {
            look = table.source.rowView(*slot.row);
            lookScope.make<std::shared_ptr<void const>>(look.keep);
        }
        table.widget->setRowKey(*slot.widget, *slot.key);
        slot.summary = look.summary;
        if (look.summary) {
            table.widget->setRowSummary(*slot.widget);
        }
        slot.heading = look.heading != nullptr;
        if (slot.heading) {
            Node const& heading = lookScope.make<Node>(look.heading);
            if (Widget* const made = mount(lookScope, heading, dynamic_cast<ContainerWidget*>(slot.widget))) {
                slot.children.push_back(made);
                slot.headingWidget = made;
            }
            table.widget->setRowHeading(*slot.widget);
        }
        if (table.spans != nullptr && (look.heading || look.spans)) {
            SpanTable& spans = *table.spans;
            spans.rows[*slot.key].heading = static_cast<bool>(look.heading);
            if (look.spans) {
                bind(lookScope, *look.spans,
                     [this, &table, &spans, key = *slot.key](std::vector<RowSpan> const& asked) {
                         spans.rows[key].requested = asked;
                         spans.anyAsked = spans.anyAsked || !asked.empty();
                         resolveSpans(*table.widget, spans);
                     });
            }
        }
        if (look.tone) {
            bind(lookScope, *look.tone, [&table, widget = slot.widget](std::optional<Tone> const& tone) {
                table.widget->setRowTone(*widget, tone);
            });
        }
        if (look.label) {
            bind(lookScope, *look.label, [&table, widget = slot.widget](std::string const& label) {
                table.widget->setRowLabel(*widget, label);
            });
        }
        if (look.marks) {
            bind(lookScope, *look.marks,
                 [&table, widget = slot.widget](RowMarks const& marks) { table.widget->setRowMarks(*widget, marks); });
        }
        if (look.modified) {
            bind(lookScope, *look.modified,
                 [&table, widget = slot.widget](bool modified) { table.widget->setRowModified(*widget, modified); });
        }
        for (auto& [column, placed] : slot.cells) {
            placed.fresh = false;
        }
    }

    // Unmounts a row's open cell editor, and draws the cell again.
    static void closeEditor(SourceTable& table, SourceTable::Realized& slot) {
        if (slot.editor == nullptr) {
            return;
        }
        Widget* const editor = std::exchange(slot.editorWidget, nullptr);
        table.widget->setCellEditor(*slot.widget, slot.editorColumn, nullptr);
        std::erase(slot.children, editor);
        slot.editor.reset();
    }

    // Brings the open editor to the cell `Table::editing` names: the one open on another cell is closed, and the
    // wanted one is opened once a realized row holds its key, built once, drawn over the cell and focused.
    void syncEditor(SourceTable& table) {
        std::optional<std::size_t> column;
        if (table.editWanted) {
            auto const found = std::ranges::find(table.columns, table.editWanted->column, &TableColumn::id);
            if (found != table.columns.end()) {
                column = static_cast<std::size_t>(found - table.columns.begin());
            }
        }
        for (SourceTable::Realized& slot : table.realized) {
            bool const wanted =
                column && slot.key && table.editWanted && *slot.key == table.editWanted->key && !slot.heading;
            if (slot.editor != nullptr && (!wanted || slot.editorColumn != *column)) {
                closeEditor(table, slot);
            }
            if (!wanted || slot.editor != nullptr || !table.editor) {
                continue;
            }
            auto scope = std::make_unique<reactive::Scope>(*_rt, table.depth + 1);
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): `wanted` holds only while an edit is wanted.
            Node const& node = scope->make<Node>(table.editor(*table.editWanted));
            Widget* const made =
                mountPass([&] { return mount(*scope, node, dynamic_cast<ContainerWidget*>(slot.widget)); });
            if (made == nullptr) {
                continue;
            }
            slot.editor = std::move(scope);
            slot.editorWidget = made;
            slot.editorColumn = *column;
            slot.children.push_back(made);
            table.widget->setCellEditor(*slot.widget, *column, made);
            try {
                made->focus();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Focus is a request: a backend that cannot move it leaves it where it was.
            }
        }
    }

    // A new slot at @p index: a stack, and the row if the rows hold it. All or nothing.
    [[nodiscard]] SourceTable::Realized makeRow(SourceTable& table, std::int64_t index) {
        SourceTable::Realized made;
        made.index = index;
        made.scope = std::make_unique<reactive::Scope>(*_rt, table.depth);
        StackWidget& stack = made.scope->adopt(_backend->createStack(table.widget, Axis::Horizontal));
        made.scope->make<OffChain>(_offChain, &stack);
        made.widget = &stack;
        std::optional<TableRowRef> const ref = table.source.rows->row(index);
        made.row = &made.scope->make<reactive::Signal<Value>>(*_rt, ref ? ref->row : Value{});
        if (ref) {
            assignRow(table, made, index, *ref);
        }
        return made;
    }

    // Parks a placed cell in its row's pool: it leaves the row's layout, keeping its widget and its record.
    static void park(SourceTable& table, SourceTable::Realized& slot, std::size_t column) {
        auto const found = slot.cells.find(column);
        if (found == slot.cells.end()) {
            return;
        }
        SourceTable::Cell cell = std::move(found->second.cell);
        slot.cells.erase(found);
        if (cell.accent != nullptr) {
            cell.accent->row = nullptr;
        }
        if (cell.marks != nullptr) {
            cell.marks->row = nullptr;
        }
        table.widget->setCellColumn(*slot.widget, *cell.widget, std::nullopt);
        slot.pool[cell.pool].push_back(std::move(cell));
    }

    // One cell's work in a pass: binding a cell to another row or column, or building one.
    struct CellWork {
        std::size_t slot = 0;
        std::size_t column = 0;
        std::size_t distance = 0;
    };

    // Puts a cell at @p column of a slot: a stale placed cell is rebound, a pooled one taken and rebound, and only
    // an empty pool builds one.
    void realizeCell(SourceTable& table, SourceTable::Realized& slot, std::size_t column, Value const& record) {
        if (auto const placed = slot.cells.find(column); placed != slot.cells.end()) {
            setRecord(*placed->second.cell.row, record);
            placed->second.fresh = true;
            ++_stats.cellsRebound;
            return;
        }
        SourceTable::PoolKey const key = poolOf(table, column);
        SourceTable::Cell cell;
        if (auto const pooled = slot.pool.find(key); pooled != slot.pool.end() && !pooled->second.empty()) {
            cell = std::move(pooled->second.back());
            pooled->second.pop_back();
            if (cell.accent != nullptr) {
                cell.accent->row = nullptr;
            }
            if (cell.marks != nullptr) {
                cell.marks->row = nullptr;
            }
            if (cell.column != nullptr && cell.column->peek() != column) {
                cell.column->set(column);
            }
            setRecord(*cell.row, record);
            ++_stats.cellsRebound;
        } else {
            cell = mountPass([&] { return makeCell(table, slot, column, record); });
        }
        table.widget->setCellColumn(*slot.widget, *cell.widget, column);
        // Attached to this row at this column: the accent is told here whether or not it changed.
        if (cell.accent != nullptr) {
            cell.accent->row = slot.widget;
            cell.accent->column = column;
            table.widget->setCellAccent(*slot.widget, column, cell.accent->last);
        }
        // The marks likewise: a cell rebound to another row re-evaluates them with its content, and this row is told
        // them even when they equal the ones it showed.
        if (cell.marks != nullptr) {
            cell.marks->row = slot.widget;
            cell.marks->column = column;
            table.widget->setCellMarks(*slot.widget, column, cell.marks->last);
        }
        slot.cells.emplace(column, SourceTable::Placed{.cell = std::move(cell), .fresh = true});
    }

    // Tells the widget each placed cell's load state that changed.
    static void tellLoads(SourceTable& table, SourceTable::Realized& slot) {
        table::UnloadedLevels const levels = table.source.rows->unloadedLevels(slot.index);
        for (auto const& [column, placed] : slot.cells) {
            if (!placed.fresh) {
                continue;
            }
            CellLoad const load = cellLoadOf(table, levels, column);
            auto const [was, made] = slot.loads.try_emplace(column, CellLoad::Loaded);
            if (was->second != load) {
                was->second = load;
                table.widget->setCellLoad(*slot.widget, column, load);
            }
        }
    }

    // Brings the realized rows to the window around the rows in view, and their cells to the column window.
    //
    // A row the rows still hold keeps its slot, wherever it moved; one at an index @p updated names has its record
    // set again. A row that enters takes the slot of one that left, and only when none left is a slot built. Within a
    // row, a cell whose column left the window goes to the row's pool, and a column that enters takes a pooled cell
    // of its pool. Binding a cell to another row or column, and building one, are counted against
    // `realizePerFrame`, nearest the viewport's centre first; the rest wait, drawn blank, for the next turn.
    //
    // The backend measures a table while its cells mount, which asks for the window again from inside the pass that
    // is mounting them, and a second pass would move the rows and slots the first is still walking. A call that comes
    // in during a pass is answered when it ends, over every row with its record set again (an equality-gated set),
    // since the rows the call names are not known once it returns.
    void realizeWindow(SourceTable& table, std::function<bool(std::int64_t)> const& updated) {
        if (table.closing) {
            return;
        }
        if (table.realizing) {
            table.realizeAgain = true;
            return;
        }
        table.realizing = true;
        try {
            realizeWindowPass(table, updated);
            while (std::exchange(table.realizeAgain, false) && !table.closing) {
                realizeWindowPass(table, [](std::int64_t) { return true; });
            }
        } catch (...) {
            table.realizing = false;
            table.realizeAgain = false;
            throw;
        }
        table.realizing = false;
    }

    // One pass of `realizeWindow`.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): rows, cells, the budget, focus and the reports.
    void realizeWindowPass(SourceTable& table, std::function<bool(std::int64_t)> const& updated) {
        TableRows const& rows = *table.source.rows;
        TableTotal const total = rows.mountedTotal();
        auto const count = static_cast<std::size_t>(std::max<std::int64_t>(total.rows, 0));
        refreshColumns(table, count);
        // A short table over a document's rows realizes every row. A windowed one realizes the rows in view and
        // `overscan` either side.
        bool const everyRow = !table.source.alwaysWindowed && count < table.virtualize.threshold;
        VisibleRange const shown = shownOf(table);
        std::size_t const overscan = table.virtualize.overscan;
        std::size_t const low = everyRow || shown.first <= overscan ? 0 : shown.first - overscan;
        std::size_t const high = everyRow ? count : std::min(count, shown.first + shown.count + overscan);
        std::vector<std::int64_t> targets;
        targets.reserve(high > low ? high - low + 1 : 1);
        for (std::size_t at = low; at < high; ++at) {
            targets.push_back(static_cast<std::int64_t>(at));
        }
        // The focused row stays realized: one row beyond the window at most.
        if (table.focused && table.focusIndex >= 0 && std::cmp_less(table.focusIndex, count) &&
            (std::cmp_less(table.focusIndex, low) || std::cmp_greater_equal(table.focusIndex, high))) {
            targets.insert(std::ranges::upper_bound(targets, table.focusIndex), table.focusIndex);
        }
        if (table.spans != nullptr) {
            ++table.spans->reconciling;
        }
        std::vector<SourceTable::Realized> old = std::move(table.realized);
        table.realized.clear();
        std::unordered_map<Key, std::size_t> keyed;
        std::unordered_map<std::int64_t, std::size_t> placeholders;
        for (std::size_t at = 0; at < old.size(); ++at) {
            if (old.at(at).key) {
                keyed.emplace(*old.at(at).key, at);
            } else {
                placeholders.emplace(old.at(at).index, at);
            }
        }
        std::vector<Widget*> current;
        current.reserve(old.size());
        for (SourceTable::Realized const& row : old) {
            current.push_back(row.widget);
        }
        std::vector<std::optional<std::size_t>> match(targets.size());
        std::vector<bool> taken(old.size(), false);
        for (std::size_t at = 0; at < targets.size(); ++at) {
            std::optional<TableRowRef> const ref = rows.row(targets.at(at));
            if (ref) {
                if (auto const found = keyed.find(ref->key); found != keyed.end() && !taken.at(found->second)) {
                    match.at(at) = found->second;
                }
            } else if (auto const found = placeholders.find(targets.at(at));
                       found != placeholders.end() && !taken.at(found->second)) {
                match.at(at) = found->second;
            }
            if (match.at(at)) {
                taken.at(*match.at(at)) = true;
            }
        }
        // Slots no row kept, for the rows that enter: keyed ones only for a keyed row, so no stack keeps a key it no
        // longer shows; a placeholder's for either. A heading's or a summary row's slot is never given another row:
        // the backend has no call that makes a row a data row again, so it goes, and the entering row gets a new one.
        std::vector<std::size_t> freeKeyed;
        std::vector<std::size_t> freePlain;
        for (std::size_t at = old.size(); at-- > 0;) {
            if (!taken.at(at) && !old.at(at).heading && !old.at(at).summary) {
                (old.at(at).key ? freeKeyed : freePlain).push_back(at);
            }
        }
        std::exception_ptr failure;
        std::vector<SourceTable::Realized> next;
        next.reserve(targets.size());
        std::vector<bool> const moved;
        for (std::size_t at = 0; at < targets.size(); ++at) {
            std::int64_t const index = targets.at(at);
            std::optional<TableRowRef> const ref = rows.row(index);
            if (match.at(at)) {
                SourceTable::Realized row = std::move(old.at(*match.at(at)));
                bool const shifted = row.index != index;
                row.index = index;
                if (ref && (shifted || updated(index))) {
                    if (row.row->peek() != ref->row) {
                        ++_stats.rowBinds;
                        row.row->set(ref->row);
                    }
                    for (auto& [column, placed] : row.cells) {
                        if (placed.fresh) {
                            setRecord(*placed.cell.row, ref->row);
                        }
                    }
                }
                next.push_back(std::move(row));
                continue;
            }
            try {
                std::vector<std::size_t>* pool = !freePlain.empty() ? &freePlain : nullptr;
                if (ref && !freeKeyed.empty()) {
                    pool = &freeKeyed;
                }
                if (pool != nullptr) {
                    SourceTable::Realized row = std::move(old.at(pool->back()));
                    pool->pop_back();
                    row.index = index;
                    if (ref) {
                        mountPass([&] {
                            assignRow(table, row, index, *ref);
                            return 0;
                        });
                    }
                    next.push_back(std::move(row));
                } else {
                    next.push_back(mountPass([&] { return makeRow(table, index); }));
                    current.push_back(next.back().widget);
                }
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        }
        // What was not taken goes, last first; the container forgets its widgets.
        for (SourceTable::Realized& row : old | std::views::reverse) {
            if (row.scope) {
                std::erase(current, row.widget);
                row.editor.reset();
                row.pool.clear();
                row.cells.clear();
                row.look.reset();
                row.scope.reset();
            }
        }
        std::vector<Widget*> target;
        target.reserve(next.size());
        for (SourceTable::Realized const& row : next) {
            target.push_back(row.widget);
        }
        try {
            reorderChildren(*table.widget, current, target);
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        table.realized = std::move(next);
        for (SourceTable::Realized& row : table.realized) {
            if (row.toldIndex != row.index) {
                row.toldIndex = row.index;
                table.widget->setRowIndex(*row.widget, static_cast<std::size_t>(row.index));
            }
        }
        if (!table.source.alwaysWindowed && !everyRow) {
            // A backend's own selection needs to know a key exists without a realized row for it.
            std::vector<Key> keys;
            keys.reserve(count);
            for (std::size_t at = 0; at < count; ++at) {
                if (std::optional<TableRowRef> const ref = rows.row(static_cast<std::int64_t>(at))) {
                    keys.push_back(ref->key);
                }
            }
            table.widget->setExistingKeys(keys);
        }
        if (table.spans != nullptr) {
            SpanTable& spans = *table.spans;
            --spans.reconciling;
            spans.windowed = !everyRow;
            spans.order.clear();
            std::unordered_set<Key> present;
            for (SourceTable::Realized const& row : table.realized) {
                if (row.key) {
                    spans.order.push_back({.key = *row.key, .widget = row.widget});
                    present.insert(*row.key);
                }
            }
            std::erase_if(spans.rows, [&present](auto const& entry) { return !present.contains(entry.first); });
            resolveSpans(*table.widget, spans);
        }
        realizeCells(table, shown);
        syncEditor(table);
        // The contiguous range only: a focused row kept beyond it is placed by its own `setRowIndex`. Every source
        // table is told it, one that realizes every row as the whole view: a backend takes the view's count from it.
        // Whether spans are drawn is the span table's (`SpanTable::windowed`), not this call's.
        table.widget->setRealizedWindow(low, high > low ? high - low : 0, count);
        afterPass(table, total);
        table.lastCount = count;
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    // The cells of the realized rows: columns that left the window are pooled, and the cells the window lacks are
    // bound or built within the frame's budget, nearest the centre first.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): pooling, the budget, and the order in each row.
    void realizeCells(SourceTable& table, VisibleRange const& shown) {
        TableRows const& rows = *table.source.rows;
        std::unordered_set<std::size_t> const inWindow(table.window.begin(), table.window.end());
        auto const centreRow = static_cast<std::int64_t>(shown.first + (shown.count / 2));
        std::size_t const centreColumn = table.window.empty() ? 0 : table.window.size() / 2;
        std::vector<CellWork> work;
        for (std::size_t at = 0; at < table.realized.size(); ++at) {
            SourceTable::Realized& slot = table.realized.at(at);
            bool const keyed = slot.key.has_value() && !slot.heading;
            std::vector<std::size_t> leaving;
            for (auto const& [column, placed] : slot.cells) {
                if (!keyed || !inWindow.contains(column)) {
                    leaving.push_back(column);
                }
            }
            for (std::size_t const column : leaving) {
                park(table, slot, column);
            }
            // A row the rows do not hold now takes none of the budget: its cells wait for its arrival, which is a
            // change of the rows and so a pass of its own.
            if (!keyed || !rows.row(slot.index)) {
                continue;
            }
            auto const rowDistance = static_cast<std::size_t>(std::abs(slot.index - centreRow));
            for (std::size_t place = 0; place < table.window.size(); ++place) {
                std::size_t const column = table.window.at(place);
                auto const placed = slot.cells.find(column);
                if (placed != slot.cells.end() && placed->second.fresh) {
                    continue;
                }
                std::size_t const columnDistance = place > centreColumn ? place - centreColumn : centreColumn - place;
                work.push_back(
                    CellWork{.slot = at, .column = column, .distance = (rowDistance * 1'000'000) + columnDistance});
            }
        }
        std::ranges::stable_sort(work, {}, &CellWork::distance);
        std::size_t const budget = table.virtualize.realizePerFrame;
        std::exception_ptr failure;
        std::size_t done = 0;
        std::unordered_set<std::size_t> waiting;
        for (std::size_t at = 0; at < work.size(); ++at) {
            SourceTable::Realized& slot = table.realized.at(work.at(at).slot);
            std::size_t const column = work.at(at).column;
            if (at >= budget) {
                // Past the budget: a stale cell leaves its column blank rather than show another row.
                park(table, slot, column);
                waiting.insert(work.at(at).slot);
                ++_stats.placeholders;
                continue;
            }
            std::optional<TableRowRef> const ref = rows.row(slot.index);
            if (!ref) {
                continue;
            }
            try {
                realizeCell(table, slot, column, ref->row);
                ++done;
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        }
        for (std::size_t at = 0; at < table.realized.size(); ++at) {
            SourceTable::Realized& slot = table.realized.at(at);
            // A row with nothing left to realize keeps no pooled cell: the widgets alive are the cells in the window.
            if (!waiting.contains(at)) {
                dropPools(slot);
            }
            tellLoads(table, slot);
            orderCells(table, slot);
        }
        // A pass that realized nothing (every build it tried threw) posts no other: it would fail the same way.
        if (work.size() > budget && done > 0) {
            continueLater(table);
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    // Whether a column's cells each show one row only.
    [[nodiscard]] static bool rowScoped(SourceTable const& table, std::size_t column) {
        return column < table.source.rowScopedCells.size() && table.source.rowScopedCells.at(column);
    }

    // Destroys a row's placed and pooled cells of the row-scoped columns, and forgets their widgets among its
    // children. A default cell is never row-scoped: it reads its row's record alone.
    static void dropRowScoped(SourceTable const& table, SourceTable::Realized& slot) {
        std::erase_if(slot.cells, [&table, &slot](auto const& entry) {
            if (!rowScoped(table, entry.first)) {
                return false;
            }
            std::erase(slot.children, entry.second.cell.widget);
            return true;
        });
        std::erase_if(slot.pool, [&table, &slot](auto const& entry) {
            if (entry.first.first || !rowScoped(table, entry.first.second)) {
                return false;
            }
            for (SourceTable::Cell const& cell : entry.second) {
                std::erase(slot.children, cell.widget);
            }
            return true;
        });
    }

    // Destroys a row's pooled cells, and forgets their widgets among its children.
    static void dropPools(SourceTable::Realized& slot) {
        for (auto& [key, cells] : slot.pool) {
            for (SourceTable::Cell const& cell : cells) {
                std::erase(slot.children, cell.widget);
            }
        }
        slot.pool.clear();
    }

    // A row's children in the window's order, then its pooled cells, for a backend that places cells by their order
    // rather than by `setCellColumn`.
    static void orderCells(SourceTable const& table, SourceTable::Realized& slot) {
        if (slot.heading || slot.children.empty()) {
            return;
        }
        std::vector<Widget*> target;
        target.reserve(slot.children.size());
        for (std::size_t const column : table.window) {
            if (auto const placed = slot.cells.find(column); placed != slot.cells.end()) {
                target.push_back(placed->second.cell.widget);
            }
        }
        for (Widget* const child : slot.children) {
            if (!std::ranges::contains(target, child)) {
                target.push_back(child);
            }
        }
        if (target != slot.children) {
            reorderChildren(*dynamic_cast<ContainerWidget*>(slot.widget), slot.children, target);
        }
    }

    // Posts the next turn's realization, once.
    void continueLater(SourceTable& table) {
        if (table.continuing) {
            return;
        }
        table.continuing = true;
        try {
            _rt->owner().post([this, &table, alive = std::weak_ptr{table.alive}] {
                if (alive.expired()) {
                    return;
                }
                table.continuing = false;
                static_cast<void>(
                    runCallback(*_rt, [&] { realizeWindow(table, [](std::int64_t) { return false; }); }));
            });
        } catch (...) {
            table.continuing = false;
            throw;
        }
    }

    // After a pass: the counts, the focused cell and the highlighted row.
    void afterPass(SourceTable& table, TableTotal const& total) {
        std::pair<std::optional<std::int64_t>, std::size_t> const counts{
            total.known && total.kind == table::TotalKind::Exact ? std::optional{total.rows} : std::nullopt,
            table.visible.size()};
        if (table.counts != counts) {
            table.counts = counts;
            table.widget->setCounts(counts.first, counts.second);
        }
        if (table.highlightKey && table.highlightSearching) {
            if (std::optional<std::int64_t> const now = placeOfKey(table, *table.highlightKey, table.highlightIndex)) {
                table.highlightSearching = false;
                table.highlightIndex = *now;
                table.widget->setHighlightedRow(static_cast<std::size_t>(*now));
            }
        }
        if (table.focused && table.focusSearching) {
            if (std::optional<std::int64_t> const now = placeOfKey(table, *table.focusKey, table.focusIndex)) {
                table.focusLocate.request_stop();
                table.focusSearching = false;
                table.focusIndex = *now;
                table.widget->setFocusCell(static_cast<std::size_t>(*now), table.focusColumn);
            }
            return;
        }
        if (table.focused && table.focusUntold && total.known) {
            if (table.focusLeft) {
                table.focusIndex = std::min(table.focusIndex, std::max<std::int64_t>(total.rows - 1, 0));
            }
            if (table.focusIndex < total.rows) {
                table.focusUntold = false;
                tellLocatedFocus(table);
            }
        }
        if (table.focused && table.focusUnconfirmed && table.focusKey) {
            std::optional<TableRowRef> const ref = table.source.rows->row(table.focusIndex);
            std::optional<std::int64_t> const now = ref && ref->key == *table.focusKey
                                                        ? std::optional{table.focusIndex}
                                                        : placeOfKey(table, *table.focusKey, table.focusIndex);
            if (now) {
                table.focusUnconfirmed = false;
                table.focusIndex = *now;
                table.widget->setFocusCell(static_cast<std::size_t>(*now), table.focusColumn);
            }
        }
        if (table.focused) {
            std::optional<TableRowRef> const ref = table.source.rows->row(table.focusIndex);
            // The row at the focused index arrived, or took the place of the focused row that left the view; a row
            // that is there for any other reason (a reset that moved the focused key away) does not take focus.
            if (ref && table.focusKey != ref->key && (!table.focusKey || table.focusLeft)) {
                table.focusKey = ref->key;
                reportFocus(table);
            }
            table.focusLeft = table.focusLeft && !ref;
        }
    }

    // Asks the rows where the focused key is now, stopping an earlier ask.
    void locateFocus(SourceTable& table) {
        table.focusLocate.request_stop();
        table.focusLocate = ::core::async::StopSource{};
        ::core::async::StopToken const stop = table.focusLocate.get_token();
        table.source.rows->locateRow(
            *table.focusKey, stop, [this, &table, stop, alive = std::weak_ptr{table.alive}](RowLocation const& found) {
                if (alive.expired() || stop.stop_requested()) {
                    return;
                }
                static_cast<void>(runCallback(*_rt, [&] { focusLocated(table, found); }));
            });
    }

    // The rows' answer to where the focused key is. A key in the view takes focus at its index, its row kept
    // realized wherever it is; a key the view lacks has left it, and focus goes to the row now at its index, or the
    // last, as for a removed row. An answer the rows could not give leaves focus looking among the realized rows.
    // The backend is told after the pass, once the view's count is known (`focusUntold`): the last row of a count
    // not known yet is no row.
    void focusLocated(SourceTable& table, RowLocation const& found) {
        if (!table.focused || !table.focusSearching || !found) {
            return;
        }
        table.focusSearching = false;
        table.focusUntold = true;
        if (*found && **found >= 0) {
            table.focusIndex = **found;
        } else {
            table.focusWas = table.focusKey;
            table.focusLeft = true;
        }
        realizeWindow(table, [](std::int64_t) { return false; });
    }

    // Tells the backend the focused cell the rows located. A row they do not hold, outside the rows in view, is
    // scrolled to, so the window fetches it, as for a move onto a row not fetched; a held row is left where it is.
    static void tellLocatedFocus(SourceTable& table) {
        auto const index = static_cast<std::size_t>(table.focusIndex);
        // A row held at the located index with another key is of the view before the change that moved the key
        // there: the index is scrolled to, so its rows are realized, and focus waits for a realized row to hold the
        // key. A row not held names no key, and is fetched.
        std::optional<TableRowRef> const ref = table.source.rows->row(table.focusIndex);
        bool const other = !table.focusLeft && table.focusKey && ref && ref->key != *table.focusKey;
        if (!table.focusLeft && table.focusKey) {
            table.focusUnconfirmed = !ref || other;
        }
        VisibleRange range = focusRangeOf(table);
        if ((!ref || other) && (index < range.first || index >= range.first + range.count)) {
            range.first = index < range.first ? index : index + 1 - range.count;
            table.range = range;
            table.widget->scrollToRow(index);
            tellVisible(table);
        }
        if (!other) {
            table.widget->setFocusCell(index, table.focusColumn);
        }
    }

    static void reportFocus(SourceTable const& table) {
        // A grid holds its own focus: the key that took focus is handed to it, as a gesture's cell is.
        if (table.grid) {
            if (table.onGridFocus && table.focusColumn < table.columns.size()) {
                table.onGridFocus(
                    GridFocus{.key = table.focusKey,
                              .viewRow = static_cast<std::size_t>(std::max<std::int64_t>(table.focusIndex, 0)),
                              .column = table.columns.at(table.focusColumn).id});
            }
            return;
        }
        if (!table.onFocusCell || !table.focusKey) {
            return;
        }
        std::string const& columnId =
            table.focusColumn < table.columns.size() ? table.columns.at(table.focusColumn).id : std::string{};
        table.onFocusCell(CellRef{.key = *table.focusKey, .column = columnId});
    }

    // Where a view row is after a change: moved with the rows around it, or nothing when the change removed it. A
    // reset keeps the index.
    [[nodiscard]] static std::optional<std::int64_t> followIndex(table::ViewChange const& change, std::int64_t index) {
        auto position = static_cast<std::size_t>(std::max<std::int64_t>(index, 0));
        for (table::ViewOp const& op : change.ops) {
            switch (op.kind) {
                case table::ViewOp::Kind::Removed:
                    if (position >= op.first && position < op.first + op.count) {
                        return std::nullopt;
                    }
                    if (position >= op.first + op.count) {
                        position -= op.count;
                    }
                    break;
                case table::ViewOp::Kind::Inserted:
                    if (position >= op.first) {
                        position += op.count;
                    }
                    break;
                case table::ViewOp::Kind::Moved:
                    if (position == op.first) {
                        position = op.to;
                    } else if (op.first < position && position <= op.to) {
                        --position;
                    } else if (op.to <= position && position < op.first) {
                        ++position;
                    }
                    break;
                case table::ViewOp::Kind::Reset:
                case table::ViewOp::Kind::Changed:
                default:
                    break;
            }
        }
        return static_cast<std::int64_t>(position);
    }

    // Moves focus as a key asks: by rows or columns, to an end of the row, or to the first or last row of the view.
    // A row not fetched takes focus as a placeholder, and the view scrolls to it.
    void moveFocus(SourceTable& table, FocusMove move) {
        TableTotal const total = table.source.rows->mountedTotal();
        std::int64_t const last = std::max<std::int64_t>(total.rows - 1, 0);
        std::int64_t const page = std::max<std::int64_t>(static_cast<std::int64_t>(focusRangeOf(table).count), 1);
        std::int64_t index = table.focused ? table.focusIndex : 0;
        std::optional<std::size_t> const focusPlace = table.focused && table.focusColumn < table.placeOf.size()
                                                          ? table.placeOf.at(table.focusColumn)
                                                          : std::nullopt;
        std::size_t place = focusPlace.value_or(0);
        std::size_t const lastPlace = table.visible.empty() ? 0 : table.visible.size() - 1;
        switch (move) {
            case FocusMove::Up:
                index = std::max<std::int64_t>(index - 1, 0);
                break;
            case FocusMove::Down:
                index = std::min(index + 1, last);
                break;
            case FocusMove::PageUp:
                index = std::max<std::int64_t>(index - page, 0);
                break;
            case FocusMove::PageDown:
                index = std::min(index + page, last);
                break;
            case FocusMove::Left:
                place = place > 0 ? place - 1 : 0;
                break;
            case FocusMove::Right:
                place = std::min(place + 1, lastPlace);
                break;
            case FocusMove::Home:
                place = 0;
                break;
            case FocusMove::End:
                place = lastPlace;
                break;
            case FocusMove::First:
                index = 0;
                break;
            case FocusMove::Last:
                index = last;
                // A total that is a lower bound: the last row known.
                if (total.kind == table::TotalKind::AtLeast) {
                    while (index > 0 && !table.source.rows->row(index)) {
                        --index;
                    }
                }
                break;
            default:
                break;
        }
        if (table.visible.empty()) {
            return;
        }
        if (table.grid) {
            handFocus(table, index, table.visible.at(place));
            return;
        }
        placeFocus(table, index, table.visible.at(place));
        reportFocus(table);
    }

    // The view position of a key, scanning the view: a table selects cells only while its rows are all on the client.
    [[nodiscard]] static std::optional<std::size_t> viewRowOf(SourceTable const& table, Key const& key) {
        TableTotal const total = table.source.rows->mountedTotal();
        for (std::int64_t index = 0; index < total.rows; ++index) {
            std::optional<TableRowRef> const ref = table.source.rows->row(index);
            if (ref && ref->key == key) {
                return static_cast<std::size_t>(index);
            }
        }
        return std::nullopt;
    }

    // Shows a cell range at its corners' view rows and visible places now: a sort or a column move redraws the
    // rectangle the same corners span. A corner whose row is not in the view, or whose column is hidden, shows none.
    static void showCells(SourceTable& table) {
        if (!table.cells) {
            return;
        }
        std::optional<std::array<std::size_t, 4>> rect;
        if (table.cellRange) {
            auto const placeOf = [&table](std::string const& columnId) -> std::optional<std::size_t> {
                auto const column = std::ranges::find(table.columns, columnId, &TableColumn::id);
                auto const index = static_cast<std::size_t>(column - table.columns.begin());
                return column == table.columns.end() ? std::nullopt : table.placeOf.at(index);
            };
            std::optional<std::size_t> const anchorRow = viewRowOf(table, table.cellRange->anchor.key);
            std::optional<std::size_t> const focusRow = viewRowOf(table, table.cellRange->focus.key);
            std::optional<std::size_t> const anchorPlace = placeOf(table.cellRange->anchor.column);
            std::optional<std::size_t> const focusPlace = placeOf(table.cellRange->focus.column);
            if (anchorRow && focusRow && anchorPlace && focusPlace) {
                rect = std::array<std::size_t, 4>{
                    std::min(*anchorRow, *focusRow),
                    std::max(*anchorRow, *focusRow) - std::min(*anchorRow, *focusRow) + 1,
                    std::min(*anchorPlace, *focusPlace),
                    std::max(*anchorPlace, *focusPlace) - std::min(*anchorPlace, *focusPlace) + 1};
            }
        }
        if (table.cellRectTold && rect == table.cellRect) {
            return;
        }
        table.cellRectTold = true;
        table.cellRect = rect;
        table.widget->setCellSelection(rect);
    }

    // A rectangle the backend reports, by view rows and visible places, as a range by keys and column ids; a corner
    // outside the view or the visible columns reports nothing.
    [[nodiscard]] static std::optional<CellRange> cellsAt(SourceTable const& table, std::size_t anchorRow,
                                                          std::size_t anchorPlace, std::size_t focusRow,
                                                          std::size_t focusPlace) {
        auto const cell = [&table](std::size_t row, std::size_t place) -> std::optional<CellRef> {
            std::optional<TableRowRef> const ref = table.source.rows->row(static_cast<std::int64_t>(row));
            if (!ref || place >= table.visible.size() || table.visible.at(place) >= table.columns.size()) {
                return std::nullopt;
            }
            return CellRef{.key = ref->key, .column = table.columns.at(table.visible.at(place)).id};
        };
        std::optional<CellRef> anchor = cell(anchorRow, anchorPlace);
        std::optional<CellRef> focus = cell(focusRow, focusPlace);
        if (!anchor || !focus) {
            return std::nullopt;
        }
        return CellRange{.anchor = std::move(*anchor), .focus = std::move(*focus)};
    }

    // A grid's focus moves where the grid says: a gesture is handed to it as a cell, a row not fetched yet by its view
    // position alone.
    static void handFocus(SourceTable const& table, std::int64_t index, std::size_t column) {
        if (!table.onGridFocus || column >= table.columns.size()) {
            return;
        }
        std::optional<TableRowRef> const ref = table.source.rows->row(index);
        table.onGridFocus(GridFocus{.key = ref ? std::optional<Key>{ref->key} : std::nullopt,
                                    .viewRow = static_cast<std::size_t>(std::max<std::int64_t>(index, 0)),
                                    .column = table.columns.at(column).id});
    }

    // Shows the focus a grid holds: its key's row where a realized row holds it, else the view position it gives, and
    // its column by id.
    void gridFocusFrom(SourceTable& table, std::optional<GridFocus> const& focus) {
        if (!focus) {
            return;
        }
        auto const column = std::ranges::find(table.columns, focus->column, &TableColumn::id);
        if (column == table.columns.end()) {
            return;
        }
        auto index = static_cast<std::int64_t>(focus->viewRow);
        if (focus->key) {
            std::optional<TableRowRef> const ref = table.source.rows->row(index);
            if (!ref || ref->key != *focus->key) {
                index = placeOfKey(table, *focus->key, index).value_or(index);
            }
        }
        placeFocus(table, index, static_cast<std::size_t>(column - table.columns.begin()));
    }

    // Puts focus on a cell by view row and column: the row is kept realized, scrolled to when it is outside the rows
    // in view, and the backend shows it.
    void placeFocus(SourceTable& table, std::int64_t index, std::size_t column) {
        table.focused = true;
        table.focusSearching = false;
        table.focusUntold = false;
        table.focusWas.reset();
        table.focusUnconfirmed = false;
        table.focusLocate.request_stop();
        table.focusIndex = index;
        table.focusColumn = column;
        std::optional<TableRowRef> const ref = table.source.rows->row(index);
        table.focusKey = ref ? std::optional<Key>{ref->key} : std::nullopt;
        VisibleRange range = focusRangeOf(table);
        auto const position = static_cast<std::size_t>(index);
        if (position < range.first || position >= range.first + range.count) {
            range.first = position < range.first ? position : position + 1 - range.count;
            table.range = range;
            table.widget->scrollToRow(position);
            tellVisible(table);
        }
        table.windowDirty = true;
        realizeWindow(table, [](std::int64_t) { return false; });
        table.widget->setFocusCell(position, table.focusColumn);
    }

    // Takes focus from `Table::focus`: the cell's row by key among the realized rows, and its column by id.
    void focusFrom(SourceTable& table, std::optional<CellRef> const& cell) {
        if (!cell) {
            return;
        }
        auto const column = std::ranges::find(table.columns, cell->column, &TableColumn::id);
        if (column == table.columns.end()) {
            return;
        }
        auto const row = std::ranges::find(table.realized, std::optional<Key>{cell->key}, &SourceTable::Realized::key);
        if (row == table.realized.end()) {
            return;
        }
        table.focused = true;
        table.focusSearching = false;
        table.focusUntold = false;
        table.focusWas.reset();
        table.focusUnconfirmed = false;
        table.focusLocate.request_stop();
        table.focusKey = cell->key;
        table.focusIndex = row->index;
        table.focusColumn = static_cast<std::size_t>(column - table.columns.begin());
        table.windowDirty = true;
        realizeWindow(table, [](std::int64_t) { return false; });
        table.widget->setFocusCell(static_cast<std::size_t>(table.focusIndex), table.focusColumn);
    }

    // Focuses a realized row's cell, as a cell menu opening on it does, and reports it as a key's move is reported.
    void focusAt(SourceTable& table, Key const& key, std::size_t column) {
        if (column >= table.columns.size()) {
            return;
        }
        // The focused cell's own entries, asked for a key, leave the focus and the view where they are.
        if (table.focused && table.focusKey == key && table.focusColumn == column) {
            return;
        }
        auto const row = std::ranges::find(table.realized, std::optional<Key>{key}, &SourceTable::Realized::key);
        if (row == table.realized.end()) {
            return;
        }
        if (table.grid) {
            handFocus(table, row->index, column);
            return;
        }
        table.focused = true;
        table.focusSearching = false;
        table.focusUntold = false;
        table.focusWas.reset();
        table.focusUnconfirmed = false;
        table.focusLocate.request_stop();
        table.focusKey = key;
        table.focusIndex = row->index;
        table.focusColumn = column;
        table.windowDirty = true;
        realizeWindow(table, [](std::int64_t) { return false; });
        table.widget->setFocusCell(static_cast<std::size_t>(table.focusIndex), table.focusColumn);
        reportFocus(table);
    }

    // Moves focus and the highlight from a row's old key to its new one, for each new generation of moves. Focus on
    // the old key, or left by it when its row went, goes to the new key: at once when a realized row holds it, else
    // the rows are asked where it is, as after a reset, and the change that removes the old row moves it nowhere
    // meanwhile; an answer that the view lacks the new key hands focus to the row at its index.
    void applyKeyMoves(SourceTable& table, KeyMoves const& moves) {
        if (moves.generation <= table.keyMovesApplied) {
            return;
        }
        table.keyMovesApplied = moves.generation;
        // A key focus left is followed by the first generation of moves after it left, and by no later one.
        std::optional<Key> const left = std::exchange(table.focusWas, std::nullopt);
        for (auto const& [from, target] : moves.moves) {
            if (table.highlightKey == from) {
                table.highlightKey = target;
                if (std::optional<std::int64_t> const now = placeOfKey(table, target, table.highlightIndex)) {
                    table.highlightSearching = false;
                    table.highlightIndex = *now;
                    table.widget->setHighlightedRow(static_cast<std::size_t>(*now));
                } else {
                    table.highlightSearching = true;
                }
            }
            if (!table.focused || (table.focusKey != from && left != from)) {
                continue;
            }
            table.focusKey = target;
            table.focusUnconfirmed = false;
            table.focusLeft = false;
            table.focusUntold = false;
            table.focusLocate.request_stop();
            if (std::optional<std::int64_t> const now = placeOfKey(table, target, table.focusIndex)) {
                table.focusSearching = false;
                table.focusIndex = *now;
                table.widget->setFocusCell(static_cast<std::size_t>(*now), table.focusColumn);
            } else {
                table.focusSearching = true;
                locateFocus(table);
            }
            reportFocus(table);
        }
    }

    // Takes a highlight: the rows fetch around it, the view scrolls to it and marks it, and its key is followed.
    static void highlightFrom(SourceTable& table, std::optional<TableHighlight> const& highlight) {
        table.highlightSearching = false;
        if (!highlight) {
            if (table.highlightKey) {
                table.highlightKey.reset();
                table.widget->setHighlightedRow(std::nullopt);
            }
            return;
        }
        table.highlightKey = highlight->key;
        table.highlightIndex = static_cast<std::int64_t>(highlight->row);
        std::size_t const overscan = table.virtualize.overscan;
        std::size_t const first = highlight->row > overscan ? highlight->row - overscan : 0;
        table.source.rows->visible(static_cast<std::int64_t>(first),
                                   static_cast<std::int64_t>(table.range.count + (2 * overscan)), table.fetched);
        table.widget->scrollToRow(highlight->row);
        table.widget->setHighlightedRow(highlight->row);
    }

    // Where a key is among the realized rows' places in the view the rows now hold, or nothing.
    [[nodiscard]] static std::optional<std::int64_t> placeOfKey(SourceTable const& table, Key const& key,
                                                                std::int64_t also) {
        TableRows const& rows = *table.source.rows;
        auto const holds = [&rows, &key](std::int64_t index) {
            std::optional<TableRowRef> const ref = rows.row(index);
            return ref && ref->key == key;
        };
        if (holds(also)) {
            return also;
        }
        for (SourceTable::Realized const& row : table.realized) {
            if (holds(row.index)) {
                return row.index;
            }
        }
        return std::nullopt;
    }

    // Follows the focused and the highlighted rows through a change of the rows. A reset (a new sort, filter or
    // description) gives no places, and the rows may come back later (a server's pages are dropped first), so a key
    // no realized row has is looked for after each pass until one has it; meanwhile the highlight shows no mark and
    // focus stays on its key. Focus also asks the rows where its key is, since a new order can move it far from the
    // rows realized; only their answer that the view lacks it moves focus to another row. An index at or past the
    // row count before the change names no row, so nothing moves it; after a reset no row is counted until the next
    // pass, so a first page that arrives before it moves nothing either.
    void follow(SourceTable& table, table::ViewChange const& change) {
        bool const reset = std::ranges::any_of(
            change.ops, [](table::ViewOp const& op) { return op.kind == table::ViewOp::Kind::Reset; });
        if (reset) {
            table.lastCount = 0;
        }
        auto const followed = [&table, &change](std::int64_t index) -> std::optional<std::int64_t> {
            if (std::cmp_greater_equal(index, table.lastCount)) {
                return index;
            }
            return followIndex(change, index);
        };
        if (table.highlightKey && !table.highlightSearching) {
            std::optional<std::int64_t> const now =
                reset ? placeOfKey(table, *table.highlightKey, table.highlightIndex) : followed(table.highlightIndex);
            if (now) {
                if (*now != table.highlightIndex) {
                    table.highlightIndex = *now;
                    table.widget->setHighlightedRow(static_cast<std::size_t>(*now));
                }
            } else if (reset) {
                table.highlightSearching = true;
                table.widget->setHighlightedRow(std::nullopt);
            } else {
                table.highlightKey.reset();
                table.widget->setHighlightedRow(std::nullopt);
            }
        }
        if (table.focused && reset && table.focusKey) {
            if (std::optional<std::int64_t> const now = placeOfKey(table, *table.focusKey, table.focusIndex)) {
                table.focusLocate.request_stop();
                table.focusSearching = false;
                if (*now != table.focusIndex) {
                    table.focusIndex = *now;
                    table.widget->setFocusCell(static_cast<std::size_t>(*now), table.focusColumn);
                }
            } else {
                table.focusSearching = true;
                table.focusUntold = false;
                locateFocus(table);
            }
            return;
        }
        if (table.focused && !table.focusSearching) {
            TableTotal const total = table.source.rows->mountedTotal();
            // A focused row the change removed leaves focus at its view index, on the row now there, or the last; a
            // count not known yet names no last row.
            std::optional<std::int64_t> const now = followed(table.focusIndex);
            if (!now && !table.focusLeft) {
                table.focusWas = table.focusKey;
            }
            table.focusLeft = table.focusLeft || !now.has_value();
            table.focusIndex = now.value_or(table.focusIndex);
            if (total.known) {
                table.focusIndex = std::min(table.focusIndex, std::max<std::int64_t>(total.rows - 1, 0));
            }
        }
    }

    // Whether a change names the view index @p index as updated: a reset names every row, and a row inserted,
    // changed or moved to the index is named.
    [[nodiscard]] static bool namesUpdated(table::ViewChange const& change, std::int64_t index) {
        auto const position = static_cast<std::size_t>(index);
        return std::ranges::any_of(change.ops, [position](table::ViewOp const& op) {
            switch (op.kind) {
                case table::ViewOp::Kind::Reset:
                    return true;
                case table::ViewOp::Kind::Changed:
                case table::ViewOp::Kind::Inserted:
                    return position >= op.first && position < op.first + op.count;
                case table::ViewOp::Kind::Moved:
                    return position == op.to;
                case table::ViewOp::Kind::Removed:
                default:
                    return false;
            }
        });
    }

    // What an editable table adds: its focus held by a grid, a click on a cell, typed text, its own keys, and focus
    // leaving it.
    void bindGrid(reactive::Scope& scope, Table const& spec, TableWidget& widget, SourceTable& table) {
        table.grid = spec.gridFocus.has_value();
        table.onGridFocus = event(spec.onGridFocus);
        table.editor = spec.editor;
        if (spec.onText) {
            widget.setOnText(event(spec.onText));
        }
        if (spec.onFocusOut) {
            widget.setOnFocusOut(event(spec.onFocusOut));
        }
        if (table.grid || spec.onActivateCell || spec.onClickCell) {
            widget.setOnCellClick([this, &table, activate = spec.onActivateCell, clicked = spec.onClickCell](
                                      std::size_t viewRow, std::size_t column, bool doubleClick) {
                static_cast<void>(runCallback(*_rt, [&] {
                    if (column >= table.columns.size()) {
                        return;
                    }
                    auto const index = static_cast<std::int64_t>(viewRow);
                    std::optional<TableRowRef> const ref = table.source.rows->row(index);
                    if (!doubleClick) {
                        if (table.grid) {
                            handFocus(table, index, column);
                        } else if (ref) {
                            focusAt(table, ref->key, column);
                        }
                        if (ref && clicked) {
                            clicked(CellRef{.key = ref->key, .column = table.columns.at(column).id});
                        }
                        return;
                    }
                    if (ref && activate) {
                        activate(CellRef{.key = ref->key, .column = table.columns.at(column).id});
                    }
                }));
            });
        }
        if (!spec.onKey) {
            return;
        }
        // The table's own chords join the document's keys in one declaration; a chord of both is the table's.
        bind(scope, spec.keyChords,
             [this, &widget, onKey = spec.onKey, common = spec.common.keys](std::vector<std::string> const& chords) {
                 std::vector<std::string> all;
                 all.reserve(common.size());
                 for (KeyBinding const& binding : common) {
                     all.push_back(binding.chord);
                 }
                 for (std::string const& chord : chords) {
                     if (!std::ranges::contains(all, chord)) {
                         all.push_back(chord);
                     }
                 }
                 widget.setKeys(all, [this, &widget, onKey, common, chords](std::string const& chord) {
                     static_cast<void>(runCallback(*_rt, [&] {
                         if (std::ranges::contains(chords, chord)) {
                             KeyOutcome const outcome = onKey(chord);
                             if (outcome == KeyOutcome::FocusNext || outcome == KeyOutcome::FocusPrevious) {
                                 static_cast<void>(focusAlong(widget, outcome == KeyOutcome::FocusNext));
                             }
                             return;
                         }
                         auto const bound = std::ranges::find(common, chord, &KeyBinding::chord);
                         if (bound != common.end() && bound->onPress) {
                             bound->onPress();
                         }
                     }));
                 });
             });
    }

    // A table over a `TableSource`. Rows are windowed, except a table over a document's rows below its threshold,
    // which realizes every row: the mount realizes the rows the backend reports in view and `overscan` rows either
    // side; before any report, the first `overscan` rows of a server's rows, and none of a document's (`shownOf`).
    // Columns are windowed once the backend reports its horizontal viewport and `windowingOf` says so; until then
    // every visible column is in the window. The mount tells the rows which rows and columns are in view, and applies
    // each `ViewChange` to the realized rows by key. Rows first, then the selection, as for a table over `rows`.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one handler per table seam.
    Widget& mountSourceTable(reactive::Scope& scope, Table const& spec, ContainerWidget* parent) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the mount dispatches here only for a spec with a source.
        TableSource const& source = *spec.source;
        if (!source.rows || !source.cellView) {
            throw std::logic_error{"morph::ui: a table source needs its rows and its cell view"};
        }
        TableWidget& widget = scope.adopt(_backend->createTable(parent));
        applyCommon(scope, widget, spec.common);
        checkNoShare(spec.columns);
        if (spec.bands.empty()) {
            widget.setColumns(spec.columns);
        } else {
            // A column under a band that shows its unit does not repeat it.
            std::vector<TableColumn> headed = spec.columns;
            for (TableColumn& column : headed) {
                if (!columnUnitShown(spec.bands, column.id, column.unit)) {
                    column.unit.clear();
                }
            }
            widget.setColumns(headed);
        }
        widget.setShowHeader(spec.showHeader);
        widget.setSelectionMode(spec.selectionMode);
        if (spec.selectColumn) {
            widget.setSelectColumn(true);
            bind(scope, spec.selectAllLabel, [&widget](std::string const& label) { widget.setSelectAllLabel(label); });
        }
        widget.setOnActivate(event(spec.onActivate));
        if (spec.onSelectAll) {
            widget.setOnSelectAll(event(spec.onSelectAll));
        }
        auto& table = scope.make<SourceTable>();
        table.bands = spec.bands;
        if (!source.alwaysWindowed) {
            auto& spans = scope.make<SpanTable>();
            spans.columns = spec.columns.size();
            spans.threshold = spec.virtualize.threshold;
            table.spans = &spans;
        }
        if (std::ranges::any_of(spec.columns, &TableColumn::sortable)) {
            bind(scope, spec.sort, [this, &widget, &table](std::vector<TableSort> const& chain) {
                widget.setSort(chain);
                if (table.spans != nullptr) {
                    table.spans->sorted = !chain.empty();
                    resolveSpans(widget, *table.spans);
                }
            });
            widget.setOnSort(event(spec.onSort));
        }
        bindTableConfig(scope, spec, widget,
                        [this, &table](Key const& key, std::size_t column) { focusAt(table, key, column); });
        bind(scope, spec.status, [&widget](TableStatus const& status) { widget.setStatus(status); });
        widget.setOnRetry(event(spec.onRetry));
        table.source = source;
        table.widget = &widget;
        table.columns = spec.columns;
        table.virtualize = spec.virtualize;
        table.depth = scope.depth() + 1;
        table.onFocusCell = event(spec.onFocusCell);
        _extents = table.extents;
        arrange(table, TableArrangement{});
        auto const pass = [this, &table] {
            if (table.mounted) {
                static_cast<void>(
                    runCallback(*_rt, [&] { realizeWindow(table, [](std::int64_t) { return false; }); }));
            }
        };
        bind(scope, spec.arrangement, [&table, pass](TableArrangement const& arrangement) {
            if (arrangement != table.arrangement) {
                arrange(table, arrangement);
                pass();
                showCells(table);
            }
        });
        table.source.rows->onChange(
            [this, &table, alive = std::weak_ptr{table.alive}](table::ViewChange const& change) {
                if (alive.expired()) {
                    return;
                }
                ++_stats.viewChanges;
                if (std::ranges::any_of(
                        change.ops, [](table::ViewOp const& op) { return op.kind == table::ViewOp::Kind::Reset; })) {
                    ++_stats.viewResets;
                }
                static_cast<void>(runCallback(*_rt, [&] {
                    follow(table, change);
                    realizeWindow(table, [&change](std::int64_t index) { return namesUpdated(change, index); });
                    showCells(table);
                }));
            });
        widget.setOnVisibleRangeChanged([this, &table](std::size_t first, std::size_t count) {
            static_cast<void>(runCallback(*_rt, [&] {
                table.range = VisibleRange{.first = first, .count = count};
                realizeWindow(table, [](std::int64_t) { return false; });
                tellVisible(table);
            }));
        });
        widget.setOnHorizontalRange([this, &table](int scrollX, int viewport) {
            static_cast<void>(runCallback(*_rt, [&] {
                table.horizontal = true;
                table.scrollX = std::max(scrollX, 0);
                table.viewport = std::max(viewport, 0);
                table.windowDirty = true;
                table.stretchDirty = true;
                realizeWindow(table, [](std::int64_t) { return false; });
            }));
        });
        widget.setOnColumnMeasured([this, &table](std::size_t column, ColumnMeasure const& measure) {
            static_cast<void>(runCallback(*_rt, [&] { measured(table, column, measure); }));
        });
        widget.setOnFocusMove(
            [this, &table](FocusMove move) { static_cast<void>(runCallback(*_rt, [&] { moveFocus(table, move); })); });
        widget.setOnSelectRange(event(spec.onSelectRange));
        bindGrid(scope, spec, widget, table);
        _rt->untracked([&] {
            realizeWindow(table, [](std::int64_t) { return false; });
            tellVisible(table);
            table.mounted = true;
        });
        bind(scope, spec.focus, [this, &table](std::optional<CellRef> const& cell) {
            static_cast<void>(runCallback(*_rt, [&] { focusFrom(table, cell); }));
        });
        bind(scope, spec.highlight, [this, &table](std::optional<TableHighlight> const& highlight) {
            static_cast<void>(runCallback(*_rt, [&] { highlightFrom(table, highlight); }));
        });
        bind(scope, spec.keyMoves, [this, &table](KeyMoves const& moves) {
            static_cast<void>(runCallback(*_rt, [&] { applyKeyMoves(table, moves); }));
        });
        if (spec.gridFocus) {
            bind(scope, *spec.gridFocus, [this, &table](std::optional<GridFocus> const& focus) {
                static_cast<void>(runCallback(*_rt, [&] { gridFocusFrom(table, focus); }));
            });
        }
        if (spec.cellSelection) {
            table.cells = true;
            widget.setSelectionMode(SelectionMode::None);
            widget.setOnCellSelect(
                [this, &table, onSelect = spec.onCellSelect](std::size_t anchorRow, std::size_t anchorPlace,
                                                             std::size_t focusRow, std::size_t focusPlace) {
                    static_cast<void>(runCallback(*_rt, [&] {
                        std::optional<CellRange> range = cellsAt(table, anchorRow, anchorPlace, focusRow, focusPlace);
                        if (range && onSelect) {
                            onSelect(std::move(range));
                        }
                    }));
                });
            bind(scope, *spec.cellSelection, [this, &table](std::optional<CellRange> const& range) {
                static_cast<void>(runCallback(*_rt, [&] {
                    table.cellRange = range;
                    showCells(table);
                }));
            });
        }
        if (spec.editing) {
            bind(scope, *spec.editing, [this, &table](std::optional<CellRef> const& cell) {
                static_cast<void>(runCallback(*_rt, [&] {
                    table.editWanted = cell;
                    syncEditor(table);
                }));
            });
        }
        // While the selection is of the view the backend marks it, and the key list it holds is not sent over it.
        auto& viewing = scope.make<bool>(false);
        widget.setOnSelectionChange(
            inputEvent(spec.onSelectionChange,
                       controlled(scope, spec.selection, [&widget, &viewing](std::vector<Key> const& keys) {
                           if (!viewing) {
                               widget.setSelection(keys);
                           }
                       })));
        bindSelectionView(scope, spec, widget, viewing);
        return widget;
    }

    // What the backend measured of @p column: a content column is as wide as its widest cell and its header, which
    // only grow once measured, and a change left of the first column fully in view moves the scroll offset by as much,
    // so the columns in view stay where they are. A
    // stretch column's measure feeds the fit of every stretch column to the table, which may narrow as well as widen
    // them. A fixed column keeps its width.
    void measured(SourceTable& table, std::size_t column, ColumnMeasure const& reported) {
        if (column >= table.measures.size()) {
            return;
        }
        ColumnMeasure& known = table.measures.at(column);
        ColumnMeasure const next{.header = std::max(known.header, reported.header),
                                 .natural = std::max(known.natural, reported.natural),
                                 .least = std::max(known.least, reported.least)};
        if (next == known) {
            return;
        }
        known = next;
        std::optional<std::size_t> const placed =
            column < table.placeOf.size() ? table.placeOf.at(column) : std::nullopt;
        if (!placed) {
            return;
        }
        std::size_t const place = *placed;
        ColumnExtents& extents = *table.extents;
        if (std::ranges::contains(table.stretchPlaces, place)) {
            table.stretchDirty = true;
            table.windowDirty = true;
            realizeWindow(table, [](std::int64_t) { return false; });
            return;
        }
        if (table.columns.at(column).width.kind != Sizing::Kind::Content) {
            return;
        }
        int const width = contentWidthOf(next, table.measure);
        int const before = extents.offsetOf(place + 1) - extents.offsetOf(place);
        std::size_t const firstFull = firstFullPlace(table);
        if (!extents.fit(place, width)) {
            return;
        }
        if (table.horizontal && place >= table.pinnedShown && place < firstFull) {
            table.scrollX += width - before;
            table.widget->setHorizontalOffset(table.scrollX);
        }
        table.windowDirty = true;
        table.stretchDirty = true;
        realizeWindow(table, [](std::int64_t) { return false; });
    }

    // The model, then the session, then the rows, then the Effect(s): destroyed in reverse, so an Effect stops
    // first, the rows (whose slots point into the session's snapshot) go next, and the model outlives its session.
    // @p virtualize is null for a ForEach (no backend seam exists yet to dewindow a bare keyed stack); when
    // non-null but @p container is not also a TableWidget (today, impossible -- only Table passes a non-null
    // virtualize, and only its mountKind gives it a TableWidget -- kept as a defensive check, not a real case),
    // virtualization is silently not wired, same as if it were null.
    void mountRows(reactive::Scope& scope, std::shared_ptr<ForEachModel const> const& model,
                   ContainerWidget& container, RowHook onRow, Virtualization const* virtualize,
                   RowsObserver observer = {}) {
        if (!model) {
            return;
        }
        scope.make<std::shared_ptr<ForEachModel const>>(model);
        ForEachSession& session = scope.adopt(model->open(*_rt));
        auto& rows = scope.make<KeyedRows>();
        TableWidget* const table = virtualize != nullptr ? dynamic_cast<TableWidget*>(&container) : nullptr;
        if (table == nullptr) {
            scope.effect([this, &session, &rows, &container, depth = scope.depth() + 1, onRow, overlay = _overlay] {
                std::vector<Key> const keys = session.pull();  // tracked: the rows are this Effect's only source
                // Untracked: what a row view reads while it is built, or what a kept row's update touches, must
                // not become a dependency of the whole list.
                underOverlay(overlay, [&] {
                    _rt->untracked([&] { reconcile(depth, session, rows.list, container, keys, onRow); });
                });
            });
            return;
        }
        // The windowing state: written by the backend's own scroll callback (via widgetEvent, so this is never
        // written from inside a flush this Effect caused), read by both Effects below -- the rows Effect, so a
        // brand-new row's very first mount already respects the window instead of needing a later windowing pass
        // to correct it; the windowing Effect, so a scroll alone realizes/releases without re-running reconcile.
        auto& range = scope.make<reactive::Signal<VisibleRange>>(*_rt, VisibleRange{});
        table->setOnVisibleRangeChanged([this, &range](std::size_t first, std::size_t count) {
            runCallback(*_rt, [&] { range.set({.first = first, .count = count}); });
        });
        // The observer sees every pass that changes the rows, the windowing one included.
        auto const observerTrack = observer.track;
        auto const around = observedPass(std::move(observer), rows);
        // Whether the backend was last told the rows are windowed: a count that falls back below the threshold then
        // owes it the end of the window.
        auto& windowed = scope.make<bool>(false);
        scope.effect([this, &session, &rows, &container, &range, &windowed, setting = *virtualize,
                      depth = scope.depth() + 1, onRow, table, overlay = _overlay, around, track = observerTrack] {
            // tracked: the rows are one source, and the sort that `track` reads is the other
            std::vector<Key> const keys = session.pull();
            if (track) {
                track();
            }
            VisibleRange const window = range.peek();  // untracked: a scroll alone must not re-run this Effect
            around([&] {
                underOverlay(overlay, [&] {
                    _rt->untracked([&] {
                        reconcileWindowed(depth, session, rows, container, *table, keys, onRow, window, setting,
                                          windowed);
                    });
                });
            });
            publishRealizedWindow(*table, rows, keys.size(), setting, windowed);
        });
        scope.effect([this, &rows, &container, &range, setting = *virtualize, depth = scope.depth() + 1, onRow, table,
                      overlay = _overlay, around] {
            VisibleRange const window = range.get();  // tracked: this Effect's own trigger
            // Below threshold, every row is already realized (reconcile's own unconditional path, §4.4), and
            // setRealizedWindow is not called at all -- not even once with "everything realized" -- so a table
            // that never crosses its threshold is indistinguishable, on the backend, from one with no
            // virtualization machinery at all.
            if (rows.list.size() < setting.threshold) {
                return;
            }
            around([&] {
                underOverlay(overlay,
                             [&] { applyWindow(depth, rows.list, container, window, setting.overscan, onRow); });
            });
            auto const [first, count] = realizedExtent(rows.list);
            table->setRealizedWindow(first, count, rows.list.size());
        });
    }

    // The snapshot index of each key's first occurrence, in order; every later duplicate is reported.
    [[nodiscard]] std::vector<std::size_t> firstOccurrences(std::vector<Key> const& keys) {
        std::unordered_set<Key> seen;
        std::vector<std::size_t> order;
        order.reserve(keys.size());
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (seen.insert(keys.at(index)).second) {
                order.push_back(index);
            } else {
                _rt->core()->report(site::kDuplicateKey);
            }
        }
        return order;
    }

    // All or nothing: the row's state and its widget are each built in a scope of their own, one level deeper than
    // the ForEach's, which the caller takes only once the mount completed. A new row's widget is realized in the
    // same pass as its state (so it is eligible for autofocus like any other newly-mounted content); a widget
    // realized later, on a row whose state already exists, goes through `realizeRowWidget` instead, which is not
    // its own `mountPass` -- see that function's own comment for why.
    [[nodiscard]] KeyedRow mountRow(std::size_t depth, ForEachSession& session, ContainerWidget& container,
                                    Key const& key, std::size_t index, RowHook const& onRow) {
        return mountPass([&] {
            KeyedRow row{.key = key, .stateScope = std::make_unique<reactive::Scope>(*_rt, depth)};
            row.slot = &row.stateScope->adopt(session.makeRow(index));
            row.widgetScope = std::make_unique<reactive::Scope>(*_rt, depth);
            row.widget = mount(*row.widgetScope, row.slot->view(), &container);
            row.widgetState = row.widget != nullptr ? RowWidgetState::Realized : RowWidgetState::NullView;
            if (row.widget != nullptr && onRow) {
                onRow(*row.widgetScope, row.slot->view(), *row.widget, key);
            }
            return row;
        });
    }

    // Realizes a widget for a row whose state (`stateScope`/`slot`) already exists -- a row re-entering the
    // visible range after being dewindowed. Deliberately not a `mountPass` of its own: `mountPass` gives the first
    // `autofocus` widget it mounts the focus once it completes, which is right for a row mounting for the first
    // time (`mountRow`, inside the ForEach's own outer pass) but wrong here -- a row that already existed and
    // simply scrolled back into view must not steal focus back from wherever the user's focus already is. `mount`
    // itself carries no such behavior; only `mountPass` does, so simply not using it is what suppresses it.
    void realizeRowWidget(std::size_t depth, KeyedRow& row, ContainerWidget& container, RowHook const& onRow) {
        row.widgetScope = std::make_unique<reactive::Scope>(*_rt, depth);
        row.widget = mount(*row.widgetScope, row.slot->view(), &container);
        row.widgetState = row.widget != nullptr ? RowWidgetState::Realized : RowWidgetState::NullView;
        if (row.widget != nullptr && onRow) {
            onRow(*row.widgetScope, row.slot->view(), *row.widget, row.key);
        }
    }

    // Releases a row's widget, keeping its state: the container sees an ordinary removal (`releasing` fires exactly
    // as it does for a real removal), but `stateScope`/`slot` are untouched, so the row's signal keeps tracking the
    // snapshot and is ready for `realizeRowWidget` to mount a fresh widget against it later.
    static void dewindowRowWidget(KeyedRow& row) {
        row.widgetScope.reset();
        row.widget = nullptr;
        row.widgetState = RowWidgetState::Dewindowed;
    }

    // Builds only a new key's state -- no widget, no mount, no onRow call -- for a row `reconcile` decides is
    // outside the current window on its very first appearance (a virtualized table above its threshold). The row
    // starts `Dewindowed`, exactly as if it had been realized once and then released, so `realizeRowWidget` can
    // bring it in later with no special case for "never had a widget" versus "had one and lost it."
    [[nodiscard]] KeyedRow mountRowState(std::size_t depth, ForEachSession& session, Key const& key,
                                         std::size_t index) {
        KeyedRow row{.key = key, .stateScope = std::make_unique<reactive::Scope>(*_rt, depth)};
        row.slot = &row.stateScope->adopt(session.makeRow(index));
        row.widgetState = RowWidgetState::Dewindowed;
        return row;
    }

    // Moves the widgets into the snapshot's order, and `rows` with them. A move that throws is recorded in `failure`
    // when it is the first failure, and leaves `rows` mirroring the container as it stands.
    void reorder(std::vector<KeyedRow>& rows, ContainerWidget& container, std::vector<Key> const& keys,
                 std::vector<std::size_t> const& order, std::exception_ptr& failure) {
        std::unordered_map<Key, std::size_t> position;  // key -> index in `rows`
        std::unordered_map<Widget const*, std::size_t> rowOf;
        std::vector<Widget*> present;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            KeyedRow const& row = rows.at(i);
            position.emplace(row.key, i);
            if (row.widget != nullptr) {
                rowOf.emplace(row.widget, i);
                present.push_back(row.widget);
            }
        }
        std::vector<Widget*> target;
        target.reserve(present.size());
        for (std::size_t const index : order) {
            if (auto const found = position.find(keys.at(index)); found != position.end()) {
                if (KeyedRow const& row = rows.at(found->second); row.widget != nullptr) {
                    target.push_back(row.widget);
                }
            }
        }
        std::vector<KeyedRow> ordered;
        ordered.reserve(rows.size());
        try {
            reorderChildren(container, present, target);
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        reorderChain(container, present);
        // `present` is the container's order now: the snapshot's, unless a move threw. Rows without a widget have
        // no place in the container and go last.
        for (Widget const* const widget : present) {
            ordered.push_back(std::move(rows.at(rowOf.at(widget))));
        }
        for (KeyedRow& row : rows) {
            if (row.widget == nullptr) {
                ordered.push_back(std::move(row));
            }
        }
        rows = std::move(ordered);
    }

    // Brings `rows` to the snapshot `keys`. `rows` mirrors the container's order on entry and on exit, also when
    // something throws, so a failure costs this run's remaining work and never the bookkeeping of the next one.
    // A row that fails to mount leaves nothing behind and does not stop the other rows; the first failure is
    // rethrown at the end, for the Effect to report, and the next run mounts the row again.
    // @p window is the virtualization window a new key's widget is realized against on its very first appearance,
    // or null for a ForEach or a Table below its threshold -- unchanged from before virtualization existed. A new
    // key outside the window (or when there is no window at all, above threshold with nothing reported yet, taken
    // as {0, overscan}; see the design's "bootstrap window") gets state only (`mountRowState`), left Dewindowed
    // for the windowing Effect to realize later if it ever enters the visible range.
    void reconcile(std::size_t depth, ForEachSession& session, std::vector<KeyedRow>& rows, ContainerWidget& container,
                   std::vector<Key> const& keys, RowHook const& onRow,
                   std::pair<VisibleRange, std::size_t> const* windowAndOverscan = nullptr) {
        std::vector<std::size_t> const order = firstOccurrences(keys);
        std::unordered_map<Key, std::size_t> wanted;          // key -> snapshot index
        std::unordered_map<Key, std::size_t> viewPositionOf;  // key -> position in view order (order's own index)
        for (std::size_t viewPos = 0; viewPos < order.size(); ++viewPos) {
            wanted.emplace(keys.at(order.at(viewPos)), order.at(viewPos));
            viewPositionOf.emplace(keys.at(order.at(viewPos)), viewPos);
        }

        // Unmount the gone keys, last first as KeyedRows does, then update the kept ones in place, which keeps
        // their widgets. A gone key's widget goes first (the Effect stops first within each row, mirroring the
        // order mountRow itself builds the two scopes in: state first, then widget), then its state.
        for (KeyedRow& row : std::views::reverse(rows)) {
            if (!wanted.contains(row.key)) {
                row.widgetScope.reset();
                row.stateScope.reset();
            }
        }
        std::erase_if(rows, [](KeyedRow const& row) { return row.stateScope == nullptr; });
        std::unordered_set<Key> kept;
        for (KeyedRow& row : rows) {
            kept.insert(row.key);
            row.slot->assign(wanted.at(row.key));
            row.viewPosition = viewPositionOf.at(row.key);
        }

        std::size_t const bootstrapFirst = 0;
        std::size_t const bootstrapCount = windowAndOverscan != nullptr ? windowAndOverscan->second : 0;
        VisibleRange const window = windowAndOverscan != nullptr && windowAndOverscan->first.count > 0
                                        ? windowAndOverscan->first
                                        : VisibleRange{.first = bootstrapFirst, .count = bootstrapCount};

        // Mount the new keys; each realized widget is appended to the container, and every new row (realized or
        // state-only) to `rows`. `viewPos` is the row's position in view order -- order's own index -- which is
        // what the window means positions in, not `index` (a position in the raw, possibly-reordered `keys`).
        std::exception_ptr failure;
        rows.reserve(order.size());
        for (std::size_t viewPos = 0; viewPos < order.size(); ++viewPos) {
            std::size_t const index = order.at(viewPos);
            if (kept.contains(keys.at(index))) {
                continue;
            }
            bool const inWindow =
                windowAndOverscan == nullptr || (viewPos >= window.first && viewPos < window.first + window.count);
            try {
                KeyedRow row = inWindow ? mountRow(depth, session, container, keys.at(index), index, onRow)
                                        : mountRowState(depth, session, keys.at(index), index);
                row.viewPosition = viewPos;
                rows.push_back(std::move(row));
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        }

        reorder(rows, container, keys, order, failure);
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    // Realizes a widget for every row within `window` (± `overscan`) that does not already have one, and releases
    // every realized widget outside it. A row's position in view order is its own `viewPosition`, set by the most
    // recent `reconcile` pass -- not its position in `rows` itself, which groups realized rows ahead of
    // dewindowed ones (so the container only ever holds the widgets that exist) and so no longer matches view
    // order once any row is dewindowed. Called only when `rows.size() >= threshold`, from the windowing Effect's
    // own Signal<VisibleRange>, never from reconcile, so a scroll alone costs O(rows) to scan, not the full
    // reconciliation pass.
    void applyWindow(std::size_t depth, std::vector<KeyedRow>& rows, ContainerWidget& container,
                     VisibleRange const& window, std::size_t overscan, RowHook const& onRow) {
        std::size_t const lo = window.first > overscan ? window.first - overscan : 0;
        std::size_t const hi =
            window.first + window.count + overscan;  // exclusive; saturates harmlessly past rows.size()
        std::exception_ptr failure;
        for (KeyedRow& row : rows) {
            bool const shouldBeRealized = row.viewPosition >= lo && row.viewPosition < hi;
            if (shouldBeRealized && row.widgetState != RowWidgetState::Realized &&
                row.widgetState != RowWidgetState::NullView) {
                try {
                    realizeRowWidget(depth, row, container, onRow);
                } catch (...) {
                    if (!failure) {
                        failure = std::current_exception();
                    }
                }
            } else if (!shouldBeRealized && row.widgetState == RowWidgetState::Realized) {
                dewindowRowWidget(row);
            }
        }
        // Realized widgets may now be a different subset of `rows` than before; bring the container back to the
        // rows' own view order (`viewPosition`), restricted to what is realized -- `reorder` needs `order` as
        // positions into `keys` in the target (view) order, not `rows`' own current array order, which groups
        // realized ahead of dewindowed and so is not view order once any row is dewindowed.
        std::vector<Key> const keys = [&] {
            std::vector<Key> out;
            out.reserve(rows.size());
            for (KeyedRow const& row : rows) {
                out.push_back(row.key);
            }
            return out;
        }();
        std::vector<std::size_t> order(rows.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::ranges::sort(order, {}, [&rows](std::size_t index) { return rows.at(index).viewPosition; });
        reorder(rows, container, keys, order, failure);
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    // The realized subset's extent in view order: the first realized row's view position and how many rows from
    // there are realized. Used only to tell the backend what to size a scrollbar against
    // (`TableWidget::setRealizedWindow`); a gap of dewindowed rows between two realized ones (the overscan margin
    // straddling a scroll position mid-transition) is not expected in steady state, so this reports the outermost
    // realized view positions, not a strict contiguous run.
    [[nodiscard]] static std::pair<std::size_t, std::size_t> realizedExtent(std::vector<KeyedRow> const& rows) {
        std::optional<std::size_t> first;
        std::size_t last = 0;
        for (KeyedRow const& row : rows) {
            if (row.widgetState == RowWidgetState::Realized) {
                first = first ? std::min(*first, row.viewPosition) : row.viewPosition;
                last = std::max(last, row.viewPosition);
            }
        }
        return first ? std::pair{*first, last - *first + 1} : std::pair{std::size_t{0}, std::size_t{0}};
    }

    Widget& mountKind(reactive::Scope& scope, Busy const& spec, ContainerWidget* parent) {
        BusyWidget& widget = scope.adopt(_backend->createBusy(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.active, [&widget](bool active) { widget.setActive(active); });
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Banner const& spec, ContainerWidget* parent) {
        BannerWidget& widget = scope.adopt(_backend->createBanner(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.tone, [&widget](Tone tone) { widget.setTone(tone); });
        if (hasSpans(spec.spans)) {
            bind(scope, spec.spans, [&widget](std::vector<TextSpan> const& spans) { widget.setSpans(spans); });
        } else {
            bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
        }
        bind(scope, spec.action.label, [&widget](std::string const& label) { widget.setActionLabel(label); });
        widget.setOnAction(event(spec.action.onClick));
        widget.setDismissible(spec.dismissible);
        widget.setOnDismiss(event(spec.onDismiss));
        if (spec.actionStyle != BannerActionStyle::Ghost || spec.actionStretch) {
            widget.setActionStyle(spec.actionStyle, spec.actionStretch);
        }
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Badge const& spec, ContainerWidget* parent) {
        BadgeWidget& widget = scope.adopt(_backend->createBadge(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.tone, [&widget](Tone tone) { widget.setTone(tone); });
        bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
        widget.setIcon(spec.icon);
        if (spec.style != BadgeStyle::Filled) {
            widget.setStyle(spec.style);
        }
        if (spec.shape != ControlShape::Default) {
            widget.setShape(spec.shape);
        }
        if (spec.mono) {
            widget.setMono(true);
        }
        if (spec.onRemove) {
            // An empty label names the control after the badge's text, which may be bound as well.
            auto const labelOf = [removeLabel = spec.removeLabel, text = spec.text] {
                std::string const label = removeLabel.evaluate();
                return label.empty() ? "Remove " + text.evaluate() : label;
            };
            Prop<std::string> const label = spec.removeLabel.isBound() || spec.text.isBound()
                                                ? Prop<std::string>(labelOf)
                                                : Prop<std::string>(labelOf());
            bind(scope, label, [&widget, onRemove = event(spec.onRemove)](std::string const& shown) {
                widget.setOnRemove(onRemove, shown);
            });
        }
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Progress const& spec, ContainerWidget* parent) {
        ProgressWidget& widget = scope.adopt(_backend->createProgress(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.value, [&widget](std::optional<double> value) { widget.setValue(value); });
        bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Steps const& spec, ContainerWidget* parent) {
        StepsWidget& widget = scope.adopt(_backend->createSteps(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.items, [&widget](std::vector<Step> const& steps) { widget.setSteps(steps); });
        bind(scope, spec.current, [&widget](std::optional<std::size_t> index) { widget.setCurrent(index); });
        if (spec.labelled.isBound() || !spec.labelled.constant()) {
            bind(scope, spec.labelled, [&widget](bool labelled) { widget.setLabelled(labelled); });
        }
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Chart const& spec, ContainerWidget* parent) {
        ChartWidget& widget = scope.adopt(_backend->createChart(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.data, [&widget](ChartData const& data) { widget.setData(data); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, KeyValue const& spec, ContainerWidget* parent) {
        KeyValueWidget& widget = scope.adopt(_backend->createKeyValue(parent));
        applyCommon(scope, widget, spec.common);
        if (spec.captions != CaptionPlacement::Beside) {
            widget.setCaptionPlacement(spec.captions);
        }
        if (spec.captionWidth) {
            if (*spec.captionWidth < 1) {
                throw std::invalid_argument{"morph::ui::mount: `captionWidth` is at least 1 unit"};
            }
            if (spec.captions == CaptionPlacement::Above) {
                throw std::invalid_argument{"morph::ui::mount: `captionWidth` needs the captions beside their values"};
            }
            widget.setCaptionWidth(*spec.captionWidth);
        }
        bind(scope, spec.items, [&widget](std::vector<KeyValueItem> const& items) { widget.setItems(items); });
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, EmptyState const& spec, ContainerWidget* parent) {
        EmptyStateWidget& widget = scope.adopt(_backend->createEmptyState(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
        widget.setIcon(spec.icon);
        bind(scope, spec.action.label, [&widget](std::string const& label) { widget.setActionLabel(label); });
        widget.setOnAction(event(spec.action.onClick));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Splitter const& spec, ContainerWidget* parent) {
        SplitterWidget& widget = scope.adopt(_backend->createSplitter(parent, spec.orientation));
        if (spec.orientation == Axis::Horizontal) {
            scope.make<ShareParent>(_shareParents, &widget);
        }
        applyCommon(scope, widget, spec.common);
        mountChildren(scope, spec.children, widget);
        widget.setOnResize(inputEvent(
            spec.onResize,
            controlled(scope, spec.sizes, [&widget](std::vector<int> const& sizes) { widget.setSizes(sizes); })));
        return widget;
    }

    // The header is mounted first, so it is the first child; the content follows it.
    Widget& mountKind(reactive::Scope& scope, Collapsible const& spec, ContainerWidget* parent) {
        CollapsibleWidget& widget = scope.adopt(_backend->createCollapsible(parent));
        applyCommon(scope, widget, spec.common);
        bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
        if (Widget* const header = mount(scope, spec.header, &widget); header != nullptr) {
            widget.setHeader(*header);
        }
        static_cast<void>(mount(scope, spec.child, &widget));
        widget.setOnToggle(
            inputEvent(spec.onToggle, controlled(scope, spec.open, [&widget](bool open) { widget.setOpen(open); })));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, DropZone const& spec, ContainerWidget* parent) {
        DropZoneWidget& widget = scope.adopt(_backend->createDropZone(parent));
        applyCommon(scope, widget, spec.common);
        widget.setAccept(spec.accept);
        widget.setMultiple(spec.multiple);
        widget.setOnDrop(event(spec.onDrop));
        static_cast<void>(mount(scope, spec.child, &widget));
        return widget;
    }

    // The refusal text of a page node, naming its test id the way every layout refusal does.
    [[nodiscard]] static std::logic_error pageRefusal(Common const& common, std::string const& what) {
        return std::logic_error{"morph::ui::mount: " + what +
                                (common.testId.empty() ? std::string{} : " (node `" + common.testId + "`)")};
    }

    // A page sizes itself: it takes `alignSelf` (not `Stretch`) and no other layout hint. A region is sized by its
    // geometry and takes none.
    static void checkPageLayout(Common const& common, bool isPage) {
        LayoutHints hints = common.layout;
        if (isPage) {
            if (hints.alignSelf && *hints.alignSelf == Align::Stretch) {
                throw pageRefusal(common, "a page is never stretched; `alignSelf` places the sheet");
            }
            hints.alignSelf.reset();
        }
        if (hints != LayoutHints{}) {
            throw pageRefusal(common, isPage ? "a page is sized by its sheet; its layout hints are refused"
                                             : "a page region is sized by its geometry; its layout hints are refused");
        }
    }

    // The geometry of the page whose regions are being mounted, restored when they are done.
    class PageGuard {
    public:
        PageGuard(Mounter& self, PageGeometry const* page) : _self{&self}, _saved{std::exchange(self._page, page)} {}
        PageGuard(PageGuard const&) = delete;
        PageGuard& operator=(PageGuard const&) = delete;
        PageGuard(PageGuard&&) = delete;
        PageGuard& operator=(PageGuard&&) = delete;
        ~PageGuard() { _self->_page = _saved; }

    private:
        Mounter* _self;
        PageGeometry const* _saved;
    };

    Widget& mountKind(reactive::Scope& scope, Page const& spec, ContainerWidget* parent) {
        checkPageLayout(spec.common, true);
        if (std::optional<std::string> const problem = spec.geometry.problem()) {
            throw pageRefusal(spec.common, *problem);
        }
        for (Node const& child : spec.children) {
            if (child && !std::holds_alternative<PageRegion>(child->kind)) {
                throw pageRefusal(spec.common, "a page holds page regions and nothing else");
            }
        }
        PageWidget& widget = scope.adopt(_backend->createPage(parent, spec.geometry));
        applyCommon(scope, widget, spec.common);
        if (spec.common.a11y.role.empty()) {
            widget.setAccessibleRole("document");
        }
        if (!spec.common.surface.isBound() && spec.common.surface.constant().empty()) {
            widget.setSurface("paper");
        }
        PageGuard const guard{*this, &spec.geometry};
        mountChildren(scope, spec.children, widget);
        return widget;
    }

    // An accented cell anywhere but a source table's cell: its content, with the accent not read.
    Widget& mountKind(reactive::Scope& scope, AccentedCell const& spec, ContainerWidget* parent) {
        return *mount(scope, spec.content, parent);
    }

    // A marked cell anywhere but a source table's cell: its content, with the marks not read.
    Widget& mountKind(reactive::Scope& scope, MarkedCell const& spec, ContainerWidget* parent) {
        return *mount(scope, spec.content, parent);
    }

    Widget& mountKind(reactive::Scope& scope, PageRegion const& spec, ContainerWidget* parent) {
        auto* const page = dynamic_cast<PageWidget*>(parent);
        if (page == nullptr || _page == nullptr) {
            throw pageRefusal(spec.common, "a page region belongs in a page");
        }
        checkPageLayout(spec.common, false);
        if (std::optional<std::string> const problem = spec.geometry.problem(*_page)) {
            throw pageRefusal(spec.common, *problem);
        }
        PageRegionWidget& widget = scope.adopt(_backend->createPageRegion(page, spec.geometry));
        applyCommon(scope, widget, spec.common);
        mountChildren(scope, spec.children, widget);
        return widget;
    }

    // One prop of a loaded component, bound to the setter its kind crosses by.
    void bindCustomProp(reactive::Scope& scope, CustomWidget& widget, CustomProp const& prop) {
        std::string const& name = prop.name;
        if (auto const* const text = std::get_if<Prop<std::string>>(&prop.value)) {
            bind(scope, *text, [&widget, name](std::string const& value) { widget.setTextProp(name, value); });
        } else if (auto const* const flag = std::get_if<Prop<bool>>(&prop.value)) {
            bind(scope, *flag, [&widget, name](bool value) { widget.setBoolProp(name, value); });
        } else if (auto const* const number = std::get_if<Prop<double>>(&prop.value)) {
            bind(scope, *number, [&widget, name](double value) { widget.setNumberProp(name, value); });
        }
    }

    // The component the backend loads in the custom node's place, or the fallback when it loads none.
    // An ActionBar holds the actions of what holds it: the footer of the overlay whose content it is in, else the
    // place it is written. It is a horizontal stack either way, marked as a row of actions so the backend spaces its
    // children by the theme's action gap.
    Widget& mountKind(reactive::Scope& scope, ActionBar const& spec, ContainerWidget* parent) {
        ContainerWidget* const footer = overlayFooter();
        if (spec.wrap && footer != nullptr) {
            throw std::invalid_argument{
                "morph::ui::Mounted: an overlay's footer is one row of actions; `wrap` is for an action bar in place"};
        }
        StackWidget& widget =
            scope.adopt(_backend->createStack(footer != nullptr ? footer : parent, Axis::Horizontal));
        // A wrapping row shares no width with its parent, as a wrapping ForEach does.
        if (footer == nullptr && !spec.wrap) {
            scope.make<ShareParent>(_shareParents, &widget);
        }
        widget.setActionRow();
        if (spec.wrap) {
            widget.setWrap(true);
        }
        applyCommon(scope, widget, spec.common);
        widget.setGap(spacingStep(2));
        if (spec.align != Align::Stretch) {
            widget.setAlign(spec.align);
        }
        if (spec.justify != Justify::Start) {
            widget.setJustify(spec.justify);
        }
        // While the children mount, a horizontal ForEach whose parent is this bar is part of its row.
        class Restore {
        public:
            Restore(Mounter& self, StackWidget* bar) : _self{&self}, _saved{std::exchange(self._actionBar, bar)} {}
            Restore(Restore const&) = delete;
            Restore& operator=(Restore const&) = delete;
            Restore(Restore&&) = delete;
            Restore& operator=(Restore&&) = delete;
            ~Restore() { _self->_actionBar = _saved; }

        private:
            Mounter* _self;
            StackWidget* _saved;
        };
        Restore const restore{*this, &widget};
        for (Node const& child : spec.children) {
            static_cast<void>(mount(scope, child, &widget));
        }
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Custom const& spec, ContainerWidget* parent) {
        CustomRequest request{.name = spec.name, .props = {}, .minVersion = spec.minVersion};
        request.props.reserve(spec.props.size());
        for (CustomProp const& prop : spec.props) {
            request.props.push_back(prop.name);
        }
        if (std::unique_ptr<CustomWidget> loaded = _backend->createCustom(parent, request)) {
            CustomWidget& widget = scope.adopt(std::move(loaded));
            for (CustomProp const& prop : spec.props) {
                bindCustomProp(scope, widget, prop);
            }
            widget.setOnEvent(spec.onEvent);
            return widget;
        }
        Widget* const fallback = mount(scope, spec.fallback, parent);
        if (fallback == nullptr) {
            throw std::invalid_argument{"morph::ui::Mounted: the custom node " + spec.name + " has no fallback"};
        }
        return *fallback;
    }

    Widget& mountKind(reactive::Scope& scope, DateTimeInput const& spec, ContainerWidget* parent) {
        DateTimeInputWidget& widget =
            scope.adopt(_backend->createDateTimeInput(parent, spec.mode, spec.offsetMinutes));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        // Before the value, which is then shown in it; an input with no pattern is told none.
        if (spec.pattern.isBound() || !spec.pattern.constant().empty()) {
            bind(scope, spec.pattern, [&widget](std::string const& pattern) { widget.setPattern(pattern); });
        }
        widget.setOnChange(inputEvent(
            spec.onChange,
            controlled(scope, spec.value,
                       [&widget](std::optional<morph::time::Timestamp> const& value) { widget.setValue(value); })));
        applyCaption(scope, widget, spec.label, spec.showLabel);
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, Slider const& spec, ContainerWidget* parent) {
        SliderWidget& widget = scope.adopt(_backend->createSlider(parent));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        widget.setRange(spec.minimum, spec.maximum, spec.step);
        widget.setOnChange(inputEvent(
            spec.onChange, controlled(scope, spec.value, [&widget](std::int64_t value) { widget.setValue(value); })));
        widget.setOnCommit(event(spec.onCommit));
        return widget;
    }

    Widget& mountKind(reactive::Scope& scope, FilePicker const& spec, ContainerWidget* parent) {
        FilePickerWidget& widget = scope.adopt(_backend->createFilePicker(parent, spec.mode));
        applyCommon(scope, widget, spec.common);
        applyField(scope, widget, spec.field);
        widget.setOnPicked(inputEvent(spec.onPicked, controlled(scope, spec.path, [&widget](std::string const& path) {
                                          widget.setPath(path);
                                      })));
        return widget;
    }

    reactive::Runtime* _rt;
    IViewBackend* _backend;
    // The dialog or drawer whose content is being mounted, while it is: an ActionBar mounts into its footer. A binding
    // that mounts later (a Switch case, a Tabs page, a row) mounts under the overlay it was made under. The footer is
    // asked for only when an ActionBar mounts, so an overlay without actions never makes one.
    ContainerWidget* _overlay = nullptr;
    // The action bar whose children are being mounted, while they are.
    StackWidget* _actionBar = nullptr;
    // The sheet whose regions are being mounted, while they are.
    PageGeometry const* _page = nullptr;
    // The first autofocus widget of the mount pass in progress, and how deeply passes are nested.
    Widget* _autofocus = nullptr;
    std::size_t _passDepth = 0;
    // The mounted widgets with a focus name. A multimap keeps equal names in insertion order, so the first mounted
    // is found first.
    FocusMap _focusable;
    // The focus chain's tree: every widget a node mounted, by its container, and the roots in the order mounted.
    std::unordered_map<Widget const*, ChainEntry> _chain;
    std::vector<Widget*> _chainRoots;
    std::unordered_map<Widget const*, std::pair<bool, bool>> _chainFlags;
    // The rows of source tables: what is mounted in them is not on the chain.
    std::unordered_set<Widget const*> _offChain;
    // The mounted containers that take a share of their width for a child's.
    std::set<ContainerWidget const*> _shareParents;
    // What the mount did to the rows of its source tables.
    MountStats _stats;
    // The column extents of the source table mounted last, for tests.
    std::weak_ptr<ColumnExtents const> _extents;
};

}  // namespace detail

/// @brief A view tree mounted on a backend: its widgets, built once, and the bindings that keep them current.
///
/// Destroying it destroys every binding before the widget it drives and every child before its parent. The
/// runtime, the backend and every signal a binding reads must outlive it; the owner of the runtime constructs and
/// destroys it. Non-copyable and non-movable: bindings point into it.
///
/// Every widget callback, a drop's `accepts` predicate included, runs as a `reactive::Runtime::widgetEvent` and
/// untracked, so a binding whose setter makes the backend call a handler does not subscribe to what the handler
/// reads. An exception a callback throws is reported (`detail::site::kCallbackThrew`) and goes no further: the
/// backend's call returns normally, a predicate that threw refuses the drop, and what the callback wrote before
/// throwing still flushes.
///
/// An input whose value is a slot is controlled: a text input's text, a checkbox's state, a select's key, a slider's,
/// date-time input's or file picker's value, a collapsible panel's state, a Tabs index, a Dialog's `open` and a
/// Table's selection. After a user's request, and after the flush it caused, a posted turn shows the slot's value if
/// the widget shows anything else: a refused request snaps back, an accepted one costs no setter call, and a
/// transformed one shows the request for that one turn. That holds with no handler too, which makes a slot without one
/// read-only. A constant is not a slot: the widget keeps what the user did to it.
///
/// A mount that throws goes no further than the content it was building, and leaves none of it behind. A node mounted
/// directly, a constant Switch selector, Tabs index or Dialog `open` included, throws out of the constructor. Content
/// a binding mounts, which a bound selector, index or `open` does in an Effect even during the constructor, is
/// reported (`reactive::detail::site::kEffectThrew`) instead: a Switch shows nothing for that key, having torn the old
/// case down first; a Tabs keeps the previous page shown and selected; a Dialog stays closed. A Tabs then calls its
/// `onSelect` with the page still shown, and a Dialog its `onDismiss`, each as a widget callback and as the last thing
/// the mount does, so the application's state follows the widget and its next pick of the failed tab, or opening of
/// the dialog, mounts again. A Switch selector is equality-gated, so its next change of key mounts again. A Tabs
/// index and a Dialog `open` are not: while one still names content that failed — no `onSelect` or `onDismiss` moved
/// it — every run of its binding, a change to any signal the binding reads included, tries that mount again, once
/// per run, reporting each failure and calling `onSelect` or `onDismiss` again.
///
/// A ForEach or a Table mounts its rows in an Effect too, one row at a time: a row that fails is reported, leaves
/// nothing behind and is left out, the other rows are mounted and ordered all the same, and the next change of the
/// rows mounts it again. A key repeated in one snapshot is reported (`detail::site::kDuplicateKey`), and only its
/// first row is shown.
///
/// Bindings run in owner order. The tree's own bindings are at the depth the mount was given; the content of a Switch
/// case, a Tabs page or a Dialog, and every ForEach or Table row, is in a scope one level deeper than the binding that
/// mounted it. So an Effect that removes content runs before the content's own bindings in the same flush, and a
/// binding never evaluates against state its owner is about to take away. A mount placed under an outer scope (a
/// screen's, a row's) takes that scope's depth plus one, so the outer scope's Effects run first too.
///
/// A mount must run to completion: nothing it calls, a backend factory or setter or a binding's evaluation, may
/// destroy this object, its runtime or its backend. Widget callbacks are events, not part of a mount, and may.
class Mounted {
public:
    /// @param runtime The runtime the bindings are made in. Borrowed.
    /// @param backend Makes the widgets. Borrowed.
    /// @param root The tree; kept alive as long as the mount.
    /// @param parent The container the root widget is appended to, or null for a backend root.
    /// @param depth The scope depth of the tree's own bindings (`reactive::Scope::depth`): zero for a mount no other
    ///        scope owns, one more than the owner's for a mount placed under a scope.
    /// @throws std::invalid_argument when @p root is null.
    Mounted(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND, Node root,
            ContainerWidget* parent = nullptr, std::size_t depth = 0)
        : _root{nonNull(std::move(root))},
          _mounter{runtime, backend},
          _scope{runtime, depth},
          // Untracked, so mounting from inside an Effect does not subscribe that Effect to anything read here.
          _widget{runtime.untracked([this, parent] { return _mounter.mountRoot(_scope, _root, parent); })} {}

    ~Mounted() = default;
    Mounted(Mounted const&) = delete;
    Mounted& operator=(Mounted const&) = delete;
    Mounted(Mounted&&) = delete;
    Mounted& operator=(Mounted&&) = delete;

    /// @brief The root node's widget.
    /// @return The widget; valid as long as this object.
    [[nodiscard]] Widget& root() const noexcept { return *_widget; }

    /// @brief Moves keyboard focus to the mounted widget whose `Common::focusId` is @p focusId, at any depth,
    ///        including the content of a Switch case, a Tabs page, a Dialog and every row mounted now.
    ///
    /// Of several such widgets, the one mounted first is taken; one whose content has since been unmounted is not
    /// found.
    /// @param focusId The name.
    /// @return Whether a mounted widget has that name.
    bool focus(std::string_view focusId) { return _mounter.focus(focusId); }

    /// @brief Moves the text cursor of the mounted text input whose `Common::focusId` is @p focusId, at any depth, to
    ///        @p offset, focuses it and asks it to reveal the cursor's line: how a source view puts the cursor where a
    ///        refusal points.
    ///
    /// Of several such widgets, the one mounted first is taken. An offset inside a UTF-8 sequence moves back to the
    /// start of that code point.
    /// @param focusId The name.
    /// @param offset  A byte offset into the text it shows.
    /// @return Whether a mounted text input has that name.
    bool placeCursor(std::string_view focusId, std::size_t offset) { return _mounter.placeCursor(focusId, offset); }

    /// @brief Moves keyboard focus to the control after the mounted widget whose `Common::focusId` is @p focusId on
    ///        the focus chain: the mounted widgets that take focus (a text input, a button, a check box, a select, a
    ///        menu, a date-time input, a slider, a file picker, a table, a tabs, a badge with `onRemove`), shown and
    ///        enabled with every container around them, in document order.
    /// @param focusId The name.
    /// @return Whether a control took focus: `false` when no widget has the name, or none comes after it.
    bool focusNext(std::string_view focusId) { return _mounter.focusAlong(focusId, true); }

    /// @brief Moves keyboard focus to the control before the mounted widget whose `Common::focusId` is @p focusId on
    ///        the focus chain (`focusNext`).
    /// @param focusId The name.
    /// @return Whether a control took focus: `false` when no widget has the name, or none comes before it.
    bool focusPrevious(std::string_view focusId) { return _mounter.focusAlong(focusId, false); }

    /// @brief What the mount did to the rows of its tables mounted over a `TableSource`, the whole mount's.
    /// @return The counters; valid as long as this object.
    [[nodiscard]] MountStats const& stats() const noexcept { return _mounter.stats(); }

    /// @brief The column extents of the table over a `TableSource` mounted last, for tests.
    /// @return They, or null when no such table is mounted.
    [[nodiscard]] ColumnExtents const* columnExtents() const noexcept { return _mounter.columnExtents(); }

private:
    [[nodiscard]] static Node nonNull(Node root) {
        if (!root) {
            throw std::invalid_argument{"morph::ui::Mounted: the root node is null"};
        }
        return root;
    }

    Node _root;
    detail::Mounter _mounter;
    reactive::Scope _scope;  // after _mounter: destroyed first, while the mounter its bindings call is alive
    Widget* _widget;
};

}  // namespace morph::ui
