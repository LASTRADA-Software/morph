// SPDX-License-Identifier: Apache-2.0

#include "tui/table_widget.hpp"

#include <algorithm>
#include <array>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Theme.hpp>
#include <iterator>
#include <limits>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

#include "tui/chord.hpp"
#include "tui/container_widgets.hpp"
#include "tui/display_widgets.hpp"
#include "tui/layout.hpp"
#include "tui/leaf_widgets.hpp"

namespace morph::tui::detail {

using ::core::tui::EventResult;
using ::core::tui::KeyCode;

namespace {

constexpr int kGutter = 2;
/// The gutter with a select column: a check box, then the cursor mark and a space.
constexpr int kSelectGutter = 5;
/// The cells of a select column's box.
constexpr int kBoxWidth = 3;
constexpr int kColumnGap = 1;
/// The cells of a sort mark after a header label: a space and an arrow.
constexpr int kSortMark = 2;
/// The width a column that may shrink gives up to: room for a few characters and an ellipsis.
constexpr int kShrunkColumn = 4;
/// How soon a second click on a row must follow the first to make a double click.
constexpr auto kDoubleClick = std::chrono::milliseconds{500};
/// What a cell whose level has not loaded draws, and one whose level failed to.
constexpr std::string_view kUnloadedMark = "···";
constexpr std::string_view kFailedMark = "!";
constexpr std::string_view kRetry = "[Retry]";
/// What separates the parts of the status line.
constexpr std::string_view kStatusGap = "  ";

/// Whether @p key is the special key @p code, not a typed character whose codepoint has the same value.
bool isKey(::core::tui::KeyEvent const& key, KeyCode code) noexcept { return key.codepoint == 0 && key.key == code; }

/// The lines a row takes: its natural height, and a line even when it has nothing to show.
int rowHeight(WidgetBase const& row) { return std::max(1, row.naturalSize().height); }

}  // namespace

void TableImpl::setColumns(std::vector<ui::TableColumn> const& columns) {
    _columns = columns;
    refresh();
}

void TableImpl::setShowHeader(bool shown) {
    _showHeader = shown;
    refresh();
}

void TableImpl::setSelectionMode(ui::SelectionMode mode) {
    _mode = mode;
    refresh();
}

void TableImpl::setRowKey(ui::Widget& row, ui::Key const& key) {
    auto& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setRowKey of a widget that is not this table's row"};
    }
    _keys.insert_or_assign(&base, key);
    refresh();
}

void TableImpl::setRowTone(ui::Widget& row, std::optional<ui::Tone> tone) {
    auto& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setRowTone of a widget that is not this table's row"};
    }
    base.applyRowTone(tone);
    refresh();
}

void TableImpl::setSelectColumn(bool shown) {
    _selectColumn = shown;
    refresh();
}

// A terminal has no accessibility tree to name the box or a row in.
void TableImpl::setSelectAllLabel(std::string_view label) { static_cast<void>(label); }

void TableImpl::setRowLabel(ui::Widget& row, std::string_view label) {
    static_cast<void>(label);
    if (WidgetBase::of(row).container() != this) {
        throw std::logic_error{"morph::tui: setRowLabel of a widget that is not this table's row"};
    }
}

void TableImpl::setRowHeading(ui::Widget& row) {
    auto& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setRowHeading of a widget that is not this table's row"};
    }
    _headings.insert(&base);
    refresh();
}

void TableImpl::setRowSpans(ui::Widget& row, std::vector<ui::RowSpan> const& spans) {
    auto& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setRowSpans of a widget that is not this table's row"};
    }
    if (spans.empty()) {
        _spans.erase(&base);
    } else {
        _spans.insert_or_assign(&base, spans);
        if (std::ranges::find(context().spanTables, this) == context().spanTables.end()) {
            context().spanTables.push_back(this);
        }
    }
    refresh();
}

int TableImpl::heightOf(WidgetBase const& row) const {
    if (_source) {
        return 1;
    }
    auto const extra = _extraLines.find(&row);
    return rowHeight(row) + (extra == _extraLines.end() ? 0 : extra->second);
}

// The rows' order decides coverage, so it is worked out here, from the rows as they are, and never when a row is told
// its spans: rows moving under an unchanged span change what is covered.
void TableImpl::planSpans() {
    if (_spans.empty() && _plan.empty()) {
        std::erase(context().spanTables, this);
        return;  // nothing spans and nothing is lifted: every cell is in its row already
    }
    auto const rows = shownChildren();
    auto const cellsOf = [](WidgetBase* row) {
        auto* const stack = dynamic_cast<ContainerBase*>(row);
        return stack == nullptr ? std::vector<WidgetBase*>{}
                                : std::vector<WidgetBase*>{stack->children().begin(), stack->children().end()};
    };
    std::vector<SpanPlace> plan;
    std::unordered_set<WidgetBase const*> covered;
    std::vector<std::size_t> coveredUntil(_columns.size(), 0);
    for (std::size_t index = 0; index < rows.size() && !_spans.empty(); ++index) {
        auto const found = _spans.find(rows.at(index));
        if (found == _spans.end() || isHeading(rows.at(index))) {
            continue;
        }
        auto const cells = cellsOf(rows.at(index));
        for (ui::RowSpan const& span : found->second) {
            if (span.column >= _columns.size() || span.column >= cells.size() || span.rows < 2 ||
                coveredUntil.at(span.column) > index) {
                continue;
            }
            std::size_t last = std::min(index + span.rows, rows.size()) - 1;
            for (std::size_t between = index + 1; between <= last; ++between) {
                if (isHeading(rows.at(between))) {
                    last = between - 1;
                    break;
                }
            }
            if (last == index) {
                continue;
            }
            coveredUntil.at(span.column) = last + 1;
            plan.push_back({.origin = rows.at(index),
                            .cell = cells.at(span.column),
                            .column = span.column,
                            .first = index,
                            .last = last});
            for (std::size_t between = index + 1; between <= last; ++between) {
                auto const coveredCells = cellsOf(rows.at(between));
                if (span.column < coveredCells.size()) {
                    covered.insert(coveredCells.at(span.column));
                }
            }
        }
    }
    // Lift the spanning cells and hide the covered ones; every other cell goes back to its row.
    std::unordered_set<WidgetBase const*> lifted;
    for (SpanPlace const& place : plan) {
        lifted.insert(place.cell);
    }
    for (auto* const row : children()) {
        for (auto* const cell : cellsOf(row)) {
            bool const lift = lifted.contains(cell);
            cell->setLifted(lift);
            ::core::tui::Component& home = lift ? view() : row->view();
            if (cell->view().parent() != &home) {
                home.addChild(cell->view(),
                              ::core::tui::LayoutParams{.area = {}, .visible = cell->shown(), .zIndex = lift ? 1 : 0});
            }
            bool const hide = covered.contains(cell);
            if (cell->spanCovered() != hide) {
                cell->setSpanCovered(hide);
                cell->setStructuralVisible(!hide);
            }
        }
    }
    // A spanning cell taller than the rows it covers adds the shortfall to the last of them.
    std::unordered_map<WidgetBase const*, int> extra;
    for (SpanPlace const& place : plan) {
        int block = 0;
        for (std::size_t row = place.first; row <= place.last; ++row) {
            auto const added = extra.find(rows.at(row));
            block += rowHeight(*rows.at(row)) + (added == extra.end() ? 0 : added->second);
        }
        int const need = requestedSize(*place.cell).height;
        if (need > block) {
            extra[rows.at(place.last)] += need - block;
        }
    }
    bool const changed = extra != _extraLines;
    _plan = std::move(plan);
    _extraLines = std::move(extra);
    if (changed) {
        refresh();
    }
}

std::vector<::core::tui::Rect> TableImpl::spanAreas(::core::tui::Size size,
                                                    std::vector<::core::tui::Rect>* blocks) const {
    std::vector<::core::tui::Rect> areas(_plan.size());
    std::vector<::core::tui::Rect> unused;
    std::vector<::core::tui::Rect>& block = blocks != nullptr ? *blocks : unused;
    block.assign(_plan.size(), {});
    auto const rows = shownChildren();
    auto const widths = columnWidths(size.width);
    int const header = _showHeader ? 1 : 0;
    // The line of each row as if every row were drawn: the first one drawn starts under the header, the ones above it
    // before that.
    std::vector<int> start(rows.size(), 0);
    int line = header;
    for (std::size_t index = std::min(_top, rows.size()); index < rows.size(); ++index) {
        start.at(index) = line;
        line += heightOf(*rows.at(index));
    }
    line = header;
    for (std::size_t index = std::min(_top, rows.size()); index > 0; --index) {
        line -= heightOf(*rows.at(index - 1));
        start.at(index - 1) = line;
    }
    for (auto const [place, area, lines] : std::views::zip(_plan, areas, block)) {
        if (place.column >= widths.size() || place.last >= rows.size()) {
            continue;
        }
        int const top = std::max(start.at(place.first), header);
        int const bottom = std::min(start.at(place.last) + heightOf(*rows.at(place.last)), size.height);
        if (bottom <= top) {
            continue;
        }
        int left = gutter();
        for (std::size_t index = 0; index < place.column; ++index) {
            left += widths.at(index) + kColumnGap;
        }
        // Placed down the lines it covers by its column's `valign`, as a cell is down its row.
        ui::Align const down = _columns.at(place.column).valign;
        int const space = bottom - top;
        int const height = down == ui::Align::Stretch ? space : std::min(requestedSize(*place.cell).height, space);
        area = {.x = left,
                .y = top + alignedOffset(down, height, space),
                .width = widths.at(place.column),
                .height = height};
        lines = {.x = left, .y = top, .width = widths.at(place.column), .height = space};
    }
    return areas;
}

WidgetBase* TableImpl::spanOriginAt(::core::tui::Point cell) const {
    // The whole block the cell stands for, not the part its content is drawn on: a click on a blank line of it, under
    // a centred or top-aligned cell, is still a click on the span.
    std::vector<::core::tui::Rect> blocks;
    static_cast<void>(spanAreas(_size, &blocks));
    for (auto const [place, block] : std::views::zip(_plan, blocks)) {
        if (!block.empty() && cell.x >= block.x && cell.x < block.x + block.width && cell.y >= block.y &&
            cell.y < block.y + block.height) {
            return place.origin;
        }
    }
    return nullptr;
}

std::size_t TableImpl::dataPlace(std::span<WidgetBase* const> rows, std::size_t place, Direction direction) const {
    auto const data = [&](std::size_t candidate) {
        return candidate < rows.size() && !isHeading(rows.subspan(candidate).front());
    };
    for (std::size_t index = place; index < rows.size(); direction == Direction::Forward ? ++index : --index) {
        if (data(index)) {
            return index;
        }
        if (direction == Direction::Backward && index == 0) {
            break;
        }
    }
    Direction const other = direction == Direction::Forward ? Direction::Backward : Direction::Forward;
    for (std::size_t index = place; index < rows.size(); other == Direction::Forward ? ++index : --index) {
        if (data(index)) {
            return index;
        }
        if (other == Direction::Backward && index == 0) {
            break;
        }
    }
    return place;
}

int TableImpl::gutter() const noexcept { return _selectColumn ? kSelectGutter : kGutter; }

std::string TableImpl::headerBox() const {
    auto const rows = shownChildren();
    auto const data = std::ranges::count_if(rows, [this](WidgetBase const* row) { return !isHeading(row); });
    auto const marked = std::ranges::count_if(rows, [this](WidgetBase const* row) { return isMarked(row); });
    if (marked == 0) {
        return "[ ]";
    }
    return marked == data ? "[x]" : "[-]";
}

void TableImpl::setSelection(std::vector<ui::Key> const& selection) {
    _selection = selection;
    refresh();
}

void TableImpl::setOnSelectionChange(
    std::function<void(std::vector<ui::Key>, ui::SelectionChange)> onSelectionChange) {
    _onSelectionChange = std::move(onSelectionChange);
}

void TableImpl::setOnActivate(std::function<void(ui::Key)> onActivate) { _onActivate = std::move(onActivate); }

void TableImpl::setSort(std::optional<ui::TableSort> sort) {
    _sort = sort;
    refresh();
}

void TableImpl::setOnSort(std::function<void(std::size_t)> onSort) { _onSort = std::move(onSort); }

std::string TableImpl::headerText(std::size_t index) const {
    std::string text = _columns.at(index).label;
    if (_sort && _sort->column == index) {
        text += _sort->direction == ui::SortDirection::Ascending ? " ▲" : " ▼";
    }
    return text;
}

int TableImpl::headerWidth(ui::TableColumn const& column) {
    return displayWidth(column.label) + (column.sortable ? kSortMark : 0);
}

void TableImpl::clickHeader(int x) {
    for (DrawnColumn const& drawn : drawnColumns(_size.width)) {
        int const left = gutter() + drawn.x;
        if (x >= left && x < left + drawn.width) {
            auto const handler = _onSort;
            if (_columns.at(drawn.column).sortable && handler) {
                handler(drawn.column);
            }
            return;
        }
    }
}

// The cursor goes to the row that took the removed one's place, or to the last row.
void TableImpl::childForgotten(WidgetBase& child) {
    _keys.erase(&child);
    _headings.erase(&child);
    _spans.erase(&child);
    _extraLines.erase(&child);
    _viewRows.erase(&child);
    _cellColumns.erase(&child);
    _loads.erase(&child);
    // The plan points at rows and at the cells lifted out of them, and is rebuilt only by the next fit: until then a
    // click would hit-test a block of a row that is gone, and read its freed cell.
    _plan.clear();
    if (_lastClick.row == &child) {
        _lastClick = {};
    }
    if (_cursor == &child) {
        auto const rows = shownChildren();
        _cursor = rows.empty() ? nullptr : rows.at(std::min(_cursorPlace, rows.size() - 1));
        _follow = true;
    }
}

std::vector<std::size_t> TableImpl::markedRows() const {
    std::vector<std::size_t> marked;
    std::size_t index = 0;
    for (auto const* row : children()) {
        if (isMarked(row)) {
            marked.push_back(index);
        }
        ++index;
    }
    return marked;
}

std::vector<int> TableImpl::naturalWidths() const {
    std::vector<int> widths;
    widths.reserve(_columns.size());
    for (auto const& column : _columns) {
        widths.push_back(headerWidth(column));
    }
    for (auto const* row : children()) {
        auto const* const cells = dynamic_cast<ContainerBase const*>(row);
        // A heading's one cell spans every column, so it widens none.
        if (cells == nullptr || !row->shown() || isHeading(row)) {
            continue;
        }
        std::size_t position = 0;
        for (auto const* const cell : cells->children()) {
            auto const column = columnOf(row, cell, position++);
            // A hidden cell is drawn as nothing, and a mark in place of a cell widens nothing, so neither asks for
            // room.
            if (column && *column < widths.size() && cell->shown() && loadOf(row, *column) == ui::CellLoad::Loaded) {
                widths.at(*column) = std::max(widths.at(*column), requestedSize(*cell).width);
            }
        }
    }
    return widths;
}

std::vector<int> TableImpl::leastWidths() const {
    std::vector<int> widths = naturalWidths();
    for (auto const [width, column] : std::views::zip(widths, _columns)) {
        if (column.wrap) {
            width = headerWidth(column);
        }
    }
    for (auto const* row : children()) {
        auto const* const cells = dynamic_cast<ContainerBase const*>(row);
        if (cells == nullptr || !row->shown() || isHeading(row)) {
            continue;
        }
        std::size_t position = 0;
        for (auto const* const cell : cells->children()) {
            auto const column = columnOf(row, cell, position++);
            if (!column || *column >= widths.size() || !_columns.at(*column).wrap || !cell->shown() ||
                loadOf(row, *column) != ui::CellLoad::Loaded) {
                continue;
            }
            auto const* const text = dynamic_cast<TextImpl const*>(cell);
            widths.at(*column) =
                std::max(widths.at(*column), text != nullptr ? text->leastWidth() : requestedSize(*cell).width);
        }
    }
    return widths;
}

std::vector<int> TableImpl::columnWidths(int width) const {
    std::vector<layout::ColumnItem> items;
    items.reserve(_columns.size());
    auto const naturals = naturalWidths();
    auto const leasts = leastWidths();
    for (auto const [column, natural, least] : std::views::zip(_columns, naturals, leasts)) {
        // No column is squeezed below its header, so none is drawn at width 0; a fixed column narrower than its
        // header keeps the width the author gave it, and a column that may shrink goes down to `kShrunkColumn`. A
        // table whose columns still exceed its width is cut at its edge.
        int const header = headerWidth(column);
        int const flexible = column.shrink ? std::min(header, kShrunkColumn) : header;
        int const minimum = column.width.kind == ui::Sizing::Kind::Fixed
                                ? std::min(header, std::max(0, column.width.amount))
                                : flexible;
        items.push_back(layout::ColumnItem{.sizing = column.width,
                                           .natural = natural,
                                           .least = least,
                                           .floor = minimum,
                                           .minimum = minimum,
                                           .shrinks = column.shrink});
    }
    return layout::planColumns(items, layout::Track{.length = std::max(0, width - gutter()), .gap = kColumnGap});
}

::core::tui::Size TableImpl::naturalSize() const {
    int width = gutter() + _totalWidth;
    if (!_arranged) {
        auto const widths = naturalWidths();
        width = gutter() + std::accumulate(widths.begin(), widths.end(), 0) +
                (kColumnGap * std::max(0, static_cast<int>(widths.size()) - 1));
    }
    std::int64_t height = (_showHeader ? 1 : 0) + (_status.shown ? 1 : 0);
    if (_source) {
        height += static_cast<std::int64_t>(_total);  // a line for each row of the view, realized or not
    } else {
        for (auto const* row : shownChildren()) {
            height += heightOf(*row);
        }
    }
    return {.width = width,
            .height = static_cast<int>(std::min<std::int64_t>(height, std::numeric_limits<int>::max()))};
}

std::optional<ui::Key> TableImpl::keyOf(WidgetBase const* row) const {
    auto const found = _keys.find(row);
    return found == _keys.end() ? std::nullopt : std::optional{found->second};
}

WidgetBase* TableImpl::rowWith(ui::Key const& key) const {
    auto const found = std::ranges::find_if(children(), [this, &key](WidgetBase const* row) {
        auto const rowKey = keyOf(row);
        return rowKey && *rowKey == key;
    });
    return found == children().end() ? nullptr : *found;
}

bool TableImpl::isMarked(WidgetBase const* row) const {
    if (isHeading(row)) {
        return false;
    }
    auto const rowKey = keyOf(row);
    return rowKey && std::ranges::find(_selection, *rowKey) != _selection.end();
}

std::size_t TableImpl::cursorPlace(std::span<WidgetBase* const> rows) const {
    if (rows.empty()) {
        return 0;
    }
    auto const found = std::ranges::find(rows, _cursor);
    if (found != rows.end()) {
        return static_cast<std::size_t>(std::distance(rows.begin(), found));
    }
    return dataPlace(rows, _cursor == nullptr ? 0 : std::min(_cursorPlace, rows.size() - 1), Direction::Forward);
}

// The cursor passes over a heading, on in the way it moved, or back when no data row is that way.
void TableImpl::moveCursor(std::span<WidgetBase* const> rows, std::size_t place) {
    if (place >= rows.size()) {
        return;
    }
    place = dataPlace(rows, place, place < _cursorPlace ? Direction::Backward : Direction::Forward);
    _cursor = rows.subspan(place).front();
    _cursorPlace = place;
    _follow = true;
}

std::size_t TableImpl::lastTop(std::span<WidgetBase* const> rows) const {
    std::size_t top = rows.size();
    int used = 0;
    for (auto const* row : rows | std::views::reverse) {
        used += heightOf(*row);
        if (used > _rowRoom) {
            break;
        }
        --top;
    }
    return rows.empty() ? 0 : std::min(top, rows.size() - 1);
}

// At least one row, so a row taller than the room still moves.
std::size_t TableImpl::pageFrom(std::span<WidgetBase* const> rows, std::size_t place, Direction direction) const {
    int used = 0;
    std::size_t target = place;
    while (direction == Direction::Forward ? target + 1 < rows.size() : target > 0) {
        auto const next = direction == Direction::Forward ? target + 1 : target - 1;
        used += heightOf(*rows.subspan(next).front());
        if (used > _rowRoom && target != place) {
            break;
        }
        target = next;
    }
    return target;
}

void TableImpl::followFocus(std::span<WidgetBase* const> rows) {
    auto const* const focused = context().screen->focusedComponent();
    if (focused == _focusSeen) {
        return;
    }
    _focusSeen = focused;
    if (focused == &view()) {
        _follow = true;
        return;
    }
    auto const holder =
        std::ranges::find_if(rows, [focused](WidgetBase const* row) { return isWithin(focused, row->view()); });
    if (holder != rows.end()) {
        moveCursor(rows, static_cast<std::size_t>(std::distance(rows.begin(), holder)));
    }
}

// A row taller than the room shows from its top.
void TableImpl::showCursor(std::span<WidgetBase* const> rows) {
    auto const place = cursorPlace(rows);
    if (rows.empty() || place < _top) {
        _top = place;
        return;
    }
    auto const shown = rows.subspan(_top, place - _top + 1);
    int used = std::accumulate(shown.begin(), shown.end(), 0,
                               [this](int sum, WidgetBase const* row) { return sum + heightOf(*row); });
    for (auto const* row : shown.first(shown.size() - 1)) {
        if (used <= _rowRoom) {
            return;
        }
        used -= heightOf(*row);
        ++_top;
    }
}

std::vector<::core::tui::Rect> TableImpl::childAreas(::core::tui::Size size) const {
    std::vector<::core::tui::Rect> areas(children().size());
    int line = _showHeader ? 1 : 0;
    if (_source) {
        // Each realized row on the line of its view index; the line below the rows is the status line's.
        int const room = rowRoomOf(size.height);
        for (auto const [row, area] : std::views::zip(children(), areas)) {
            auto const found = _viewRows.find(row);
            if (!row->shown() || found == _viewRows.end() || found->second < _top ||
                found->second - _top >= static_cast<std::size_t>(room)) {
                continue;
            }
            area = {.x = gutter(),
                    .y = line + static_cast<int>(found->second - _top),
                    .width = std::max(0, size.width - gutter()),
                    .height = 1};
        }
        return areas;
    }
    std::size_t place = 0;
    for (auto const [row, area] : std::views::zip(children(), areas)) {
        if (!row->shown()) {
            continue;
        }
        bool const above = place < _top;
        ++place;
        // The status line, while shown, is the last line: no row is drawn over it.
        if (above || line >= size.height - (_status.shown ? 1 : 0)) {
            continue;
        }
        int const height = heightOf(*row);
        area = {.x = gutter(), .y = line, .width = std::max(0, size.width - gutter()), .height = height};
        line += height;
    }
    return areas;
}

void TableImpl::paintHeader(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& columns) const {
    auto const& theme = canvas.theme();
    if (_selectColumn) {
        canvas.putString(0, 0, headerBox(), theme.textNormal);
    }
    for (DrawnColumn const& drawn : columns) {
        auto label = canvas.subcanvas({.x = gutter() + drawn.x, .y = 0, .width = drawn.width, .height = 1});
        // Over its cells: a number column's header ends where its digits do.
        std::string const text = headerText(drawn.column);
        label.putString(0, alignedOffset(_columns.at(drawn.column).align, displayWidth(text), drawn.width), text,
                        theme.textBold);
    }
}

std::string TableImpl::rowMark(WidgetBase const* row, bool cursor) const {
    // With a select column the box shows the mark; without, `*` does.
    std::string mark;
    if (isHeading(row)) {
        mark.assign(static_cast<std::size_t>(gutter()), ' ');
        return mark;
    }
    if (_selectColumn) {
        mark = isMarked(row) ? "[x]" : "[ ]";
    } else {
        mark = isMarked(row) ? "*" : " ";
    }
    mark += cursor ? ">" : " ";
    return mark;
}

// Hands each row the table's column layout; a heading's one cell takes every column's width and the gaps between them.
void TableImpl::arrangeRows(std::vector<int> const& widths) {
    std::vector<CellPlace> places;
    places.reserve(_columns.size());
    for (auto const& column : _columns) {
        places.push_back(CellPlace{.across = column.align, .down = column.valign});
    }
    // A heading's one cell takes every column's width and the gaps between them.
    int const across = std::accumulate(widths.begin(), widths.end(), 0) +
                       (kColumnGap * std::max(0, static_cast<int>(widths.size()) - 1));
    for (auto* row : children()) {
        if (auto* const stack = dynamic_cast<StackImpl*>(row)) {
            if (isHeading(row)) {
                stack->setColumnLayout({across}, kColumnGap, {CellPlace{.across = ui::Align::Start}});
            } else {
                stack->setColumnLayout(widths, kColumnGap, places);
                // The terminal cannot wrap, so a text cell a column cuts ends in an ellipsis: on one line in a
                // column that does not wrap, on each line in one that does.
                for (auto const [column, cell] : std::views::zip(_columns, stack->children())) {
                    if (auto* const text = dynamic_cast<TextImpl*>(cell)) {
                        text->setElided(column.wrap ? TextImpl::Elision::PerLine : TextImpl::Elision::OneLine);
                    } else if (auto* const badge = dynamic_cast<BadgeImpl*>(cell)) {
                        badge->setElided(true);
                    }
                }
            }
        }
    }
}

void TableImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    _size = canvas.size();
    int const room = rowRoomOf(canvas.height());
    if (_source) {
        _rowRoom = room;
        clampView();
        reportBeforeNextFrame();
    }
    auto const drawn = drawnColumns(canvas.width());

    if (_showHeader) {
        paintHeader(canvas, drawn);
    }

    auto const rows = shownChildren();
    if (!_source) {
        if (room != _rowRoom) {
            _rowRoom = room;
            _follow = true;
        }
        followFocus(rows);
        if (_follow) {
            _follow = false;
            showCursor(rows);
        }
        _top = std::min(_top, lastTop(rows));
        _cursorPlace = cursorPlace(rows);
    }

    planSpans();
    if (_source) {
        slotCells(drawn);
    } else {
        std::vector<int> widths;
        widths.reserve(drawn.size());
        for (DrawnColumn const& column : drawn) {
            widths.push_back(column.width);
        }
        arrangeRows(widths);
    }
    auto const areas = childAreas(_size);
    placeChildren(areas);
    for (auto const [place, area] : std::views::zip(_plan, spanAreas(_size))) {
        place.cell->place(area);
    }

    bool const focused = hasFocus();
    for (auto const [row, area] : std::views::zip(children(), areas)) {
        if (area.empty()) {
            continue;
        }
        bool cursor = false;
        if (_source) {
            auto const found = _viewRows.find(row);
            cursor = focused && _focusCell && found != _viewRows.end() && found->second == _focusCell->first;
        } else {
            cursor = focused && row == rows.at(_cursorPlace);
        }
        canvas.putString(area.y, 0, rowMark(row, cursor), cursor ? theme.listItemSelected : theme.textNormal);
    }
    if (_source) {
        paintLoads(canvas, drawn, areas);
    }
    paintStatus(canvas);
}

// Shift with an arrow moves as the arrow does and, in Multiple mode, selects from the anchor to the new cursor row;
// without an anchor, the range starts at the row the cursor leaves.
EventResult TableImpl::modifiedKey(::core::tui::KeyEvent const& key) {
    bool const upward = matchesChord("Shift+Up", key);
    if (upward || matchesChord("Shift+Down", key)) {
        auto const rows = shownChildren();
        auto const place = cursorPlace(rows);
        if (!rows.empty() && (!_anchor || rowWith(*_anchor) == nullptr)) {
            _anchor = keyOf(rows.at(place));
        }
        auto const last = rows.empty() ? std::size_t{0} : rows.size() - 1;
        std::size_t const above = place > 0 ? place - 1 : 0;
        moveCursor(rows, upward ? above : std::min(place + 1, last));
        refresh();
        if (_mode == ui::SelectionMode::Multiple && !rows.empty()) {
            auto const* const cursor = rows.at(cursorPlace(rows));
            if (cursor->actionable()) {
                static_cast<void>(change(rangeTo(*cursor), ui::SelectionChange::Replace));
            }
        }
        return EventResult::Handled;
    }
    if (matchesChord("Ctrl+A", key) && _mode == ui::SelectionMode::Multiple) {
        static_cast<void>(change(allOrNone(), ui::SelectionChange::Extend));
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

// Any key ends a double click: the click after it is a first click.
EventResult TableImpl::key(::core::tui::KeyEvent const& key) {
    _lastClick = {};
    if (_source) {
        if (auto const result = sourceKey(key); result == EventResult::Handled) {
            return result;
        }
        // A modified key the source path leaves goes on outwards: no range runs over the realized rows alone.
        if (::core::tui::withoutLockKeys(key.modifiers) != ::core::tui::Modifier::None) {
            return EventResult::Ignored;
        }
    }
    if (::core::tui::withoutLockKeys(key.modifiers) != ::core::tui::Modifier::None) {
        return modifiedKey(key);
    }
    auto const rows = shownChildren();
    auto const place = cursorPlace(rows);
    auto const last = rows.empty() ? std::size_t{0} : rows.size() - 1;
    if (isKey(key, KeyCode::Up)) {
        moveCursor(rows, place > 0 ? place - 1 : 0);
    } else if (isKey(key, KeyCode::Down)) {
        moveCursor(rows, std::min(place + 1, last));
    } else if (isKey(key, KeyCode::PageUp)) {
        moveCursor(rows, pageFrom(rows, place, Direction::Backward));
    } else if (isKey(key, KeyCode::PageDown)) {
        moveCursor(rows, pageFrom(rows, place, Direction::Forward));
    } else if (isKey(key, KeyCode::Home)) {
        moveCursor(rows, 0);
    } else if (isKey(key, KeyCode::End)) {
        moveCursor(rows, last);
    } else if (key.codepoint == U' ') {
        if (!rows.empty()) {
            static_cast<void>(pick(*rows.at(place)));
        }
        return EventResult::Handled;
    } else if (isKey(key, KeyCode::Enter)) {
        // The selection handler may destroy this table, or the row: the key is taken first, and the row it names
        // is looked up again before it is activated.
        if (auto const rowKey = rows.empty() ? std::nullopt : keyOf(rows.at(place))) {
            if (_mode != ui::SelectionMode::Single || pick(*rows.at(place))) {
                activateRow(*rowKey);
            }
        }
        return EventResult::Handled;
    } else {
        return EventResult::Ignored;
    }
    refresh();
    return EventResult::Handled;
}

WidgetBase* TableImpl::rowAtLine(int line) const {
    for (auto const [row, area] : std::views::zip(children(), childAreas(_size))) {
        if (!area.empty() && line >= area.y && line < area.y + area.height) {
            return row;
        }
    }
    return nullptr;
}

void TableImpl::clickWith(::core::tui::Point cell, ::core::tui::Modifier modifiers) {
    if (_status.shown && cell.y == _size.height - 1) {
        if (_retryX >= 0 && cell.x >= _retryX && cell.x < _retryX + displayWidth(kRetry) && _onRetry) {
            auto const handler = _onRetry;
            handler();
        }
        return;
    }
    bool const onBox = _selectColumn && cell.x < kBoxWidth;
    // The header box is Ctrl+A; it does nothing while the table shows no rows. Over a source it selects the view.
    if (onBox && _showHeader && cell.y == 0) {
        if (_source) {
            if (_total > 0) {
                static_cast<void>(selectRange(0, _total - 1, ui::SelectionChange::Extend));
            }
            return;
        }
        if (_mode == ui::SelectionMode::Multiple && !shownChildren().empty()) {
            static_cast<void>(change(allOrNone(), ui::SelectionChange::Extend));
        }
        return;
    }
    if (_showHeader && cell.y == 0) {
        clickHeader(cell.x);
        return;
    }
    // A spanning cell drawn over a covered row's line is its origin row's.
    auto* const spanning = spanOriginAt(cell);
    auto* const row = spanning != nullptr ? spanning : rowAtLine(cell.y);
    if (row == nullptr || isHeading(row)) {
        return;
    }
    auto const held = ::core::tui::withoutLockKeys(modifiers);
    ui::RowClick click = ui::RowClick::Only;
    // A row's box is a Ctrl-click on the row.
    if (onBox || ::core::tui::hasModifier(held, ::core::tui::Modifier::Ctrl)) {
        click = ui::RowClick::Toggle;
    } else if (::core::tui::hasModifier(held, ::core::tui::Modifier::Shift)) {
        click = ui::RowClick::Range;
    }
    // Over a source a Shift-click is a range through the mount, from the anchor; any other click makes the row the
    // anchor.
    if (auto const index = _viewRows.find(row); _source && index != _viewRows.end()) {
        if (click == ui::RowClick::Range &&
            selectRange(_anchorRow.value_or(index->second), index->second, ui::SelectionChange::Replace)) {
            return;
        }
        _anchorRow = index->second;
    }
    // Only plain clicks make a double click, so two quick Ctrl-clicks toggle a row twice.
    auto const now = context().now();
    bool const twice = click == ui::RowClick::Only && _lastClick.row == row && now - _lastClick.at <= kDoubleClick;
    _lastClick = (twice || click != ui::RowClick::Only) ? LastClick{} : LastClick{.row = row, .at = now};
    auto const rows = shownChildren();
    moveCursor(rows, static_cast<std::size_t>(std::distance(rows.begin(), std::ranges::find(rows, row))));
    refresh();
    if (!twice) {
        static_cast<void>(choose(*row, click));
    } else if (auto const rowKey = keyOf(row)) {
        activateRow(*rowKey);
    }
}

// A move the user asked for counts as having followed the cursor and the focus as they are now, so the next frame
// does not undo it when either moved since the last one.
bool TableImpl::wheel(int delta) {
    if (_source) {
        std::size_t const before = _top;
        _top = delta < 0 ? _top - std::min(_top, static_cast<std::size_t>(-static_cast<std::int64_t>(delta)))
                         : _top + static_cast<std::size_t>(delta);
        clampView();
        if (_top == before) {
            return false;
        }
        reportRanges();
        refresh();
        return true;
    }
    auto const rows = shownChildren();
    auto const target = delta < 0 ? _top - std::min(_top, static_cast<std::size_t>(-delta))
                                  : std::min(_top + static_cast<std::size_t>(delta), lastTop(rows));
    if (target == _top) {
        return false;
    }
    _top = target;
    _follow = false;
    _focusSeen = context().screen->focusedComponent();
    refresh();
    return true;
}

bool TableImpl::pick(WidgetBase const& row) {
    return choose(row, _mode == ui::SelectionMode::Multiple ? ui::RowClick::Toggle : ui::RowClick::Only);
}

bool TableImpl::choose(WidgetBase const& row, ui::RowClick click) {
    auto const rowKey = keyOf(&row);
    if (_mode == ui::SelectionMode::None || !rowKey || !row.actionable() || isHeading(&row)) {
        return true;
    }
    if (_mode == ui::SelectionMode::Single || click == ui::RowClick::Only) {
        _anchor = *rowKey;
        return change({*rowKey}, ui::SelectionChange::Replace);
    }
    if (click == ui::RowClick::Range) {
        return change(rangeTo(row), ui::SelectionChange::Replace);
    }
    // A user's change drops a requested key still waiting for its row.
    std::vector<ui::Key> next;
    for (auto const& selected : _selection) {
        if (rowWith(selected) != nullptr && std::ranges::find(next, selected) == next.end()) {
            next.push_back(selected);
        }
    }
    if (auto const found = std::ranges::find(next, *rowKey); found != next.end()) {
        next.erase(found);
    } else {
        next.push_back(*rowKey);
    }
    _anchor = *rowKey;
    return change(std::move(next), ui::SelectionChange::Extend);
}

std::vector<ui::Key> TableImpl::rangeTo(WidgetBase const& row) const {
    auto const rows = shownChildren();
    auto const target = std::ranges::find(rows, &row);
    if (target == rows.end()) {
        return {};
    }
    WidgetBase const* const anchorRow = _anchor ? rowWith(*_anchor) : nullptr;
    auto from = anchorRow == nullptr ? rows.end() : std::ranges::find(rows, anchorRow);
    if (from == rows.end()) {
        from = target;
    }
    auto const [low, high] = std::minmax(from, target);
    std::vector<ui::Key> keys;
    for (auto const* const between : std::ranges::subrange(low, std::next(high))) {
        if (auto const betweenKey = keyOf(between); betweenKey && between->actionable() && !isHeading(between)) {
            keys.push_back(*betweenKey);
        }
    }
    return keys;
}

std::vector<ui::Key> TableImpl::allOrNone() const {
    std::vector<ui::Key> keys;
    for (auto const* const row : shownChildren()) {
        if (auto const rowKey = keyOf(row); rowKey && row->actionable() && !isHeading(row)) {
            keys.push_back(*rowKey);
        }
    }
    bool const every = std::ranges::all_of(
        keys, [this](ui::Key const& rowKey) { return std::ranges::find(_selection, rowKey) != _selection.end(); });
    return every ? std::vector<ui::Key>{} : keys;
}

bool TableImpl::change(std::vector<ui::Key> next, ui::SelectionChange change) {
    if (next == _selection) {
        return true;
    }
    _selection = next;
    refresh();
    return report(std::move(next), change);
}

bool TableImpl::report(std::vector<ui::Key> keys, ui::SelectionChange change) {
    auto const handler = _onSelectionChange;
    if (!handler) {
        return true;
    }
    auto& shared = context();
    WidgetBase* alive = this;
    shared.watches.push_back(&alive);
    handler(std::move(keys), change);
    std::erase(shared.watches, &alive);
    return alive != nullptr;
}

void TableImpl::activateRow(ui::Key const& key) {
    auto const* const row = rowWith(key);
    if (row == nullptr || !row->actionable() || isHeading(row)) {
        return;
    }
    auto const handler = _onActivate;
    if (handler) {
        handler(key);
    }
}

void TableImpl::setRealizedWindow(std::size_t first, std::size_t count, std::size_t total) {
    static_cast<void>(first);
    static_cast<void>(count);
    _total = total;
    refresh();
}

void TableImpl::setOnVisibleRangeChanged(std::function<void(std::size_t, std::size_t)> onVisibleRangeChanged) {
    _onVisibleRange = std::move(onVisibleRangeChanged);
}

void TableImpl::setRowIndex(ui::Widget& row, std::size_t viewRow) {
    auto& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setRowIndex of a widget that is not this table's row"};
    }
    _viewRows.insert_or_assign(&base, viewRow);
    refresh();
}

void TableImpl::setStatus(ui::TableStatus const& status) {
    _status = status;
    refresh();
}

void TableImpl::setOnRetry(std::function<void()> onRetry) { _onRetry = std::move(onRetry); }

void TableImpl::setCellLoad(ui::Widget& row, std::size_t column, ui::CellLoad load) {
    auto& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setCellLoad of a widget that is not this table's row"};
    }
    if (load == ui::CellLoad::Loaded) {
        if (auto const found = _loads.find(&base); found != _loads.end()) {
            found->second.erase(column);
        }
    } else {
        _loads[&base].insert_or_assign(column, load);
    }
    refresh();
}

void TableImpl::setColumnWindow(std::span<std::size_t const> columns, std::span<int const> offsets,
                                std::span<int const> widths, int totalWidth, std::size_t pinned) {
    _arranged = true;
    _window.clear();
    for (auto const [column, offset, width] : std::views::zip(columns, offsets, widths)) {
        _window.push_back(WindowColumn{.column = column, .offset = offset, .width = std::max(width, 0)});
    }
    _pinned = std::min(pinned, _window.size());
    _totalWidth = std::max(totalWidth, 0);
    refresh();
}

// Only a table over a `TableSource` is given one, before any row: its rows are then placed by their view index and its
// cells by their column. A table over `rows` is told its realized window too, once it is long enough, and is no
// source.
void TableImpl::setOnHorizontalRange(std::function<void(int scrollX, int viewport)> handler) {
    _onHorizontalRange = std::move(handler);
    _source = true;
}

void TableImpl::setOnSelectRange(
    std::function<void(std::size_t anchorRow, std::size_t focusRow, ui::SelectionChange)> handler) {
    _onSelectRange = std::move(handler);
}

bool TableImpl::selectRange(std::size_t anchorRow, std::size_t focusRow, ui::SelectionChange change) {
    if (_mode != ui::SelectionMode::Multiple || !_onSelectRange) {
        return false;
    }
    auto const handler = _onSelectRange;
    handler(anchorRow, focusRow, change);
    return true;
}

EventResult TableImpl::sourceKey(::core::tui::KeyEvent const& key) {
    // The whole view, through the mount, which resolves it over the rows not realized; in Single mode it goes on
    // outwards.
    if (matchesChord("Ctrl+A", key) && _mode == ui::SelectionMode::Multiple) {
        if (_total > 0) {
            _anchorRow = 0;
            static_cast<void>(selectRange(0, _total - 1, ui::SelectionChange::Extend));
        }
        return EventResult::Handled;
    }
    bool const upward = matchesChord("Shift+Up", key);
    if ((upward || matchesChord("Shift+Down", key)) && _onFocusMove) {
        // Without an anchor, the range starts at the row the focus leaves.
        if (!_anchorRow) {
            _anchorRow = _focusCell ? _focusCell->first : 0;
        }
        auto const move = _onFocusMove;
        move(upward ? ui::FocusMove::Up : ui::FocusMove::Down);
        if (_focusCell) {
            static_cast<void>(selectRange(*_anchorRow, _focusCell->first, ui::SelectionChange::Replace));
        }
        return EventResult::Handled;
    }
    if (auto const move = focusMoveOf(key); move && _onFocusMove) {
        auto const handler = _onFocusMove;
        handler(*move);
        return EventResult::Handled;
    }
    WidgetBase* const row = _focusCell ? rowAtView(_focusCell->first) : nullptr;
    if (::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None && row != nullptr) {
        if (key.codepoint == U' ') {
            _anchorRow = _focusCell->first;
            static_cast<void>(pick(*row));
            return EventResult::Handled;
        }
        if (isKey(key, KeyCode::Enter)) {
            if (auto const rowKey = keyOf(row); rowKey && (_mode != ui::SelectionMode::Single || pick(*row))) {
                activateRow(*rowKey);
            }
            return EventResult::Handled;
        }
    }
    return EventResult::Ignored;
}

// The mount moved the view to keep the columns in view where they were: it knows the offset, so it is not told.
void TableImpl::setHorizontalOffset(int scrollX) {
    _scrollX = std::max(scrollX, 0);
    if (_reportedColumns) {
        _reportedColumns->first = _scrollX;
    }
    refresh();
}

// Inside the mount's pass: the view follows the cell now, and the mount hears of the move before the next frame.
void TableImpl::setFocusCell(std::size_t viewRow, std::size_t column) {
    _focusCell = std::pair{viewRow, column};
    scrollToRow(viewRow);
    int pinnedEdge = 0;
    for (std::size_t at = 0; at < _pinned; ++at) {
        pinnedEdge = std::max(pinnedEdge, _window.at(at).offset + _window.at(at).width);
    }
    for (std::size_t at = _pinned; at < _window.size(); ++at) {
        WindowColumn const& shown = _window.at(at);
        if (shown.column != column) {
            continue;
        }
        if (shown.offset - _scrollX < pinnedEdge) {
            _scrollX = shown.offset - pinnedEdge;
        } else if (shown.offset + shown.width - _scrollX > viewport()) {
            _scrollX = std::min(shown.offset - pinnedEdge, shown.offset + shown.width - viewport());
        }
    }
    clampView();
    reportBeforeNextFrame();
    refresh();
}

void TableImpl::setOnFocusMove(std::function<void(ui::FocusMove)> handler) { _onFocusMove = std::move(handler); }

void TableImpl::scrollToRow(std::size_t viewRow) {
    auto const room = static_cast<std::size_t>(std::max(_rowRoom, 1));
    if (viewRow < _top) {
        _top = viewRow;
    } else if (viewRow >= _top + room) {
        _top = viewRow + 1 - room;
    }
    clampView();
    reportBeforeNextFrame();
    refresh();
}

void TableImpl::setCellColumn(ui::Widget& row, ui::Widget& cell, std::optional<std::size_t> column) {
    auto& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setCellColumn of a widget that is not this table's row"};
    }
    _cellColumns[&base].insert_or_assign(&WidgetBase::of(cell), column);
    refresh();
}

void TableImpl::scrollColumnsTo(int scrollX) {
    _scrollX = scrollX;
    clampView();
    reportRanges();
    refresh();
}

// Columns first: the rows the mount realizes next are then built with the columns in view only.
void TableImpl::reportRanges() {
    if (!_source || _size.width <= 0) {
        return;
    }
    std::pair<int, int> const columns{_scrollX, viewport()};
    if (_onHorizontalRange && _reportedColumns != columns) {
        _reportedColumns = columns;
        auto const handler = _onHorizontalRange;
        handler(columns.first, columns.second);
    }
    auto const rows = rowsInView();
    if (_onVisibleRange && _reportedRows != rows) {
        _reportedRows = rows;
        auto const handler = _onVisibleRange;
        handler(rows.first, rows.second);
    }
}

std::vector<TableImpl::DrawnColumn> TableImpl::drawnColumns(int width) const {
    std::vector<DrawnColumn> drawn;
    if (!_arranged) {
        int x = 0;
        for (auto const [index, columnWidth] :
             std::views::zip(std::views::iota(std::size_t{0}), columnWidths(width))) {
            drawn.push_back(DrawnColumn{.column = index, .x = x, .width = columnWidth});
            x += columnWidth + kColumnGap;
        }
        return drawn;
    }
    // The pinned columns stay where they are; the rest scroll under them, cut where they pass under the last.
    int pinnedEdge = 0;
    for (std::size_t at = 0; at < _pinned; ++at) {
        pinnedEdge = std::max(pinnedEdge, _window.at(at).offset + _window.at(at).width);
    }
    int const room = std::max(0, width - gutter());
    for (std::size_t at = 0; at < _window.size(); ++at) {
        WindowColumn const& shown = _window.at(at);
        int x = shown.offset;
        int extent = shown.width;
        if (at >= _pinned) {
            x -= _scrollX;
            if (x < pinnedEdge) {
                extent -= pinnedEdge - x;
                x = pinnedEdge;
            }
        }
        extent = std::min(extent, room - x) - kColumnGap;
        if (extent > 0 && shown.column < _columns.size()) {
            drawn.push_back(DrawnColumn{.column = shown.column, .x = x, .width = extent});
        }
    }
    return drawn;
}

std::optional<std::size_t> TableImpl::columnOf(WidgetBase const* row, WidgetBase const* cell,
                                               std::size_t position) const {
    if (!_source) {
        return position;
    }
    auto const cells = _cellColumns.find(row);
    if (cells == _cellColumns.end()) {
        return std::nullopt;
    }
    auto const found = cells->second.find(cell);
    return found == cells->second.end() ? std::nullopt : found->second;
}

ui::CellLoad TableImpl::loadOf(WidgetBase const* row, std::size_t column) const {
    auto const cells = _loads.find(row);
    if (cells == _loads.end()) {
        return ui::CellLoad::Loaded;
    }
    auto const found = cells->second.find(column);
    return found == cells->second.end() ? ui::CellLoad::Loaded : found->second;
}

// A cell takes its column's slot; a pooled cell, one whose column is not drawn and one a mark stands for take none.
// A heading's one cell takes every column drawn.
void TableImpl::slotCells(std::vector<DrawnColumn> const& drawn) {
    std::unordered_map<std::size_t, DrawnColumn const*> byColumn;
    int across = 0;
    for (DrawnColumn const& column : drawn) {
        byColumn.emplace(column.column, &column);
        across = std::max(across, column.x + column.width);
    }
    for (auto* const row : children()) {
        auto* const stack = dynamic_cast<StackImpl*>(row);
        if (stack == nullptr) {
            continue;
        }
        std::vector<CellSlot> slots;
        slots.reserve(stack->children().size());
        for (auto* const cell : stack->children()) {
            CellSlot slot;
            auto const column = columnOf(row, cell, 0);
            if (isHeading(row)) {
                if (!_cellColumns.contains(row) || !_cellColumns.at(row).contains(cell)) {
                    slot = CellSlot{.x = 0, .width = across, .place = CellPlace{.across = ui::Align::Start}};
                }
            } else if (auto const found = column ? byColumn.find(*column) : byColumn.end();
                       found != byColumn.end() && loadOf(row, *column) == ui::CellLoad::Loaded) {
                ui::TableColumn const& spec = _columns.at(*column);
                slot = CellSlot{.x = found->second->x,
                                .width = found->second->width,
                                .place = CellPlace{.across = spec.align, .down = spec.valign}};
                // The terminal cannot wrap, so a text cell a column cuts ends in an ellipsis.
                if (auto* const text = dynamic_cast<TextImpl*>(cell)) {
                    text->setElided(spec.wrap ? TextImpl::Elision::PerLine : TextImpl::Elision::OneLine);
                } else if (auto* const badge = dynamic_cast<BadgeImpl*>(cell)) {
                    badge->setElided(true);
                }
            }
            slots.push_back(slot);
        }
        // A row with no cell keeps one empty slot, so it is never laid out by extents it was not given.
        if (slots.empty()) {
            slots.push_back(CellSlot{});
        }
        // A cell that is gone leaves its row: what is kept is the row's children's columns.
        if (auto const cells = _cellColumns.find(row);
            cells != _cellColumns.end() && cells->second.size() > stack->children().size()) {
            std::erase_if(cells->second, [stack](auto const& entry) {
                return !std::ranges::contains(stack->children(), entry.first);
            });
        }
        stack->setCellSlots(std::move(slots));
    }
}

int TableImpl::rowRoomOf(int height) const noexcept {
    return std::max(0, height - (_showHeader ? 1 : 0) - (_status.shown ? 1 : 0));
}

int TableImpl::viewport() const noexcept { return std::max(0, _size.width - gutter()); }

std::pair<std::size_t, std::size_t> TableImpl::rowsInView() const noexcept {
    return {_top, static_cast<std::size_t>(std::max(_rowRoom, 0))};
}

void TableImpl::clampView() {
    auto const room = static_cast<std::size_t>(std::max(_rowRoom, 0));
    _top = std::min(_top, _total > room ? _total - room : std::size_t{0});
    _scrollX = std::clamp(_scrollX, 0, std::max(0, _totalWidth - viewport()));
}

void TableImpl::reportBeforeNextFrame() {
    // A table drawn at no width reports nothing (`reportRanges`), so it asks for no frame either.
    if (_size.width <= 0) {
        return;
    }
    bool const columnsDue = _onHorizontalRange && _reportedColumns != std::pair{_scrollX, viewport()};
    bool const rowsDue = _onVisibleRange && _reportedRows != rowsInView();
    if (!columnsDue && !rowsDue) {
        return;
    }
    if (std::ranges::find(context().windowTables, this) == context().windowTables.end()) {
        context().windowTables.push_back(this);
    }
    refresh();
}

void TableImpl::paintStatus(::core::tui::Canvas& canvas) {
    _retryX = -1;
    if (!_status.shown || canvas.height() <= 0) {
        return;
    }
    auto const& theme = canvas.theme();
    int const line = canvas.height() - 1;
    std::string lead = _status.counts;
    if (!_status.state.empty()) {
        lead += (lead.empty() ? "" : std::string{kStatusGap}) + _status.state;
    }
    canvas.putString(line, 0, lead, theme.textMuted);
    int x = displayWidth(lead);
    if (_status.retry) {
        x += lead.empty() ? 0 : displayWidth(kStatusGap);
        _retryX = x;
        canvas.putString(line, x, std::string{kRetry}, theme.textAccent);
        x += displayWidth(kRetry);
    }
    if (!_status.message.empty()) {
        // At the trailing edge, or after the rest when the line is too short for both.
        int const width = displayWidth(_status.message);
        int const gap = x == 0 ? 0 : displayWidth(kStatusGap);
        canvas.putString(line, std::max(x + gap, canvas.width() - width), _status.message, theme.textNormal);
    }
}

void TableImpl::paintLoads(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& drawn,
                           std::vector<::core::tui::Rect> const& areas) const {
    auto const& theme = canvas.theme();
    for (auto const [row, area] : std::views::zip(children(), areas)) {
        auto const loads = _loads.find(row);
        if (area.empty() || loads == _loads.end() || isHeading(row)) {
            continue;
        }
        for (DrawnColumn const& column : drawn) {
            auto const found = loads->second.find(column.column);
            if (found == loads->second.end()) {
                continue;
            }
            bool const failed = found->second == ui::CellLoad::Failed;
            std::string const mark{failed ? kFailedMark : kUnloadedMark};
            auto cell = canvas.subcanvas({.x = gutter() + column.x, .y = area.y, .width = column.width, .height = 1});
            cell.putString(0, alignedOffset(_columns.at(column.column).align, displayWidth(mark), column.width), mark,
                           failed ? theme.error : theme.textMuted);
        }
    }
}

WidgetBase* TableImpl::rowAtView(std::size_t viewRow) const {
    auto const found = std::ranges::find_if(children(), [this, viewRow](WidgetBase const* row) {
        auto const index = _viewRows.find(row);
        return index != _viewRows.end() && index->second == viewRow;
    });
    return found == children().end() ? nullptr : *found;
}

std::optional<ui::FocusMove> TableImpl::focusMoveOf(::core::tui::KeyEvent const& key) {
    auto const held = ::core::tui::withoutLockKeys(key.modifiers);
    if (held == ::core::tui::Modifier::Ctrl) {
        if (isKey(key, KeyCode::Home)) {
            return ui::FocusMove::First;
        }
        if (isKey(key, KeyCode::End)) {
            return ui::FocusMove::Last;
        }
        return std::nullopt;
    }
    if (held != ::core::tui::Modifier::None) {
        return std::nullopt;
    }
    static constexpr std::array<std::pair<KeyCode, ui::FocusMove>, 8> kMoves{{
        {KeyCode::Up, ui::FocusMove::Up},
        {KeyCode::Down, ui::FocusMove::Down},
        {KeyCode::Left, ui::FocusMove::Left},
        {KeyCode::Right, ui::FocusMove::Right},
        {KeyCode::Home, ui::FocusMove::Home},
        {KeyCode::End, ui::FocusMove::End},
        {KeyCode::PageUp, ui::FocusMove::PageUp},
        {KeyCode::PageDown, ui::FocusMove::PageDown},
    }};
    for (auto const& [code, move] : kMoves) {
        if (isKey(key, code)) {
            return move;
        }
    }
    return std::nullopt;
}

}  // namespace morph::tui::detail
