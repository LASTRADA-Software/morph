// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <cstddef>
#include <functional>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tui/context.hpp"
#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Table: a header row above keyed rows. Each row is a horizontal stack of cells laid out in the column widths the
/// solver gives the table, one cell gap apart; a hidden cell keeps its column, and counts toward its width, so the
/// cells after it stay under their own headers. A two-cell gutter marks selected rows `*` and, while the table has
/// focus, the cursor row `>`. With a select column the gutter is five cells: a `[x]` or `[ ]` box per row in place of
/// the `*`, under a header box that reads `[x]`, `[-]` or `[ ]`; a click on a row's box toggles the row as Ctrl+click
/// does, and a click on the header box is Ctrl+A. A click on a sortable column's header asks to sort by it
/// (`setOnSort`), and the sorted column's label is followed by ` ▲` or ` ▼`; a sortable column's header keeps room
/// for the mark, so the columns do not move when the sort does. The header takes no keyboard focus. A heading row
/// (`setRowHeading`) is one cell across every column; it draws no mark or box, the cursor passes over it, a click on
/// it does nothing, and Ctrl+A, a range and the header box leave it out.
///
/// Up, Down, Home and End move the cursor, and PageUp and PageDown by as many rows as fill the room below the header;
/// Space selects the cursor row (Single) or toggles it and makes it the anchor (Multiple); Enter selects it in Single
/// mode and activates it in every mode. In Multiple mode Shift+Up and Shift+Down move the cursor and select the rows
/// from the anchor to it, and Ctrl+A selects every row, or clears them when every row is selected; in Single mode
/// Shift with an arrow moves as the arrow does and Ctrl+A goes on outwards. A click moves the cursor to a row and
/// selects only that row, making it the anchor; Ctrl+click toggles the row and makes it the anchor, Shift+click
/// selects the rows from the anchor to it, where the terminal passes the modifier on. A second click on the same row
/// within half a second (on `Context::now`), with no key between, is a double click, which activates the row and
/// leaves the selection as the first click left it. A row the user cannot reach (disabled) is neither selected nor
/// activated. A click, a Shift-click, Shift with an arrow and every Single change report `Replace`; a Ctrl-click,
/// Space and Ctrl+A `Extend`.
///
/// The table keeps the keys last requested by `setSelection` or selected by the user, and marks every row that has
/// one of them: a key with no row marks nothing and stays requested, a row that goes is unmarked and is marked again
/// when it returns, and a moved row keeps its mark and the cursor. None of that calls a handler. A user's change
/// replaces the requested keys with exactly the keys of existing rows then selected, in the order selected, and
/// reports them; a change that selects what is already selected reports nothing.
///
/// Rows that do not fit below the header scroll: whenever the cursor moves, the focus moves to the table or into a
/// row, or the room for rows changes, the view brings the cursor row into view. The wheel moves the view one row and
/// is left to the table's surroundings when the view cannot move that way. A row scrolled out of view is given no
/// area, so it neither draws nor takes a click.
///
/// A table over a `TableSource` draws what the mount realizes. Its view scrolls over every row of the view, one line
/// each, and each realized row is drawn on the line of its view index (`setRowIndex`); a line whose row is not
/// realized is blank. Each cell is drawn in the column `setCellColumn` gave it, and a cell with none is not drawn.
/// Once the mount lays the columns out (`setColumnWindow`), the header and the cells show the window's columns only,
/// at the mount's offsets and widths in cells, the last cell of each its gap; the pinned columns stay at the leading
/// edge and the rest scroll under them. The table reports the columns and the rows in view (`setOnHorizontalRange`,
/// `setOnVisibleRangeChanged`) before the next frame's layout whenever a frame found them changed, and at once when
/// the wheel or `scrollColumnsTo` moves the view. The arrows, Home, End, PageUp, PageDown, Ctrl+Home and Ctrl+End go
/// to the mount (`setOnFocusMove`), and the focused cell (`setFocusCell`) is kept in view: Left and Right are how the
/// user scrolls sideways. A row is one line, so no row grows, and none above the view moves the rows in it. An
/// `Unloaded` cell draws `···` and a `Failed` one `!`, in its column's alignment, in place of its content.
///
/// The status line (`setStatus`), while shown, is the table's last line: the counts, the state, `[Retry]` while the
/// state offers it, which a click chooses, and the message at the trailing edge.
class TableImpl final : public TuiContainer<ui::TableWidget> {
public:
    explicit TableImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setColumns(std::vector<ui::TableColumn> const& columns) override;
    void setShowHeader(bool shown) override;
    void setSelectionMode(ui::SelectionMode mode) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowKey(ui::Widget& row, ui::Key const& key) override;
    /// The row's cells draw in the tone's style. Throws std::logic_error when @p row is not one of this table's rows.
    void setRowTone(ui::Widget& row, std::optional<ui::Tone> tone) override;
    void setSelectColumn(bool shown) override;
    void setSelectAllLabel(std::string_view label) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowLabel(ui::Widget& row, std::string_view label) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowHeading(ui::Widget& row) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowSpans(ui::Widget& row, std::vector<ui::RowSpan> const& spans) override;
    void setSelection(std::vector<ui::Key> const& selection) override;
    void setOnSelectionChange(
        std::function<void(std::vector<ui::Key>, ui::SelectionChange)> onSelectionChange) override;
    void setOnActivate(std::function<void(ui::Key)> onActivate) override;
    void setSort(std::optional<ui::TableSort> sort) override;
    void setOnSort(std::function<void(std::size_t)> onSort) override;
    void setRealizedWindow(std::size_t first, std::size_t count, std::size_t total) override;
    void setOnVisibleRangeChanged(std::function<void(std::size_t, std::size_t)> onVisibleRangeChanged) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowIndex(ui::Widget& row, std::size_t viewRow) override;
    void setStatus(ui::TableStatus const& status) override;
    void setOnRetry(std::function<void()> onRetry) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setCellLoad(ui::Widget& row, std::size_t column, ui::CellLoad load) override;
    void setColumnWindow(std::span<std::size_t const> columns, std::span<int const> offsets,
                         std::span<int const> widths, int totalWidth, std::size_t pinned) override;
    void setOnHorizontalRange(std::function<void(int scrollX, int viewport)> handler) override;
    void setHorizontalOffset(int scrollX) override;
    void setFocusCell(std::size_t viewRow, std::size_t column) override;
    void setOnFocusMove(std::function<void(ui::FocusMove)> handler) override;
    void scrollToRow(std::size_t viewRow) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setCellColumn(ui::Widget& row, ui::Widget& cell, std::optional<std::size_t> column) override;
    /// Moves the view sideways to @p scrollX cells into the columns, kept within them, and reports the columns in
    /// view.
    void scrollColumnsTo(int scrollX);
    /// Reports the columns, then the rows, in view, each when it changed since it was last reported. `fit` calls it
    /// before a frame's layout for a table whose last frame found them changed.
    void reportRanges();
    /// The positions among `children()` of the rows the table marks as selected, ascending.
    [[nodiscard]] std::vector<std::size_t> markedRows() const;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void clickWith(::core::tui::Point cell, ::core::tui::Modifier modifiers) override;
    [[nodiscard]] bool wheel(int delta) override;

    /// Works out which cells span and which are covered from the shown rows' order and their spans, clipping a span
    /// that runs past the last row or onto a heading, lifts the spanning cells into the table's view and hides the
    /// covered ones, and finds the lines a spanning cell needs beyond the rows it covers.
    /// Runs before every frame's layout, as well as in the frame, so the table is sized by rows that already
    /// lack their lifted cells.
    void planSpans();

protected:
    void childForgotten(WidgetBase& child) override;

private:
    /// A column as it is drawn: its index into `_columns`, and where its cells go, from the end of the gutter.
    struct DrawnColumn {
        std::size_t column = 0;
        int x = 0;
        int width = 0;  ///< Without the gap after it.
    };
    /// A spanning cell as the last plan found it: lifted out of its origin row and drawn over the rows it covers.
    struct SpanPlace {
        WidgetBase* origin = nullptr;  ///< The row that gives the span.
        WidgetBase* cell = nullptr;    ///< The spanning cell, a child of the table's view while the plan holds it.
        std::size_t column = 0;
        std::size_t first = 0;  ///< The origin's place among the shown rows.
        std::size_t last = 0;   ///< The last covered row's place.
    };
    /// The lines of @p row: its natural height, a line at least, and what a spanning cell it ends needs besides.
    [[nodiscard]] int heightOf(WidgetBase const& row) const;
    /// The area of each spanning cell for a table of @p size: the lines of the rows it covers, clipped to those
    /// shown, and its column's cells, placed down them by the column's `valign`; empty while none shows. With
    /// @p blocks, those lines and cells before the placement: where a click selects the cell's origin row.
    [[nodiscard]] std::vector<::core::tui::Rect> spanAreas(::core::tui::Size size,
                                                           std::vector<::core::tui::Rect>* blocks = nullptr) const;
    /// The origin row of the spanning cell at @p cell, or null.
    [[nodiscard]] WidgetBase* spanOriginAt(::core::tui::Point cell) const;
    /// The cells before the first column: the marks, and the select column's box when it is drawn.
    [[nodiscard]] int gutter() const noexcept;
    /// The header's box: `[x]` while every shown row is selected, `[-]` while some are, `[ ]` while none are.
    [[nodiscard]] std::string headerBox() const;
    /// The header text of the column at @p index: its label, then the sort mark when it is the sorted column.
    [[nodiscard]] std::string headerText(std::size_t index) const;
    /// The cells a column's header needs: its label, and room for the sort mark when it is sortable.
    [[nodiscard]] static int headerWidth(ui::TableColumn const& column);
    /// Calls onSort when the header click at column @p x lands on a sortable column.
    void clickHeader(int x);
    /// Draws the header line: the header box, then each column's label at its alignment.
    void paintHeader(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& columns) const;
    /// Gives each row its column layout.
    void arrangeRows(std::vector<int> const& widths);
    /// The gutter of @p row: its selection mark, or its box, then the cursor mark when @p cursor.
    [[nodiscard]] std::string rowMark(WidgetBase const* row, bool cursor) const;
    /// Each column's content width: its label's, or its widest shown cell's.
    [[nodiscard]] std::vector<int> naturalWidths() const;
    /// Each column's least width: for a column that wraps, the widest of its header and its shown cells' least
    /// widths (a text cell's widest word; any other cell's natural width); for a column that does not, its natural
    /// width.
    [[nodiscard]] std::vector<int> leastWidths() const;
    /// The solved column widths for a table @p width cells wide.
    [[nodiscard]] std::vector<int> columnWidths(int width) const;
    [[nodiscard]] std::optional<ui::Key> keyOf(WidgetBase const* row) const;
    /// The row with @p key, or null.
    [[nodiscard]] WidgetBase* rowWith(ui::Key const& key) const;
    [[nodiscard]] bool isMarked(WidgetBase const* row) const;
    [[nodiscard]] bool isHeading(WidgetBase const* row) const { return _headings.contains(row); }
    /// The first place from @p place among @p rows, toward the end (or the start when there is none there), that is
    /// not a heading; @p place itself when every row is one.
    [[nodiscard]] std::size_t dataPlace(std::span<WidgetBase* const> rows, std::size_t place,
                                        Direction direction) const;
    /// The cursor's position among @p rows (the shown rows); 0 when there are none.
    [[nodiscard]] std::size_t cursorPlace(std::span<WidgetBase* const> rows) const;
    /// Puts the cursor on the row at @p place of @p rows, and has the next frame bring it into view.
    void moveCursor(std::span<WidgetBase* const> rows, std::size_t place);
    /// The first of @p rows to draw so that the last ones fill the room below the header.
    [[nodiscard]] std::size_t lastTop(std::span<WidgetBase* const> rows) const;
    /// The row a page away from @p place among @p rows in @p direction: as far as the rows passed fit in the room
    /// below the header, and at least one row on while there is one.
    [[nodiscard]] std::size_t pageFrom(std::span<WidgetBase* const> rows, std::size_t place,
                                       Direction direction) const;
    /// Moves the cursor into the row holding the focus when the focus moved there, and has the view follow it when
    /// the focus moved to the table or into a row.
    void followFocus(std::span<WidgetBase* const> rows);
    /// Moves the view so that the cursor row shows.
    void showCursor(std::span<WidgetBase* const> rows);
    /// The drawn row at line @p line of the table, or null.
    [[nodiscard]] WidgetBase* rowAtLine(int line) const;
    /// Selects @p row (Single) or toggles it (Multiple) as the user did, and reports a change; whether this table is
    /// still there afterwards.
    [[nodiscard]] bool pick(WidgetBase const& row);
    /// What a click on @p row selects, as the user did, and reports a change; whether this table is still there
    /// afterwards.
    [[nodiscard]] bool choose(WidgetBase const& row, ui::RowClick click);
    /// The keys of the shown rows from the anchor to @p row, in row order; @p row alone without an anchor.
    [[nodiscard]] std::vector<ui::Key> rangeTo(WidgetBase const& row) const;
    /// Every shown row's key, or none when every one is selected already.
    [[nodiscard]] std::vector<ui::Key> allOrNone() const;
    /// Makes @p next the selection and reports it as @p change, unless it is the selection already; whether this
    /// table is still there afterwards.
    [[nodiscard]] bool change(std::vector<ui::Key> next, ui::SelectionChange change);
    /// Calls onSelectionChange with @p keys and @p change; whether this table is still there afterwards.
    [[nodiscard]] bool report(std::vector<ui::Key> keys, ui::SelectionChange change);
    /// Handles the keys a modifier changes: Shift+Up, Shift+Down and Ctrl+A.
    [[nodiscard]] ::core::tui::EventResult modifiedKey(::core::tui::KeyEvent const& key);
    /// Calls onActivate with @p key when a row the user can reach has it; the handler may destroy this table.
    void activateRow(ui::Key const& key);

    /// The columns drawn in a table @p width cells wide: the mount's window once it lays the columns out, else every
    /// column at its solved width.
    [[nodiscard]] std::vector<DrawnColumn> drawnColumns(int width) const;
    /// The column a cell of @p row is in: by `setCellColumn` in a table over a source, else its @p position.
    [[nodiscard]] std::optional<std::size_t> columnOf(WidgetBase const* row, WidgetBase const* cell,
                                                      std::size_t position) const;
    /// The load state of @p row's cell in @p column.
    [[nodiscard]] ui::CellLoad loadOf(WidgetBase const* row, std::size_t column) const;
    /// Gives each row of a table over a source a slot per cell: its column's, or none.
    void slotCells(std::vector<DrawnColumn> const& drawn);
    /// The lines below the header that rows take in a table @p height lines tall: all but the status line.
    [[nodiscard]] int rowRoomOf(int height) const noexcept;
    /// The view's width across the columns: the table's, less the gutter.
    [[nodiscard]] int viewport() const noexcept;
    /// The rows in view, as the mount is told them: the first view row drawn and how many lines rows take.
    [[nodiscard]] std::pair<std::size_t, std::size_t> rowsInView() const noexcept;
    /// Keeps the view within the rows and the columns.
    void clampView();
    /// Has the next frame report the ranges in view, when they changed.
    void reportBeforeNextFrame();
    /// Draws the status line on the last line.
    void paintStatus(::core::tui::Canvas& canvas);
    /// Draws the marks of the cells that are not loaded, in their rows' @p areas.
    void paintLoads(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& drawn,
                    std::vector<::core::tui::Rect> const& areas) const;
    /// The realized row at view row @p viewRow, or null.
    [[nodiscard]] WidgetBase* rowAtView(std::size_t viewRow) const;
    /// The move a key asks of the mount in a table over a source, or none.
    [[nodiscard]] static std::optional<ui::FocusMove> focusMoveOf(::core::tui::KeyEvent const& key);

    std::vector<ui::TableColumn> _columns;
    bool _showHeader = true;
    bool _selectColumn = false;
    ui::SelectionMode _mode = ui::SelectionMode::None;
    std::unordered_map<WidgetBase const*, ui::Key> _keys;
    std::unordered_set<WidgetBase const*> _headings;
    std::unordered_map<WidgetBase const*, std::vector<ui::RowSpan>> _spans;  ///< Each row's effective spans.
    std::vector<SpanPlace> _plan;                                            ///< The spanning cells, as last planned.
    std::unordered_map<WidgetBase const*, int> _extraLines;  ///< The lines a spanning cell adds to its last row.
    std::vector<ui::Key> _selection;  ///< The keys requested or selected by the user, in that order.
    WidgetBase* _cursor = nullptr;    ///< The cursor row; null for the first shown row.
    std::optional<ui::Key> _anchor;   ///< Where a range starts: the row last clicked or toggled.
    std::size_t _cursorPlace = 0;     ///< The cursor's position among the shown rows, when last known.
    std::size_t _top = 0;             ///< The first shown row drawn.
    ::core::tui::Size _size{};        ///< The table's size, as of the last frame.
    int _rowRoom = 0;                 ///< Lines below the header, as of the last frame.
    bool _follow = true;              ///< Whether the next frame brings the cursor row into view.
    /// The focused view the table last looked at, compared by address only: it may be gone by the next frame.
    ::core::tui::Component const* _focusSeen = nullptr;

    /// The first click of a possible double click.
    struct LastClick {
        WidgetBase const* row = nullptr;           ///< The row clicked; null for none.
        std::chrono::steady_clock::time_point at;  ///< When.
    };
    LastClick _lastClick;
    std::function<void(std::vector<ui::Key>, ui::SelectionChange)> _onSelectionChange;
    std::function<void(ui::Key)> _onActivate;
    std::optional<ui::TableSort> _sort;
    std::function<void(std::size_t)> _onSort;

    /// Whether the rows come from a `TableSource`: the mount places each row by its view index and each cell by its
    /// column.
    bool _source = false;
    std::size_t _total = 0;                                        ///< The rows of the view.
    std::unordered_map<WidgetBase const*, std::size_t> _viewRows;  ///< Each realized row's view index.
    /// Each row's cells' columns, by `setCellColumn`; none for a pooled cell.
    std::unordered_map<WidgetBase const*, std::unordered_map<WidgetBase const*, std::optional<std::size_t>>>
        _cellColumns;
    /// Each row's cells that are not loaded, by column.
    std::unordered_map<WidgetBase const*, std::unordered_map<std::size_t, ui::CellLoad>> _loads;
    /// A column of the mount's window: its index into `_columns`, its offset and its width.
    struct WindowColumn {
        std::size_t column = 0;
        int offset = 0;
        int width = 0;
    };
    bool _arranged = false;  ///< Whether the mount lays the columns out.
    std::vector<WindowColumn> _window;
    std::size_t _pinned = 0;
    int _totalWidth = 0;
    int _scrollX = 0;
    std::function<void(std::size_t, std::size_t)> _onVisibleRange;
    std::function<void(int, int)> _onHorizontalRange;
    std::optional<std::pair<std::size_t, std::size_t>> _reportedRows;
    std::optional<std::pair<int, int>> _reportedColumns;
    std::function<void(ui::FocusMove)> _onFocusMove;
    std::optional<std::pair<std::size_t, std::size_t>> _focusCell;  ///< The focused cell's view row and column.
    ui::TableStatus _status;
    std::function<void()> _onRetry;
    int _retryX = -1;  ///< Where `[Retry]` starts on the status line; -1 while it is not drawn.
};

}  // namespace morph::tui::detail
