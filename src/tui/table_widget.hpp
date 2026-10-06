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
#include <unordered_map>
#include <vector>

#include "tui/context.hpp"
#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Table: a header row above keyed rows. Each row is a horizontal stack of cells laid out in the column widths the
/// solver gives the table, one cell gap apart; a hidden cell keeps its column, and counts toward its width, so the
/// cells after it stay under their own headers. A two-cell gutter marks selected rows `*` and, while the table has
/// focus, the cursor row `>`.
///
/// Up, Down, Home and End move the cursor, and PageUp and PageDown by as many rows as fill the room below the header;
/// Space selects the cursor row (Single) or toggles it (Multiple); Enter selects it in Single mode and activates it in
/// every mode. A click moves the cursor to a row and selects or toggles it as Space does; a second click on the same
/// row within half a second (on `Context::now`), with no key between, is a double click, which activates the row and
/// leaves the selection as the first click left it. A row the user cannot reach (disabled) is neither selected nor
/// activated.
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
class TableImpl final : public TuiContainer<ui::TableWidget> {
public:
    explicit TableImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setColumns(std::vector<ui::TableColumn> const& columns) override;
    void setSelectionMode(ui::SelectionMode mode) override;
    /// Throws std::logic_error when @p row is not one of this table's rows.
    void setRowKey(ui::Widget& row, ui::Key const& key) override;
    void setSelection(std::vector<ui::Key> const& selection) override;
    void setOnSelectionChange(std::function<void(std::vector<ui::Key>)> onSelectionChange) override;
    void setOnActivate(std::function<void(ui::Key)> onActivate) override;
    /// The positions among `children()` of the rows the table marks as selected, ascending.
    [[nodiscard]] std::vector<std::size_t> markedRows() const;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return true; }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void click(::core::tui::Point cell) override;
    [[nodiscard]] bool wheel(int delta) override;

protected:
    void childForgotten(WidgetBase& child) override;

private:
    /// Each column's content width: its label's, or its widest cell's, hidden cells included.
    [[nodiscard]] std::vector<int> naturalWidths() const;
    /// The solved column widths for a table @p width cells wide.
    [[nodiscard]] std::vector<int> columnWidths(int width) const;
    [[nodiscard]] std::optional<ui::Key> keyOf(WidgetBase const* row) const;
    /// The row with @p key, or null.
    [[nodiscard]] WidgetBase* rowWith(ui::Key const& key) const;
    [[nodiscard]] bool isMarked(WidgetBase const* row) const;
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
    /// Calls onSelectionChange with @p keys; whether this table is still there afterwards.
    [[nodiscard]] bool report(std::vector<ui::Key> keys);
    /// Calls onActivate with @p key when a row the user can reach has it; the handler may destroy this table.
    void activateRow(ui::Key const& key);

    std::vector<ui::TableColumn> _columns;
    ui::SelectionMode _mode = ui::SelectionMode::None;
    std::unordered_map<WidgetBase const*, ui::Key> _keys;
    std::vector<ui::Key> _selection;  ///< The keys requested or selected by the user, in that order.
    WidgetBase* _cursor = nullptr;    ///< The cursor row; null for the first shown row.
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
    std::function<void(std::vector<ui::Key>)> _onSelectionChange;
    std::function<void(ui::Key)> _onActivate;
};

}  // namespace morph::tui::detail
