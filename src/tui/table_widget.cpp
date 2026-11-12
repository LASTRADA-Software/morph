// SPDX-License-Identifier: Apache-2.0

#include "tui/table_widget.hpp"

#include <algorithm>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Theme.hpp>
#include <iterator>
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
    int left = gutter();
    for (auto const [index, width] : std::views::zip(std::views::iota(std::size_t{0}), columnWidths(_size.width))) {
        if (x >= left && x < left + width) {
            auto const handler = _onSort;
            if (_columns.at(index).sortable && handler) {
                handler(index);
            }
            return;
        }
        left += width + kColumnGap;
    }
}

// The cursor goes to the row that took the removed one's place, or to the last row.
void TableImpl::childForgotten(WidgetBase& child) {
    _keys.erase(&child);
    _headings.erase(&child);
    _spans.erase(&child);
    _extraLines.erase(&child);
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
        for (auto const [width, cell] : std::views::zip(widths, cells->children())) {
            // A hidden cell is drawn as nothing, so it asks for no room.
            if (cell->shown()) {
                width = std::max(width, requestedSize(*cell).width);
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
        for (auto const [width, column, cell] : std::views::zip(widths, _columns, cells->children())) {
            if (!column.wrap || !cell->shown()) {
                continue;
            }
            auto const* const text = dynamic_cast<TextImpl const*>(cell);
            width = std::max(width, text != nullptr ? text->leastWidth() : requestedSize(*cell).width);
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
    auto const widths = naturalWidths();
    int const width = gutter() + std::accumulate(widths.begin(), widths.end(), 0) +
                      (kColumnGap * std::max(0, static_cast<int>(widths.size()) - 1));
    int height = _showHeader ? 1 : 0;
    for (auto const* row : shownChildren()) {
        height += heightOf(*row);
    }
    return {.width = width, .height = height};
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
    std::size_t place = 0;
    for (auto const [row, area] : std::views::zip(children(), areas)) {
        if (!row->shown()) {
            continue;
        }
        bool const above = place < _top;
        ++place;
        if (above || line >= size.height) {
            continue;
        }
        int const height = heightOf(*row);
        area = {.x = gutter(), .y = line, .width = std::max(0, size.width - gutter()), .height = height};
        line += height;
    }
    return areas;
}

void TableImpl::paintHeader(::core::tui::Canvas& canvas, std::vector<int> const& widths) const {
    auto const& theme = canvas.theme();
    if (_selectColumn) {
        canvas.putString(0, 0, headerBox(), theme.textNormal);
    }
    int left = gutter();
    for (auto const [index, column, width] : std::views::zip(std::views::iota(std::size_t{0}), _columns, widths)) {
        auto label = canvas.subcanvas({.x = left, .y = 0, .width = width, .height = 1});
        // Over its cells: a number column's header ends where its digits do.
        std::string const text = headerText(index);
        label.putString(0, alignedOffset(column.align, displayWidth(text), width), text, theme.textBold);
        left += width + kColumnGap;
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
    auto const widths = columnWidths(canvas.width());

    if (_showHeader) {
        paintHeader(canvas, widths);
    }

    auto const rows = shownChildren();
    int const room = std::max(0, canvas.height() - (_showHeader ? 1 : 0));
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

    planSpans();
    arrangeRows(widths);
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
        bool const cursor = focused && row == rows.at(_cursorPlace);
        canvas.putString(area.y, 0, rowMark(row, cursor), cursor ? theme.listItemSelected : theme.textNormal);
    }
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
    bool const onBox = _selectColumn && cell.x < kBoxWidth;
    // The header box is Ctrl+A; it does nothing while the table shows no rows.
    if (onBox && _showHeader && cell.y == 0) {
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

}  // namespace morph::tui::detail
