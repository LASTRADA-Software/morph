// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <morph/export.hpp>
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

#include "table_rows.hpp"

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
[[nodiscard]] MORPH_API Windowing windowingOf(Virtualization const& virtualization, std::size_t rows,
                                              std::size_t visibleColumns, bool serverMode);

/// @brief The widths of a table's visible columns, in layout order, and their running offsets.
///
/// Widths are kept in a Fenwick tree, so a width's change, a column's offset and the columns at a scroll position
/// each read O(log n) nodes; no call reads every column. `columnReads` counts the reads of single columns' widths
/// (`widthOf`), which is what a pass over the window does per column.
class MORPH_API ColumnExtents {
public:
    /// @brief Replaces every width.
    /// @param widths The visible columns' widths, in layout order; a negative width reads as 0.
    void assign(std::span<int const> widths);

    /// @brief Grows a column to @p width; a measured width never narrows a column, nor changes one the user set.
    /// @param column The column's place in the layout.
    /// @param width  The width measured.
    /// @return Whether the width changed.
    bool grow(std::size_t column, int width);

    /// @brief Sets a width the mount computed, which may be narrower than the one before; a width the user set stays.
    /// @param column The column's place in the layout.
    /// @param width  The width; a negative width reads as 0.
    /// @return Whether the width changed.
    bool fit(std::size_t column, int width);

    /// @brief Sets a width the user chose; measurement no longer changes it.
    /// @param column The column's place in the layout.
    /// @param width  The width.
    void set(std::size_t column, int width);

    /// @brief The columns a horizontal viewport shows.
    /// @param offset   The scroll offset.
    /// @param viewport The viewport's width.
    /// @return `[first, last)`: the column under @p offset to the one past the last under the viewport's end.
    [[nodiscard]] std::pair<std::size_t, std::size_t> columnsAt(int offset, int viewport) const;

    /// @brief Where a column starts: the sum of the widths before it.
    /// @param column The column's place in the layout; past the last, the total width.
    /// @return The offset.
    [[nodiscard]] int offsetOf(std::size_t column) const;

    /// @brief A column's width; one counted read.
    /// @param column The column's place in the layout.
    /// @return The width; 0 past the last column.
    [[nodiscard]] int widthOf(std::size_t column) const;

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
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`column`, `delta`): called from `assign`, `grow`, `fit` and `set`, each passing the column first and the signed width change second
    void add(std::size_t column, int delta);

    // How many leading columns end at or before @p offset: the column under @p offset, by descent on the tree.
    [[nodiscard]] std::size_t below(int offset) const;

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
[[nodiscard]] MORPH_API ColumnWindow columnWindow(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`pinned`, `scrollX`): a column count then a scroll offset, passed by name from the table state at the one call site
    ColumnExtents const& extents, std::size_t pinned, int scrollX,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`viewport`, `overscan`): a width then a column count, passed by name from the table state at the one call site
    int viewport, std::size_t overscan);

namespace detail {

/// @brief Appends the runs of @p bands, at @p level, and of their children one level down: one run per maximal run of
///        a band's columns that stand together in @p visible, split at the pinned edge.
/// @param bands   The bands of one level.
/// @param visible The visible column ids, in layout order.
/// @param pinned  How many of the leading visible columns are pinned.
/// @param level   The level of @p bands, 0 for the outermost.
/// @param out     The runs found so far; the new ones are appended.
// NOLINTNEXTLINE(misc-no-recursion): a band's children are bands one level down.
MORPH_API void appendBandRuns(std::span<table::BandInfo const> bands, std::span<std::string const> visible,
                              std::size_t pinned, std::size_t level, std::vector<BandRun>& out);

/// @brief Appends the bands that enclose a column, outermost first: the band of @p bands that names it, then the one
///        of that band's children that does, and so on.
/// @param bands    The bands of one level.
/// @param columnId The column's id.
/// @param out      The bands found so far; the enclosing ones are appended.
// NOLINTNEXTLINE(misc-no-recursion): a band's children are bands one level down.
MORPH_API void enclosingBands(std::span<table::BandInfo const> bands, std::string_view columnId,
                              std::vector<table::BandInfo const*>& out);

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
[[nodiscard]] MORPH_API std::vector<BandRun> bandRuns(std::span<table::BandInfo const> bands,
                                                      std::span<std::string const> visible, std::size_t pinned);

/// @brief Whether a column's header shows its unit under the bands: not when a band enclosing it shows the same one.
/// @param bands    The bands, outermost first.
/// @param columnId The column's id.
/// @param unit     The column's unit.
/// @return `false` when @p unit is not empty and an enclosing band's unit equals it; `true` otherwise.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`columnId`, `unit`): a column id then its unit id, both read from the same TableColumn at every call
[[nodiscard]] MORPH_API bool columnUnitShown(std::span<table::BandInfo const> bands, std::string_view columnId,
                                             std::string_view unit);

/// @brief A column header's accessible name under the bands: each enclosing band's label, outermost first, then the
///        column's, joined by `", "` ("Mass retained, sieve 4 mm").
/// @param bands    The bands, outermost first.
/// @param columnId The column's id.
/// @param label    The column's own name.
/// @return The name; @p label alone for a column under no band.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`columnId`, `unit`): a column id then its unit id, both read from the same TableColumn at every call
[[nodiscard]] MORPH_API std::string bandedHeaderName(std::span<table::BandInfo const> bands, std::string_view columnId,
                                                     std::string_view label);

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
[[nodiscard]] MORPH_API std::size_t cutColumns(CellSpan const& span, std::span<std::string const> declared,
                                               std::span<std::string const> visible);

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
[[nodiscard]] MORPH_API ResolvedSpans resolveRowSpans(std::vector<SpanRow> const& rows, std::size_t columns,
                                                      std::span<std::optional<std::size_t> const> placeOf = {});

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
struct MORPH_API KeyedRows {
    KeyedRows() = default;
    /// @brief Unmounts the rows last first, as a container's children are; a vector's own destruction order differs
    ///        between standard libraries.
    ~KeyedRows();
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
[[nodiscard]] MORPH_API std::vector<bool> stableTargets(std::vector<std::size_t> const& sequence);

/// @brief Brings a container's children from their current order into the target order with `moveChild`,
///        moving only the children outside a longest run already in order.
/// @param container The container.
/// @param current Its children, in their current order. Kept equal to the container's order after every move, so
///        it is still that order when a move throws.
/// @param target The same children, in the order wanted.
MORPH_API void reorderChildren(ContainerWidget& container, std::vector<Widget*>& current,
                               std::vector<Widget*> const& target);

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
class MORPH_API Mounter {
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
    Widget* mount(reactive::Scope& scope, Node const& node, ContainerWidget* parent);

    /// @brief Moves keyboard focus along the focus chain from the mounted widget whose `focusId` is @p focusId.
    /// @param focusId The name.
    /// @param forward Whether to the next control, or the previous one.
    /// @return Whether a control took focus.
    bool focusAlong(std::string_view focusId, bool forward);

    /// @brief Moves keyboard focus along the focus chain from @p from: the mounted widgets that take focus (a text
    ///        input, a button, a check box, a select, a menu, a date-time input, a slider, a file picker, a table, a
    ///        tabs, a badge that can be removed), shown and enabled with every container around them, in document
    ///        order, walked when asked.
    /// @param from    The widget focus leaves.
    /// @param forward Whether to the next control, or the previous one.
    /// @return Whether a control took focus; `false` when none lies that way.
    bool focusAlong(Widget const& from, bool forward);

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
    bool focus(std::string_view focusId);

    /// @brief Moves the text cursor of the mounted text input whose `focusId` is @p focusId to @p offset, focuses it
    ///        and asks it to reveal the cursor's line.
    ///
    /// Of several such widgets, the one mounted first is taken. An offset inside a UTF-8 sequence moves back to the
    /// start of that code point.
    /// @param focusId The name.
    /// @param offset  A byte offset into the text it shows.
    /// @return Whether a mounted text input has that name.
    bool placeCursor(std::string_view focusId, std::size_t offset);

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
    [[nodiscard]] ContainerWidget* overlayFooter() const;

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
    static void checkLayout(Common const& common);

    // The loader's rule for `percent`, for a view tree built in C++: a share of its container's width is a width along
    // a row that does not wrap, a horizontal `forEach` that does not wrap, or a horizontal splitter's pane.
    void checkShare(Common const& common, ContainerWidget const* parent) const;

    // A grid track's or a table column's width is no share.
    static void checkNoShare(std::span<GridTrack const> tracks);
    static void checkNoShare(std::span<TableColumn const> columns);

    // A dialog or a drawer is sized by the frontend, not by its container: Stretch sizing on one would silently
    // change how every *other* sibling in its own parent layout is justified. Covers both the document path's own
    // computed (non-literal) `sizing` -- resolved to a Sizing only here, at mount time, since the document-level
    // checkSizing validates only a literal one -- and a C++ author calling ui::dialog()/ui::drawer() directly, both
    // of which reach this same mountKind overload before either backend ever sees the result.
    static void checkOverlayLayout(Common const& common, std::string_view kind);

    // The loader's rule for `alignSelf`, for a view tree built in C++: only a stack's or a grid's child has a cross
    // axis to be placed on.
    static void checkPlacement(Common const& common, ContainerWidget const* parent);

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): applies each common property (visible, enabled, tooltip, keys and so on) to the widget; each binds against the same scope
    void applyCommon(reactive::Scope& scope, Widget& widget, Common const& common);

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
    struct MORPH_API ChainFlags {
        ChainFlags(std::unordered_map<Widget const*, std::pair<bool, bool>>& owner, Widget const* widget)
            : flags{&owner}, of{widget} {
            flags->insert_or_assign(of, std::pair{true, true});
        }
        ChainFlags(ChainFlags const&) = delete;
        ChainFlags& operator=(ChainFlags const&) = delete;
        ChainFlags(ChainFlags&&) = delete;
        ChainFlags& operator=(ChainFlags&&) = delete;
        ~ChainFlags() { flags->erase(of); }

        void set(bool visible, bool value) const;

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
    void link(reactive::Scope& scope, Widget& widget, ContainerWidget const* parent, bool focusable);

    void unlink(Widget* widget) noexcept;

    // Brings a container's children in the chain's tree to the order the mount moved them to.
    void reorderChain(Widget const& container, std::vector<Widget*> const& order);

    // The widgets that take focus under @p widget, in document order, each with whether it is shown and enabled with
    // every container around it.
    // NOLINTNEXTLINE(misc-no-recursion): a container's children are walked in turn.
    void chainOrder(Widget& widget, bool shown, std::vector<std::pair<Widget*, bool>>& out) const;

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
    struct MORPH_API Focusable {
        Focusable(FocusMap& owner, std::optional<FocusMap::iterator> added) noexcept : map{&owner}, entry{added} {}
        Focusable(Focusable const&) = delete;
        Focusable& operator=(Focusable const&) = delete;
        Focusable(Focusable&&) = delete;
        Focusable& operator=(Focusable&&) = delete;
        ~Focusable() { reset(std::nullopt); }

        // Forgets the entry held and holds @p next instead.
        void reset(std::optional<FocusMap::iterator> next) noexcept;

        FocusMap* map;
        std::optional<FocusMap::iterator> entry;
    };

    // No predicate accepts every key; a predicate that throws refuses the drop.
    void applyDrop(Widget& widget, Common const& common);

    // One setKeys for all the chords; a chord listed twice runs its first binding, as a widget callback.
    void applyKeys(Widget& widget, std::vector<KeyBinding> const& keys);

    void mountChildren(reactive::Scope& scope, std::vector<Node> const& children, ContainerWidget& container);

    // The alignment only when it is not the default, as for a button's emphasis, so a stack that says nothing more
    // costs no call.
    Widget& mountStack(reactive::Scope& scope, Axis axis, std::vector<Node> const& children, int gap,
                       Common const& common, ContainerWidget* parent, Align align = Align::Stretch,
                       Justify justify = Justify::Start, bool wrap = false, bool divided = false, Padding padding = {},
                       std::optional<Prop<bool>> const& selected = std::nullopt, bool holder = false);

    // Whether a node's `visible` is false as it mounts.
    static bool startsHidden(Common const& common) {
        return common.visible.isBound() ? !common.visible.evaluate() : !common.visible.constant();
    }

    // The children of a stack that mounts hidden: its own widget exists, and they mount the first time `visible`
    // becomes true, into a scope of their own one level deeper than the stack's. They stay mounted after that, and
    // hiding the stack again only hides it. A `visible` that is a constant `false` never shows, so they never mount.
    void mountWhenShown(reactive::Scope& scope, std::vector<Node> const& children, ContainerWidget& container,
                        Prop<bool> const& visible);

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
                                                           std::map<MenuPath, MenuPath>& sources);

    // The visible entries as the widget shows them; called inside the menu's one binding.
    [[nodiscard]] static ShownMenu shownOf(std::vector<MenuItem> const& items);

    // Whether any entry, at any depth, has a bound label, description, check mark, enablement or visibility.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static bool anyBound(std::vector<MenuItem> const& items) {
        return std::ranges::any_of(items, entryBound);
    }

    // Whether this entry, or one in its submenu, has a bound label, description, check mark, enablement or
    // visibility.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static bool entryBound(MenuItem const& item);

    // The item a path through the shown entries names, or null when it names none or a separator.
    [[nodiscard]] static MenuItem const* itemAt(std::vector<MenuItem> const& items,
                                                std::map<MenuPath, MenuPath> const& sources, MenuPath const& path);

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
    void applyField(reactive::Scope& scope, FieldWidget& widget, FieldState const& field);

    Widget& mountKind(reactive::Scope& scope, Text const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Button const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, TextInput const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Checkbox const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Select const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Menu const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Column const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Row const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Grid const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Spacer const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Panel const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Scroll const& spec, ContainerWidget* parent);

    // Content a binding mounts (a Switch case, Dialog content), all or nothing: it is built in a scope of its own,
    // which the caller takes only once the mount completed, so content that throws partway leaves nothing behind.
    // Built untracked, so the binding's Effect depends on its own prop alone. The scope is one level deeper than the
    // one that owns the binding, so the owner's Effects run before the content's.
    [[nodiscard]] std::unique_ptr<reactive::Scope> mountContent(std::size_t depth, Node const& node,
                                                                ContainerWidget& host);

    // reset() tears the old case down, newest first, before the new one is built. The Computed behind a bound
    // selector is equality-gated, so an unchanged key never gets here. A case that fails to mount leaves nothing for
    // its key; the next change of the key mounts again.
    Widget& mountKind(reactive::Scope& scope, Switch const& spec, ContainerWidget* parent);

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
    Widget& mountKind(reactive::Scope& scope, Tabs const& spec, ContainerWidget* parent);

    // A page is mounted the first time its tab is selected and only hidden afterwards, so its widgets keep their
    // state (focus, scroll position, half-typed text) across tab switches. A new page is complete before anything
    // else changes: one that fails to mount leaves the previous page shown and selected, and the bar's highlight,
    // which the user's pick may already have moved, goes back to it. The next change of the index mounts again.
    //
    // Only the mount runs under the tabs' overlay, and that overlay is put back before a failure is reported:
    // reporting it may destroy the Mounted, and with it this Mounter.
    void showPage(TabsWidget& widget, reactive::Scope& pageScope, Pages& pages, std::vector<Tab> const& pageSpecs,
                  std::size_t index, ContainerWidget* overlay);

    // Moves the bar's highlight, and records it.
    static void highlight(TabsWidget& widget, Pages& pages, std::size_t index);

    // After a page failed to mount: puts the bar's highlight back on the page still shown, and tells the application
    // through `onSelect`, so its index matches the widget and selecting the failed tab again is a change that mounts
    // again.
    //
    // onSelect runs last, from a copy: it is a widget callback, which may destroy the Mounted and with it `pages`,
    // the widget and the Effect that called this. Nothing after it touches any of them; the caller only rethrows.
    //
    // Nothing here may replace the failure being reported: the callback's wrapper reports what the callback throws,
    // and anything else thrown here is dropped.
    static void restoreSelection(TabsWidget& widget, Pages& pages, std::size_t shown) noexcept;

    // The page is built in a scope of its own and handed to `pageScope` once complete, so one that fails to mount
    // leaves nothing behind.
    void mountPage(TabsWidget& widget, reactive::Scope& pageScope, Pages& pages, Node const& node, std::size_t index);

    // The content is built before the overlay opens and torn down before it closes. Content that fails to mount
    // under a bound `open` leaves the dialog closed and calls `onDismiss`, so the application's `open` follows the
    // widget and opening it again is a change that mounts again.
    //
    // A bound `open` is not equality-gated, for the reason a Tabs index is not: onDismiss writes `open` inside this
    // Effect's own run, which a Computed would miss, and then find the next opening unchanged. An Effect that runs
    // with `open` unchanged re-sends `setOpen`, and mounts content only if none is mounted: once per run while `open`
    // stays true over content that failed.
    Widget& mountKind(reactive::Scope& scope, Dialog const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Drawer const& spec, ContainerWidget* parent);

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
    static void dismissFailed(std::function<void()> const& onDismiss) noexcept;

    Widget& mountKind(reactive::Scope& scope, ForEach const& spec, ContainerWidget* parent);

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
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): resolves spans against the table's rows in one pass: what the widget is told depends on the difference from the previous resolve, and refusals report when their set changes
    void resolveSpans(TableWidget& widget, SpanTable& table);

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
                              std::vector<OpenItem>& kept);

    // The entry a path names among @p kept; null when the path leaves them.
    [[nodiscard]] static OpenItem const* openItemAt(std::vector<OpenItem> const& kept,
                                                    std::span<std::size_t const> path);

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
                         std::function<void(Key const&, std::size_t)> focusCell = {});

    // A selection of the view and the status line's entries. While `selectionView` is engaged the backend marks every
    // row but its keys; when it disengages the selection is given again, since the one the backend showed before the
    // view may equal the one it ends on, which would not be sent again.
    void bindSelectionView(reactive::Scope& scope, Table const& spec, TableWidget& widget, bool& viewing);

    // Rows first, then the selection. The widget marks a selected key whenever its row arrives, so the order only
    // shapes the log: the first setSelection comes after the rows and already marks them.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): mounts each table spec kind (rows, source, model) against one widget, in the order the rows then the selection must bind
    Widget& mountKind(reactive::Scope& scope, Table const& spec, ContainerWidget* parent);

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
                           VisibleRange const& window, Virtualization const& setting, bool windowed);

    // Tells the backend which rows are realized once @p keyCount is at or above the threshold, and that the window is
    // over once the count has fallen back below it; @p windowed is whether the backend was last told the rows are
    // windowed, and is updated.
    static void publishRealizedWindow(TableWidget& table, KeyedRows const& rows, std::size_t keyCount,
                                      Virtualization const& setting, bool& windowed);

    // A table over a `TableSource`: the rows in and near view, each built from the source's view index, and in each
    // row the cells of the columns in or near view.
    // NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): the fields are grouped and commented by role, and the realized rows are declared to go last first, as their comment says; a size-ordered layout would break both
    struct MORPH_API SourceTable {
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
        ~SourceTable();
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
                                             std::size_t column);

    [[nodiscard]] static SourceTable::PoolKey poolOf(SourceTable const& table, std::size_t column);

    // Whether the columns are laid out by the mount: told to the backend by `setColumnWindow`, and each cell placed
    // by `setCellColumn`.
    [[nodiscard]] static bool arranged(SourceTable const& table) {
        return table.horizontal || table.arrangement != TableArrangement{};
    }

    // A content column's width: its widest cell and its header once either is measured, the backend's estimate before.
    [[nodiscard]] static int contentWidthOf(ColumnMeasure const& known, TableMeasure const& measure);

    // Takes a new arrangement: the visible columns, their places, and their widths, the declared ones and the user's.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): takes a new arrangement: the visible columns, their places and their widths, declared and user's, each step reading the previous one's result
    static void arrange(SourceTable& table, TableArrangement arrangement);

    // The ids every `visible` call names: the always-fetched ones, then each window column's id and what its cell
    // reads.
    [[nodiscard]] static std::vector<std::string> fetchedOf(SourceTable const& table);

    // The rows `visible` is told: the reported range. Before any report, a window over a server's rows shows its
    // first `overscan` rows, and one over a document's rows the empty view a keyed list starts from.
    [[nodiscard]] static VisibleRange shownOf(SourceTable const& table);

    // The rows focus takes as in view: the reported range, or the first `overscan` rows before any report, whatever
    // the rows, so a page moves by the rows a view would show and a row past them is scrolled to.
    [[nodiscard]] static VisibleRange focusRangeOf(SourceTable const& table) {
        return table.range.count > 0 ? table.range : VisibleRange{.first = 0, .count = table.virtualize.overscan};
    }

    static void tellVisible(SourceTable& table);

    // Brings the column window up to date with the scroll, the arrangement, the widths and the rows' count, and tells
    // the backend and the rows what changed. Reads one width per column of the window, and none outside it.
    static void refreshColumns(SourceTable& table, std::size_t rows);

    // The place of the first column fully in view, past the pinned ones: a width change left of it moves the scroll
    // offset by as much, so the columns in view stay where they are.
    [[nodiscard]] static std::size_t firstFullPlace(SourceTable const& table);

    // Gives the stretch columns the width the other columns leave, by the rules of `stretchWidths`: each column's
    // natural width is the larger of its header and its widest cell measured so far, so a table over rows it has not
    // realized fits the rows it has seen. A table whose viewport the backend has not reported keeps the estimate.
    static void fitStretch(SourceTable& table);

    // The band runs over the visible columns, cut to the column window's places: a band exists for the columns in the
    // window. The backend is told only when they change.
    static void tellBands(SourceTable& table, std::vector<std::size_t> const& places, std::size_t pinned);

    // Builds a cell for @p column of @p row, showing @p record. All or nothing.
    [[nodiscard]] SourceTable::Cell makeCell(SourceTable& table, SourceTable::Realized& row, std::size_t column,
                                             Value const& record);

    // Sets a cell's record, counting a set that changed it.
    void setRecord(reactive::Signal<Value>& signal, Value const& record);

    // Gives a slot the row @p ref: its key, its record, its look. Its cells still show the row they showed, until
    // each is rebound; the cells of a row-scoped column (`TableSource::rowScopedCells`) of another key are dropped,
    // to be built for this one.
    void assignRow(SourceTable& table, SourceTable::Realized& slot, std::int64_t index, TableRowRef const& ref);

    // Unmounts a row's open cell editor, and draws the cell again.
    static void closeEditor(SourceTable& table, SourceTable::Realized& slot);

    // Brings the open editor to the cell `Table::editing` names: the one open on another cell is closed, and the
    // wanted one is opened once a realized row holds its key, built once, drawn over the cell and focused.
    void syncEditor(SourceTable& table);

    // A new slot at @p index: a stack, and the row if the rows hold it. All or nothing.
    [[nodiscard]] SourceTable::Realized makeRow(SourceTable& table, std::int64_t index);

    // Parks a placed cell in its row's pool: it leaves the row's layout, keeping its widget and its record.
    static void park(SourceTable& table, SourceTable::Realized& slot, std::size_t column);

    // One cell's work in a pass: binding a cell to another row or column, or building one.
    struct CellWork {
        std::size_t slot = 0;
        std::size_t column = 0;
        std::size_t distance = 0;
    };

    // Puts a cell at @p column of a slot: a stale placed cell is rebound, a pooled one taken and rebound, and only
    // an empty pool builds one.
    void realizeCell(SourceTable& table, SourceTable::Realized& slot, std::size_t column, Value const& record);

    // Tells the widget each placed cell's load state that changed.
    static void tellLoads(SourceTable& table, SourceTable::Realized& slot);

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
    void realizeWindow(SourceTable& table, std::function<bool(std::int64_t)> const& updated);

    // One pass of `realizeWindow`.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): rows, cells, the budget, focus and the reports.
    void realizeWindowPass(SourceTable& table, std::function<bool(std::int64_t)> const& updated);

    // The cells of the realized rows: columns that left the window are pooled, and the cells the window lacks are
    // bound or built within the frame's budget, nearest the centre first.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): pooling, the budget, and the order in each row.
    void realizeCells(SourceTable& table, VisibleRange const& shown);

    // Whether a column's cells each show one row only.
    [[nodiscard]] static bool rowScoped(SourceTable const& table, std::size_t column) {
        return column < table.source.rowScopedCells.size() && table.source.rowScopedCells.at(column);
    }

    // Destroys a row's placed and pooled cells of the row-scoped columns, and forgets their widgets among its
    // children. A default cell is never row-scoped: it reads its row's record alone.
    static void dropRowScoped(SourceTable const& table, SourceTable::Realized& slot);

    // Destroys a row's pooled cells, and forgets their widgets among its children.
    static void dropPools(SourceTable::Realized& slot);

    // A row's children in the window's order, then its pooled cells, for a backend that places cells by their order
    // rather than by `setCellColumn`.
    static void orderCells(SourceTable const& table, SourceTable::Realized& slot);

    // Posts the next turn's realization, once.
    void continueLater(SourceTable& table);

    // After a pass: the counts, the focused cell and the highlighted row.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity, readability-convert-member-functions-to-static): the pass's reports (counts, focused cell, highlighted row) each read the same table state in turn
    void afterPass(SourceTable& table, TableTotal const& total);

    // Asks the rows where the focused key is now, stopping an earlier ask.
    void locateFocus(SourceTable& table);

    // The rows' answer to where the focused key is. A key in the view takes focus at its index, its row kept
    // realized wherever it is; a key the view lacks has left it, and focus goes to the row now at its index, or the
    // last, as for a removed row. An answer the rows could not give leaves focus looking among the realized rows.
    // The backend is told after the pass, once the view's count is known (`focusUntold`): the last row of a count
    // not known yet is no row.
    void focusLocated(SourceTable& table, RowLocation const& found);

    // Tells the backend the focused cell the rows located. A row they do not hold, outside the rows in view, is
    // scrolled to, so the window fetches it, as for a move onto a row not fetched; a held row is left where it is.
    static void tellLocatedFocus(SourceTable& table);

    static void reportFocus(SourceTable const& table);

    // Where a view row is after a change: moved with the rows around it, or nothing when the change removed it. A
    // reset keeps the index.
    [[nodiscard]] static std::optional<std::int64_t> followIndex(table::ViewChange const& change, std::int64_t index);

    // Moves focus as a key asks: by rows or columns, to an end of the row, or to the first or last row of the view.
    // A row not fetched takes focus as a placeholder, and the view scrolls to it.
    void moveFocus(SourceTable& table, FocusMove move);

    // The view position of a key, scanning the view: a table selects cells only while its rows are all on the client.
    [[nodiscard]] static std::optional<std::size_t> viewRowOf(SourceTable const& table, Key const& key);

    // Shows a cell range at its corners' view rows and visible places now: a sort or a column move redraws the
    // rectangle the same corners span. A corner whose row is not in the view, or whose column is hidden, shows none.
    static void showCells(SourceTable& table);

    // A rectangle the backend reports, by view rows and visible places, as a range by keys and column ids; a corner
    // outside the view or the visible columns reports nothing.
    [[nodiscard]] static std::optional<CellRange> cellsAt(
        SourceTable const& table, std::size_t anchorRow,
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`anchorPlace`, `focusRow`): the selection's anchor then its focus, passed in that order by the one caller
        std::size_t anchorPlace, std::size_t focusRow, std::size_t focusPlace);

    // A grid's focus moves where the grid says: a gesture is handed to it as a cell, a row not fetched yet by its view
    // position alone.
    static void handFocus(SourceTable const& table, std::int64_t index, std::size_t column);

    // Shows the focus a grid holds: its key's row where a realized row holds it, else the view position it gives, and
    // its column by id.
    void gridFocusFrom(SourceTable& table, std::optional<GridFocus> const& focus);

    // Puts focus on a cell by view row and column: the row is kept realized, scrolled to when it is outside the rows
    // in view, and the backend shows it.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`index`, `column`): row index then column, the order every focus seam in this file uses
    void placeFocus(SourceTable& table, std::int64_t index, std::size_t column);

    // Takes focus from `Table::focus`: the cell's row by key among the realized rows, and its column by id.
    void focusFrom(SourceTable& table, std::optional<CellRef> const& cell);

    // Focuses a realized row's cell, as a cell menu opening on it does, and reports it as a key's move is reported.
    void focusAt(SourceTable& table, Key const& key, std::size_t column);

    // Moves focus and the highlight from a row's old key to its new one, for each new generation of moves. Focus on
    // the old key, or left by it when its row went, goes to the new key: at once when a realized row holds it, else
    // the rows are asked where it is, as after a reset, and the change that removes the old row moves it nowhere
    // meanwhile; an answer that the view lacks the new key hands focus to the row at its index.
    void applyKeyMoves(SourceTable& table, KeyMoves const& moves);

    // Takes a highlight: the rows fetch around it, the view scrolls to it and marks it, and its key is followed.
    static void highlightFrom(SourceTable& table, std::optional<TableHighlight> const& highlight);

    // Where a key is among the realized rows' places in the view the rows now hold, or nothing.
    [[nodiscard]] static std::optional<std::int64_t> placeOfKey(SourceTable const& table, Key const& key,
                                                                std::int64_t also);

    // Follows the focused and the highlighted rows through a change of the rows. A reset (a new sort, filter or
    // description) gives no places, and the rows may come back later (a server's pages are dropped first), so a key
    // no realized row has is looked for after each pass until one has it; meanwhile the highlight shows no mark and
    // focus stays on its key. Focus also asks the rows where its key is, since a new order can move it far from the
    // rows realized; only their answer that the view lacks it moves focus to another row. An index at or past the
    // row count before the change names no row, so nothing moves it; after a reset no row is counted until the next
    // pass, so a first page that arrives before it moves nothing either.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): follows the focused and the highlighted rows through each operation of a change, which shifts both by the operation kind
    void follow(SourceTable& table, table::ViewChange const& change);

    // Whether a change names the view index @p index as updated: a reset names every row, and a row inserted,
    // changed or moved to the index is named.
    [[nodiscard]] static bool namesUpdated(table::ViewChange const& change, std::int64_t index);

    // What an editable table adds: its focus held by a grid, a click on a cell, typed text, its own keys, and focus
    // leaving it.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): binds what an editable table adds (focus, click, typed text, keys, focus loss), each handler capturing the same table state
    void bindGrid(reactive::Scope& scope, Table const& spec, TableWidget& widget, SourceTable& table);

    // A table over a `TableSource`. Rows are windowed, except a table over a document's rows below its threshold,
    // which realizes every row: the mount realizes the rows the backend reports in view and `overscan` rows either
    // side; before any report, the first `overscan` rows of a server's rows, and none of a document's (`shownOf`).
    // Columns are windowed once the backend reports its horizontal viewport and `windowingOf` says so; until then
    // every visible column is in the window. The mount tells the rows which rows and columns are in view, and applies
    // each `ViewChange` to the realized rows by key. Rows first, then the selection, as for a table over `rows`.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one handler per table seam.
    Widget& mountSourceTable(reactive::Scope& scope, Table const& spec, ContainerWidget* parent);

    // What the backend measured of @p column: a content column is as wide as its widest cell and its header, which
    // only grow once measured, and a change left of the first column fully in view moves the scroll offset by as much,
    // so the columns in view stay where they are. A
    // stretch column's measure feeds the fit of every stretch column to the table, which may narrow as well as widen
    // them. A fixed column keeps its width.
    void measured(SourceTable& table, std::size_t column, ColumnMeasure const& reported);

    // The model, then the session, then the rows, then the Effect(s): destroyed in reverse, so an Effect stops
    // first, the rows (whose slots point into the session's snapshot) go next, and the model outlives its session.
    // @p virtualize is null for a ForEach (no backend seam exists yet to dewindow a bare keyed stack); when
    // non-null but @p container is not also a TableWidget (today, impossible -- only Table passes a non-null
    // virtualize, and only its mountKind gives it a TableWidget -- kept as a defensive check, not a real case),
    // virtualization is silently not wired, same as if it were null.
    void mountRows(reactive::Scope& scope, std::shared_ptr<ForEachModel const> const& model,
                   ContainerWidget& container, RowHook onRow, Virtualization const* virtualize,
                   RowsObserver observer = {});

    // The snapshot index of each key's first occurrence, in order; every later duplicate is reported.
    [[nodiscard]] std::vector<std::size_t> firstOccurrences(std::vector<Key> const& keys);

    // All or nothing: the row's state and its widget are each built in a scope of their own, one level deeper than
    // the ForEach's, which the caller takes only once the mount completed. A new row's widget is realized in the
    // same pass as its state (so it is eligible for autofocus like any other newly-mounted content); a widget
    // realized later, on a row whose state already exists, goes through `realizeRowWidget` instead, which is not
    // its own `mountPass` -- see that function's own comment for why.
    [[nodiscard]] KeyedRow mountRow(std::size_t depth, ForEachSession& session, ContainerWidget& container,
                                    Key const& key, std::size_t index, RowHook const& onRow);

    // Realizes a widget for a row whose state (`stateScope`/`slot`) already exists -- a row re-entering the
    // visible range after being dewindowed. Deliberately not a `mountPass` of its own: `mountPass` gives the first
    // `autofocus` widget it mounts the focus once it completes, which is right for a row mounting for the first
    // time (`mountRow`, inside the ForEach's own outer pass) but wrong here -- a row that already existed and
    // simply scrolled back into view must not steal focus back from wherever the user's focus already is. `mount`
    // itself carries no such behavior; only `mountPass` does, so simply not using it is what suppresses it.
    void realizeRowWidget(std::size_t depth, KeyedRow& row, ContainerWidget& container, RowHook const& onRow);

    // Releases a row's widget, keeping its state: the container sees an ordinary removal (`releasing` fires exactly
    // as it does for a real removal), but `stateScope`/`slot` are untouched, so the row's signal keeps tracking the
    // snapshot and is ready for `realizeRowWidget` to mount a fresh widget against it later.
    static void dewindowRowWidget(KeyedRow& row);

    // Builds only a new key's state -- no widget, no mount, no onRow call -- for a row `reconcile` decides is
    // outside the current window on its very first appearance (a virtualized table above its threshold). The row
    // starts `Dewindowed`, exactly as if it had been realized once and then released, so `realizeRowWidget` can
    // bring it in later with no special case for "never had a widget" versus "had one and lost it."
    [[nodiscard]] KeyedRow mountRowState(std::size_t depth, ForEachSession& session, Key const& key,
                                         std::size_t index);

    // Moves the widgets into the snapshot's order, and `rows` with them. A move that throws is recorded in `failure`
    // when it is the first failure, and leaves `rows` mirroring the container as it stands.
    void reorder(std::vector<KeyedRow>& rows, ContainerWidget& container, std::vector<Key> const& keys,
                 std::vector<std::size_t> const& order, std::exception_ptr& failure);

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
                   std::pair<VisibleRange, std::size_t> const* windowAndOverscan = nullptr);

    // Realizes a widget for every row within `window` (± `overscan`) that does not already have one, and releases
    // every realized widget outside it. A row's position in view order is its own `viewPosition`, set by the most
    // recent `reconcile` pass -- not its position in `rows` itself, which groups realized rows ahead of
    // dewindowed ones (so the container only ever holds the widgets that exist) and so no longer matches view
    // order once any row is dewindowed. Called only when `rows.size() >= threshold`, from the windowing Effect's
    // own Signal<VisibleRange>, never from reconcile, so a scroll alone costs O(rows) to scan, not the full
    // reconciliation pass.
    void applyWindow(std::size_t depth, std::vector<KeyedRow>& rows, ContainerWidget& container,
                     VisibleRange const& window, std::size_t overscan, RowHook const& onRow);

    // The realized subset's extent in view order: the first realized row's view position and how many rows from
    // there are realized. Used only to tell the backend what to size a scrollbar against
    // (`TableWidget::setRealizedWindow`); a gap of dewindowed rows between two realized ones (the overscan margin
    // straddling a scroll position mid-transition) is not expected in steady state, so this reports the outermost
    // realized view positions, not a strict contiguous run.
    [[nodiscard]] static std::pair<std::size_t, std::size_t> realizedExtent(std::vector<KeyedRow> const& rows);

    Widget& mountKind(reactive::Scope& scope, Busy const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Banner const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Badge const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Progress const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Steps const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Chart const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, KeyValue const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, EmptyState const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Splitter const& spec, ContainerWidget* parent);

    // The header is mounted first, so it is the first child; the content follows it.
    Widget& mountKind(reactive::Scope& scope, Collapsible const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, DropZone const& spec, ContainerWidget* parent);

    // The refusal text of a page node, naming its test id the way every layout refusal does.
    [[nodiscard]] static std::logic_error pageRefusal(Common const& common, std::string const& what);

    // A page sizes itself: it takes `alignSelf` (not `Stretch`) and no other layout hint. A region is sized by its
    // geometry and takes none.
    static void checkPageLayout(Common const& common, bool isPage);

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

    Widget& mountKind(reactive::Scope& scope, Page const& spec, ContainerWidget* parent);

    // An accented cell anywhere but a source table's cell: its content, with the accent not read.
    Widget& mountKind(reactive::Scope& scope, AccentedCell const& spec, ContainerWidget* parent) {
        return *mount(scope, spec.content, parent);
    }

    // A marked cell anywhere but a source table's cell: its content, with the marks not read.
    Widget& mountKind(reactive::Scope& scope, MarkedCell const& spec, ContainerWidget* parent) {
        return *mount(scope, spec.content, parent);
    }

    Widget& mountKind(reactive::Scope& scope, PageRegion const& spec, ContainerWidget* parent);

    // One prop of a loaded component, bound to the setter its kind crosses by.
    void bindCustomProp(reactive::Scope& scope, CustomWidget& widget, CustomProp const& prop);

    // The component the backend loads in the custom node's place, or the fallback when it loads none.
    // An ActionBar holds the actions of what holds it: the footer of the overlay whose content it is in, else the
    // place it is written. It is a horizontal stack either way, marked as a row of actions so the backend spaces its
    // children by the theme's action gap.
    Widget& mountKind(reactive::Scope& scope, ActionBar const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Custom const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, DateTimeInput const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, Slider const& spec, ContainerWidget* parent);

    Widget& mountKind(reactive::Scope& scope, FilePicker const& spec, ContainerWidget* parent);

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
class MORPH_API Mounted {
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
    [[nodiscard]] static Node nonNull(Node root);

    Node _root;
    detail::Mounter _mounter;
    reactive::Scope _scope;  // after _mounter: destroyed first, while the mounter its bindings call is alive
    Widget* _widget;
};

}  // namespace morph::ui
