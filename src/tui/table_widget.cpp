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

#include "tui/container_widgets.hpp"
#include "tui/layout.hpp"

namespace morph::tui::detail {

using ::core::tui::EventResult;
using ::core::tui::KeyCode;

namespace {

constexpr int kGutter = 2;
constexpr int kColumnGap = 1;
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

void TableImpl::setSelection(std::vector<ui::Key> const& selection) {
    _selection = selection;
    refresh();
}

void TableImpl::setOnSelectionChange(std::function<void(std::vector<ui::Key>)> onSelectionChange) {
    _onSelectionChange = std::move(onSelectionChange);
}

void TableImpl::setOnActivate(std::function<void(ui::Key)> onActivate) { _onActivate = std::move(onActivate); }

// The cursor goes to the row that took the removed one's place, or to the last row.
void TableImpl::childForgotten(WidgetBase& child) {
    _keys.erase(&child);
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
        widths.push_back(displayWidth(column.label));
    }
    for (auto const* row : children()) {
        auto const* const cells = dynamic_cast<ContainerBase const*>(row);
        if (cells == nullptr || !row->shown()) {
            continue;
        }
        for (auto const [width, cell] : std::views::zip(widths, cells->children())) {
            width = std::max(width, requestedSize(*cell).width);
        }
    }
    return widths;
}

std::vector<int> TableImpl::columnWidths(int width) const {
    std::vector<layout::Item> items;
    items.reserve(_columns.size());
    for (auto const [column, natural] : std::views::zip(_columns, naturalWidths())) {
        items.push_back(layout::Item{.sizing = column.width, .natural = natural});
    }
    return layout::distribute(items, layout::Track{.length = std::max(0, width - kGutter), .gap = kColumnGap});
}

::core::tui::Size TableImpl::naturalSize() const {
    auto const widths = naturalWidths();
    int const width = kGutter + std::accumulate(widths.begin(), widths.end(), 0) +
                      (kColumnGap * std::max(0, static_cast<int>(widths.size()) - 1));
    int height = 1;
    for (auto const* row : shownChildren()) {
        height += rowHeight(*row);
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
    return _cursor == nullptr ? 0 : std::min(_cursorPlace, rows.size() - 1);
}

void TableImpl::moveCursor(std::span<WidgetBase* const> rows, std::size_t place) {
    if (place >= rows.size()) {
        return;
    }
    _cursor = rows.subspan(place).front();
    _cursorPlace = place;
    _follow = true;
}

std::size_t TableImpl::lastTop(std::span<WidgetBase* const> rows) const {
    std::size_t top = rows.size();
    int used = 0;
    for (auto const* row : rows | std::views::reverse) {
        used += rowHeight(*row);
        if (used > _rowRoom) {
            break;
        }
        --top;
    }
    return rows.empty() ? 0 : std::min(top, rows.size() - 1);
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
                               [](int sum, WidgetBase const* row) { return sum + rowHeight(*row); });
    for (auto const* row : shown.first(shown.size() - 1)) {
        if (used <= _rowRoom) {
            return;
        }
        used -= rowHeight(*row);
        ++_top;
    }
}

std::vector<::core::tui::Rect> TableImpl::childAreas(::core::tui::Size size) const {
    std::vector<::core::tui::Rect> areas(children().size());
    int line = 1;
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
        int const height = rowHeight(*row);
        area = {.x = kGutter, .y = line, .width = std::max(0, size.width - kGutter), .height = height};
        line += height;
    }
    return areas;
}

void TableImpl::paint(::core::tui::Canvas& canvas) {
    auto const& theme = canvas.theme();
    _size = canvas.size();
    auto const widths = columnWidths(canvas.width());

    int left = kGutter;
    for (auto const [column, width] : std::views::zip(_columns, widths)) {
        auto label = canvas.subcanvas({.x = left, .y = 0, .width = width, .height = 1});
        label.putString(0, 0, column.label, theme.textBold);
        left += width + kColumnGap;
    }

    auto const rows = shownChildren();
    int const room = std::max(0, canvas.height() - 1);
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

    for (auto* row : children()) {
        if (auto* const stack = dynamic_cast<StackImpl*>(row)) {
            stack->setColumnLayout(widths, kColumnGap);
        }
    }
    auto const areas = childAreas(_size);
    placeChildren(areas);

    bool const focused = hasFocus();
    for (auto const [row, area] : std::views::zip(children(), areas)) {
        if (area.empty()) {
            continue;
        }
        bool const cursor = focused && row == rows.at(_cursorPlace);
        std::string gutter{isMarked(row) ? "*" : " "};
        gutter += cursor ? ">" : " ";
        canvas.putString(area.y, 0, gutter, cursor ? theme.listItemSelected : theme.textNormal);
    }
}

EventResult TableImpl::key(::core::tui::KeyEvent const& key) {
    if (::core::tui::withoutLockKeys(key.modifiers) != ::core::tui::Modifier::None) {
        return EventResult::Ignored;
    }
    auto const rows = shownChildren();
    auto const place = cursorPlace(rows);
    auto const last = rows.empty() ? std::size_t{0} : rows.size() - 1;
    auto const page = static_cast<std::size_t>(std::max(1, _rowRoom));
    if (isKey(key, KeyCode::Up)) {
        moveCursor(rows, place > 0 ? place - 1 : 0);
    } else if (isKey(key, KeyCode::Down)) {
        moveCursor(rows, std::min(place + 1, last));
    } else if (isKey(key, KeyCode::PageUp)) {
        moveCursor(rows, place - std::min(place, page));
    } else if (isKey(key, KeyCode::PageDown)) {
        moveCursor(rows, std::min(place + page, last));
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

void TableImpl::click(::core::tui::Point cell) {
    auto* const row = rowAtLine(cell.y);
    if (row == nullptr) {
        return;
    }
    auto const now = std::chrono::steady_clock::now();
    bool const twice = _lastClick.row == row && now - _lastClick.at <= kDoubleClick;
    _lastClick = twice ? LastClick{} : LastClick{.row = row, .at = now};
    auto const rows = shownChildren();
    moveCursor(rows, static_cast<std::size_t>(std::distance(rows.begin(), std::ranges::find(rows, row))));
    refresh();
    if (!twice) {
        static_cast<void>(pick(*row));
    } else if (auto const rowKey = keyOf(row)) {
        activateRow(*rowKey);
    }
}

bool TableImpl::wheel(int delta) {
    auto const rows = shownChildren();
    auto const target = delta < 0 ? _top - std::min(_top, static_cast<std::size_t>(-delta))
                                  : std::min(_top + static_cast<std::size_t>(delta), lastTop(rows));
    if (target == _top) {
        return false;
    }
    _top = target;
    refresh();
    return true;
}

bool TableImpl::pick(WidgetBase const& row) {
    auto const rowKey = keyOf(&row);
    if (_mode == ui::SelectionMode::None || !rowKey || !row.actionable()) {
        return true;
    }
    std::vector<ui::Key> next;
    if (_mode == ui::SelectionMode::Single) {
        next.push_back(*rowKey);
    } else {
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
    }
    if (next == _selection) {
        return true;
    }
    _selection = next;
    refresh();
    return report(std::move(next));
}

bool TableImpl::report(std::vector<ui::Key> keys) {
    auto const handler = _onSelectionChange;
    if (!handler) {
        return true;
    }
    auto& shared = context();
    WidgetBase* alive = this;
    shared.watches.push_back(&alive);
    handler(std::move(keys));
    std::erase(shared.watches, &alive);
    return alive != nullptr;
}

void TableImpl::activateRow(ui::Key const& key) {
    auto const* const row = rowWith(key);
    if (row == nullptr || !row->actionable()) {
        return;
    }
    auto const handler = _onActivate;
    if (handler) {
        handler(key);
    }
}

}  // namespace morph::tui::detail
