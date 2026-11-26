// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
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
#include "tui/list_widgets.hpp"
#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Table: a header row above keyed rows. Each row is a horizontal stack of cells laid out in the column widths the
/// solver gives the table, one cell gap apart; a hidden cell keeps its column, and counts toward its width, so the
/// cells after it stay under their own headers. A two-cell gutter marks selected rows `*` and, while the table has
/// focus, the cursor row `>`. With a select column the gutter is five cells: a `[x]` or `[ ]` box per row in place of
/// the `*`, under a header box that reads `[x]`, `[-]` or `[ ]`; a click on a row's box toggles the row as Ctrl+click
/// does, and a click on the header box is Ctrl+A. A click on a sortable column's header asks to sort by it
/// (`setOnSort`), `Plain`, or `Shift` with Shift held; each header of the sort chain is followed by its mark
/// (`ui::sortMarkText`: ` ▲` or ` ▼`, with its place from 1 when the chain has two or more keys), and a sortable
/// column's header keeps room for the mark, so the columns do not move when a one-key sort does. A filterable column's
/// header ends in its filter button, `▿`, drawn `▾` while `setFiltered` names the column, which a click presses
/// (`setOnFilter`). Groups (`setColumnGroups`) draw a line above the headers: each group's label over the visible
/// columns it names, ending in its own filter button. Bands (`setBands`) draw a line per level above that, outermost
/// first: each run's label, then its unit in brackets, from the run's first drawn column, cut at its last. With menus
/// (`setMenu`), the header line keeps its last two cells for a `⋯` that opens the table's menu, so no column's header
/// or filter button lies under it; a header's menu opens on a right click on the header, or with Shift+F10 or the
/// context-menu key on the focused header. A menu is a list below its header, as a button-style menu's is. The
/// terminal draws no drag or resize handle: a move is Ctrl+Shift+Left and Ctrl+Shift+Right on the focused header, and
/// nothing resizes a column. With a cell menu
/// (`setCellMenu`), a right click on a row's cell opens it below the cell, after selecting the row alone when it is
/// not selected; over a source, Shift+F10 or the context-menu key opens it on the focused cell, and a key the table
/// leaves that is one of `setCellMenuKeys` runs the focused cell's entry whose chord it is.
///
/// While a header can act (a column sorts, or the table has menus or moves columns), it takes the keyboard focus
/// within the table: Up on the first row (the first view row over a source) focuses the header of the chain's first
/// key, or the first visible column, drawn highlighted while the table has
/// the focus. Left and Right move along the visible columns, Enter and Space sort (`Plain`), Shift+Enter and
/// Shift+Space extend (`Shift`), Ctrl+Shift+Left and Ctrl+Shift+Right report a move one place over, Shift+F10 opens
/// its menu, and Down returns to the rows. Ctrl+C on the table asks to copy (`setOnCopy`) wherever its focus is. A
/// heading row
/// (`setRowHeading`) is one cell across every column; it draws no mark or box, the cursor passes over it, a click on
/// it does nothing, and Ctrl+A, a range and the header box leave it out. A summary row (`setRowSummary`) draws its
/// cells as any row does and `Σ` in its gutter; like a heading, it is never selected, activated or the cursor's.
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
    void setRowSummary(ui::Widget& row) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowSpans(ui::Widget& row, std::vector<ui::RowSpan> const& spans) override;
    void setSelection(std::vector<ui::Key> const& selection) override;
    void setOnSelectionChange(
        std::function<void(std::vector<ui::Key>, ui::SelectionChange)> onSelectionChange) override;
    void setOnSelectAll(std::function<void()> onSelectAll) override;
    void setSelectionView(std::vector<ui::Key> const& except) override;
    void setOnStatusEntry(std::function<void(std::string entryId)> onStatusEntry) override;
    /// An unmatched row's cells draw dim; a busy row shows the busy glyph and a locked row the lock glyph in gutter
    /// cells of their own (the glyph and a gap), after the selection and cursor marks; the marks' tooltip, or for a
    /// busy or locked row without one a text saying why, is the row's hover text over its own tooltip. Throws
    /// std::logic_error when
    /// @p row is not one of this table's rows.
    void setRowMarks(ui::Widget& row, ui::RowMarks const& marks) override;
    void setOnActivate(std::function<void(ui::Key)> onActivate) override;
    void setSort(std::vector<ui::TableSort> const& chain) override;
    void setOnSort(std::function<void(std::size_t, ui::HeaderClick)> onSort) override;
    void setMenu(std::function<std::vector<ui::MenuEntry>(std::optional<std::size_t>)> entries,
                 std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> onChoose) override;
    void setCellMenu(
        std::function<std::vector<ui::MenuEntry>(ui::Key const&, std::size_t)> entries,
        std::function<void(ui::Key const&, std::size_t, std::vector<std::size_t> const&)> chosen) override;
    void setCellMenuKeys(std::vector<std::string> const& chords) override { _cellMenuKeys = chords; }
    void setVisibleColumns(std::vector<std::size_t> const& visible) override;
    void setOnColumnMove(std::function<void(std::size_t, std::size_t)> handler) override;
    void setOnFilter(std::function<void(std::size_t)> handler) override;
    void setFiltered(std::vector<std::size_t> const& columns) override;
    void setColumnGroups(std::vector<ui::TableColumnGroup> const& groups) override;
    void setBands(std::vector<ui::BandRun> const& runs) override;
    void setOnGroupFilter(std::function<void(std::size_t)> handler) override;
    void setOnCopy(std::function<void()> onCopy) override;
    void closePopup() override { _menuPopup.close(); }
    [[nodiscard]] ::core::tui::Component const* openPopup() const override { return _menuPopup.list(); }
    /// The column whose header has the focus within the table, or none while the rows have it.
    [[nodiscard]] std::optional<std::size_t> focusedHeader() const noexcept { return _headerFocus; }
    /// The cells the header line keeps at its end for the `⋯`: a gap and the button, while the table has menus.
    [[nodiscard]] int menuRoom() const noexcept;
    /// Whether a focused header could act: a column sorts, or the table has menus or moves columns.
    [[nodiscard]] bool headerActs() const;
    /// The visible columns in layout order: as `setVisibleColumns` told them, or every column.
    [[nodiscard]] std::vector<std::size_t> visibleOrder() const;
    /// The entries of the open header or table menu; empty while none is open.
    [[nodiscard]] std::vector<ui::MenuEntry> openMenuEntries() const {
        return _menuPopup.isOpen() ? _menuPopup.entries() : std::vector<ui::MenuEntry>{};
    }
    /// The header text of the column at @p index: its label with its unit (`Mass [kg]`), then its sort key's mark
    /// when it is in the chain.
    [[nodiscard]] std::string headerText(std::size_t index) const;
    /// The accessible name of a column's header: its label with its unit (`ui::headerAccessibleName`), followed by
    /// `, filtered` while it has a filter.
    [[nodiscard]] std::string headerName(std::size_t index) const;
    /// The accessible description of a column's header: its sort key's (`ui::sortDescription`), or empty.
    [[nodiscard]] std::string headerDescription(std::size_t index) const;
    void setRealizedWindow(std::size_t first, std::size_t count, std::size_t total) override;
    void setOnVisibleRangeChanged(std::function<void(std::size_t, std::size_t)> onVisibleRangeChanged) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowIndex(ui::Widget& row, std::size_t viewRow) override;
    /// The terminal measures in cells: a unit, and a monospace character, is one cell; a column takes its gap after it
    /// beyond its content, as an unwindowed column does; a column that declares no width starts at twelve; a column
    /// that shrinks goes down to a few cells and its gap.
    [[nodiscard]] ui::TableMeasure tableMeasure() const override;
    void setStatus(ui::TableStatus const& status) override;
    void setOnRetry(std::function<void()> onRetry) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setCellLoad(ui::Widget& row, std::size_t column, ui::CellLoad load) override;
    /// A tone tints the cell's text in the tone's style, `invalid` draws the error glyph in place of the text, and a
    /// tooltip is shown in the status line while the cell has the focus. Throws std::logic_error when @p row is not
    /// one of this table's rows.
    void setCellAccent(ui::Widget& row, std::size_t column, ui::CellAccent const& accent) override;
    void setColumnWindow(std::span<std::size_t const> columns, std::span<int const> offsets,
                         std::span<int const> widths, int totalWidth, std::size_t pinned) override;
    void setOnHorizontalRange(std::function<void(int scrollX, int viewport)> handler) override;
    /// Reports the widths of the headers and of the shown cells, with the gap after the column, when one grew since it
    /// was last reported, so the mount can fit the columns to the table.
    void setOnColumnMeasured(
        std::function<void(std::size_t column, ui::ColumnMeasure const& measure)> handler) override;
    void setHorizontalOffset(int scrollX) override;
    void setFocusCell(std::size_t viewRow, std::size_t column) override;
    void setOnFocusMove(std::function<void(ui::FocusMove)> handler) override;
    /// Printable text typed on a focused cell of a table over a source, with no chord of the table's for the key.
    void setOnText(std::function<void(std::string)> onText) override;
    void setOnCellClick(
        std::function<void(std::size_t viewRow, std::size_t column, bool doubleClick)> onClick) override;
    /// Reported before the frame after the one that found the focus outside the table and everything in it.
    void setOnFocusOut(std::function<void()> onFocusOut) override;
    /// The editor takes the cell's slot and the cell none; an editor that had the focus gives it back to the table
    /// when it goes. Throws std::logic_error when @p row is not one of this table's rows.
    void setCellEditor(ui::Widget& row, std::size_t column, ui::Widget* editor) override;
    /// An error mark draws `!` and a modified one `+` at the cell's trailing edge. Throws std::logic_error when @p row
    /// is not one of this table's rows.
    void setCellMarks(ui::Widget& row, std::size_t column, ui::CellMarks const& marks) override;
    /// A modified row draws `+` in its selection mark's cell while it is not selected. Throws std::logic_error when
    /// @p row is not one of this table's rows.
    void setRowModified(ui::Widget& row, bool modified) override;
    /// The rows a rectangle of cells spans draw `*` in the gutter, as selected rows do; the terminal draws no column
    /// of it.
    void setCellSelection(std::optional<std::array<std::size_t, 4>> rect) override;
    /// In cell mode a click in a row's gutter selects the row's cells, a Shift-click the cells from the focused one to
    /// the one clicked, and a Ctrl-click on a column's header the column's cells.
    void setOnCellSelect(std::function<void(std::size_t anchorRow, std::size_t anchorPosition, std::size_t focusRow,
                                            std::size_t focusPosition)>
                             onSelect) override;
    /// Over a source, Shift with Up or Down, a Shift-click and Ctrl+A (and the header box) go to the handler as view
    /// rows; the mount resolves them over rows not realized.
    void setOnSelectRange(
        std::function<void(std::size_t anchorRow, std::size_t focusRow, ui::SelectionChange)> handler) override;
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
    [[nodiscard]] bool contextPress(::core::tui::Point cell) override;
    /// The cells of the header line a column's label is drawn in, relative to the table, the filter button left out;
    /// none while the column's header is not drawn.
    [[nodiscard]] std::optional<::core::tui::Rect> headerLabelArea(std::size_t column) const;
    /// The cell of a group's filter button on the group line, relative to the table; none while it is not drawn.
    [[nodiscard]] std::optional<int> groupFilterX(std::size_t group) const;
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
        std::size_t columns = 1;  ///< How many columns it covers from `column`, its own included.
        std::size_t first = 0;    ///< The origin's place among the shown rows.
        std::size_t last = 0;     ///< The last covered row's place.
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
    /// The cells a column's header needs: its label with its unit, room for the sort mark when it is sortable, and
    /// its filter button when it is filterable.
    [[nodiscard]] int headerWidth(ui::TableColumn const& column) const;
    /// The lines above the rows: none without a header, else the header line, under the group line when there are
    /// groups.
    [[nodiscard]] int headerLines() const noexcept;
    /// The lines the bands take: one per level.
    [[nodiscard]] int bandLines() const noexcept;
    /// The sort chain's place of @p index, from 0, or none.
    [[nodiscard]] std::optional<std::size_t> chainPlace(std::size_t index) const;
    /// Acts on a click on the header line at column @p x: the `⋯`, a filter button, or a sortable column's header;
    /// in cell mode a Ctrl-click on a column's header selects its cells.
    void clickHeader(int x, ::core::tui::Modifier modifiers);
    /// In cell mode, a click in @p row's gutter or a Shift-click on one of its cells, reported as a rectangle; whether
    /// the click was one of those.
    [[nodiscard]] bool selectCellsAt(WidgetBase const* row, ::core::tui::Point cell, ::core::tui::Modifier held);
    /// A column's place among the visible columns, or none while it is hidden.
    [[nodiscard]] std::optional<std::size_t> visiblePlace(std::size_t column) const;
    /// Acts on a click on the group line at column @p x: a group's filter button.
    void clickGroups(int x);
    /// Draws the group line above the headers.
    void paintGroups(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& columns) const;
    /// Draws each band run on its level's line, over the drawn columns at the visible places it names.
    void paintBands(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& columns) const;
    /// The keys of a focused header; Ignored for any other key, which then goes on outwards.
    [[nodiscard]] ::core::tui::EventResult headerKey(::core::tui::KeyEvent const& key);
    /// Focuses the header of the chain's first key, or the first visible column.
    void focusHeader();
    /// Opens the menu of @p column's header, or the table's menu, below its place on the header line.
    void openMenu(std::optional<std::size_t> column);
    /// Opens the menu of @p row's cell in @p column below the cell, after making the row the selection alone when it
    /// is not selected.
    void openCellMenu(WidgetBase const& row, std::size_t column);
    /// The menu key on the focused cell of a table over a source, and the chords of the focused cell's entries;
    /// Ignored for any other key.
    [[nodiscard]] ::core::tui::EventResult cellKey(::core::tui::KeyEvent const& key);
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
    /// Whether a row is never selected, activated or the cursor's: a heading or a summary row.
    [[nodiscard]] bool isPassive(WidgetBase const* row) const {
        return _headings.contains(row) || _summaries.contains(row);
    }
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
    /// The load state of @p row's cell in @p column; a loaded cell whose value could not be read is `Failed`, so it
    /// draws the error glyph in place of its text.
    [[nodiscard]] ui::CellLoad loadOf(WidgetBase const* row, std::size_t column) const;
    /// The tooltip of the focused cell; empty when it has none.
    [[nodiscard]] std::string focusedTooltip() const;
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
    /// The columns whose measure grew past what was last reported, with the measure to report.
    [[nodiscard]] std::vector<std::pair<std::size_t, ui::ColumnMeasure>> grownMeasures() const;
    /// Draws the status line on the last line.
    void paintStatus(::core::tui::Canvas& canvas);
    /// Draws the marks of the cells that are not loaded, in their rows' @p areas.
    void paintLoads(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& drawn,
                    std::vector<::core::tui::Rect> const& areas) const;
    /// Draws the marks of the cells that have some, in their rows' @p areas.
    void paintMarks(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& drawn,
                    std::vector<::core::tui::Rect> const& areas) const;
    /// Calls onCellClick for a click at column @p x of @p row, a second time for a double click; whether it did.
    bool clickCell(WidgetBase const* row, int x, bool twice);
    /// The realized row at view row @p viewRow, or null.
    [[nodiscard]] WidgetBase* rowAtView(std::size_t viewRow) const;
    /// Calls onSelectRange with a range of view rows in `Multiple` mode; whether it was called.
    bool selectRange(std::size_t anchorRow, std::size_t focusRow, ui::SelectionChange change);
    /// The keys of a table over a source: moves and ranges through the mount, Space and Enter on the focused row;
    /// Ignored for any other key.
    [[nodiscard]] ::core::tui::EventResult sourceKey(::core::tui::KeyEvent const& key);
    /// The move a key asks of the mount in a table over a source, or none.
    [[nodiscard]] static std::optional<ui::FocusMove> focusMoveOf(::core::tui::KeyEvent const& key);

    std::vector<ui::TableColumn> _columns;
    bool _showHeader = true;
    bool _selectColumn = false;
    ui::SelectionMode _mode = ui::SelectionMode::None;
    std::unordered_map<WidgetBase const*, ui::Key> _keys;
    std::unordered_set<WidgetBase const*> _headings;
    std::unordered_set<WidgetBase const*> _summaries;
    // The marks of each row that has any, and whether the gutter holds a cell for a busy or locked glyph.
    std::unordered_map<WidgetBase const*, ui::RowMarks> _marks;
    bool _markGutter = false;
    std::unordered_map<WidgetBase const*, std::vector<ui::RowSpan>> _spans;  ///< Each row's effective spans.
    std::vector<SpanPlace> _plan;                                            ///< The spanning cells, as last planned.
    std::unordered_map<WidgetBase const*, int> _extraLines;  ///< The lines a spanning cell adds to its last row.
    std::vector<ui::Key> _selection;  ///< The keys requested or selected by the user, in that order.
    /// Engaged while the selection is of the view: every row but these keys is marked, until the next `setSelection`.
    std::optional<std::vector<ui::Key>> _viewExcept;
    WidgetBase* _cursor = nullptr;   ///< The cursor row; null for the first shown row.
    std::optional<ui::Key> _anchor;  ///< Where a range starts: the row last clicked or toggled.
    std::size_t _cursorPlace = 0;    ///< The cursor's position among the shown rows, when last known.
    std::size_t _top = 0;            ///< The first shown row drawn.
    ::core::tui::Size _size{};       ///< The table's size, as of the last frame.
    int _rowRoom = 0;                ///< Lines below the header, as of the last frame.
    bool _follow = true;             ///< Whether the next frame brings the cursor row into view.
    /// The focused view the table last looked at, compared by address only: it may be gone by the next frame.
    ::core::tui::Component const* _focusSeen = nullptr;

    /// The first click of a possible double click.
    struct LastClick {
        WidgetBase const* row = nullptr;           ///< The row clicked; null for none.
        std::chrono::steady_clock::time_point at;  ///< When.
    };
    LastClick _lastClick;
    std::function<void(std::vector<ui::Key>, ui::SelectionChange)> _onSelectionChange;
    /// Ctrl+A's and the header box's handler in place of selecting the rows held; empty for none.
    std::function<void()> _onSelectAll;
    std::function<void(ui::Key)> _onActivate;
    std::vector<ui::TableSort> _chain;
    std::function<void(std::size_t, ui::HeaderClick)> _onSort;
    std::function<std::vector<ui::MenuEntry>(std::optional<std::size_t>)> _menu;
    std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> _onMenu;
    std::function<std::vector<ui::MenuEntry>(ui::Key const&, std::size_t)> _cellMenu;
    std::function<void(ui::Key const&, std::size_t, std::vector<std::size_t> const&)> _cellChosen;
    std::vector<std::string> _cellMenuKeys;  ///< The chords that ask for the focused cell's entries.
    std::vector<std::size_t> _visible;  ///< The visible columns, as `setVisibleColumns` told them; empty: every one.
    std::function<void(std::size_t, std::size_t)> _onColumnMove;
    std::function<void(std::size_t)> _onFilter;
    std::vector<std::size_t> _filtered;
    std::vector<ui::TableColumnGroup> _groups;
    std::vector<ui::BandRun> _bands;
    std::function<void(std::size_t)> _onGroupFilter;
    std::function<void()> _onCopy;
    std::optional<std::size_t> _headerFocus;  ///< The focused header's column; none while the rows have the focus.
    MenuPopup _menuPopup{*this};

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
    /// Each row's cells' accents, by column; a cell with none has no entry.
    std::unordered_map<WidgetBase const*, std::unordered_map<std::size_t, ui::CellAccent>> _accents;
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
    std::function<void(std::size_t, ui::ColumnMeasure const&)> _onColumnMeasured;
    std::vector<ui::ColumnMeasure> _reportedMeasures;
    std::function<void(ui::FocusMove)> _onFocusMove;
    std::function<void(std::string)> _onText;
    std::function<void(std::size_t, std::size_t, bool)> _onCellClick;
    std::function<void()> _onFocusOut;
    bool _focusWithin = false;  ///< Whether the focus was in the table at the last frame.
    bool _focusLeft = false;    ///< Whether the next fit reports that the focus left the table.
    /// Each row's open cell editor and the column it is drawn over.
    std::unordered_map<WidgetBase const*, std::pair<std::size_t, WidgetBase*>> _editors;
    /// Each row's cells' marks, by column; a cell with none has no entry.
    std::unordered_map<WidgetBase const*, std::unordered_map<std::size_t, ui::CellMarks>> _cellMarks;
    std::unordered_set<WidgetBase const*> _modifiedRows;
    std::function<void(std::size_t, std::size_t, ui::SelectionChange)> _onSelectRange;
    std::optional<std::size_t> _anchorRow;  ///< Over a source, where a range starts: a view row.
    std::optional<std::pair<std::size_t, std::size_t>> _focusCell;  ///< The focused cell's view row and column.
    /// In cell mode, what a gesture that selects cells reports; empty selects rows.
    std::function<void(std::size_t, std::size_t, std::size_t, std::size_t)> _onCellSelect;
    /// The rectangle of selected cells, `{firstRow, rowCount, firstPosition, positionCount}`; none while empty.
    std::optional<std::array<std::size_t, 4>> _cellRect;
    ui::TableStatus _status;
    std::function<void()> _onRetry;
    int _retryX = -1;  ///< Where `[Retry]` starts on the status line; -1 while it is not drawn.
    std::function<void(std::string)> _onStatusEntry;
    /// The status line's entries as drawn: where each starts, its width and its id; the n-th is chosen by `Alt+<n>`.
    struct DrawnEntry {
        int x = 0;
        int width = 0;
        std::string entryId;
    };
    std::vector<DrawnEntry> _entriesDrawn;
};

}  // namespace morph::tui::detail
