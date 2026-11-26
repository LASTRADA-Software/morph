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
#include <morph/ui/cell_format.hpp>
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
/// The cells of a filter button at a header's end: a space and the button.
constexpr int kFilterMark = 2;
/// A filter button, empty and filled.
constexpr std::string_view kFilterEmpty = "\u25BF";
constexpr std::string_view kFilterFilled = "\u25BE";
/// What the gutter's last cell shows for a row an action is working on, and for a row another user holds.
constexpr std::string_view kBusyGlyph = "\u2026";
constexpr std::string_view kLockGlyph = "#";
/// The gutter cells a busy or locked glyph takes: the glyph, and a gap, so the ellipsis is not read as the first
/// cell's truncation.
constexpr int kMarkCells = 2;
/// What a busy or locked row says on hover when its marks carry no text of their own.
constexpr std::string_view kBusyText = "Saving this row";
constexpr std::string_view kLockText = "Locked by another user";
/// The button at the header line's end that opens the table's menu.
constexpr std::string_view kTableMenu = "\u22EF";
/// The cells the header line keeps for the table menu: a gap and the button.
constexpr int kTableMenuRoom = 2;
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

/// A codepoint as UTF-8.
std::string utf8Of(char32_t codepoint) {
    std::string out;
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return out;
}

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

void TableImpl::setRowSummary(ui::Widget& row) {
    auto const& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setRowSummary of a widget that is not this table's row"};
    }
    _summaries.insert(&base);
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
// NOLINTNEXTLINE(readability-function-cognitive-complexity): plans spans in one pass in the rows' order: the spans carried from earlier rows decide what each next row covers
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
            if (span.column >= _columns.size() || span.column >= cells.size() || span.rows == 0 ||
                coveredUntil.at(span.column) > index) {
                continue;
            }
            // The columns it covers, its own and the next ones, clipped at the last column.
            std::size_t const wide = std::max<std::size_t>(1, std::min(span.columns, _columns.size() - span.column));
            std::size_t last = std::min(index + span.rows, rows.size()) - 1;
            for (std::size_t between = index + 1; between <= last; ++between) {
                if (isHeading(rows.at(between))) {
                    last = between - 1;
                    break;
                }
            }
            if (last == index && wide == 1) {
                continue;
            }
            for (std::size_t column = span.column; column < span.column + wide; ++column) {
                coveredUntil.at(column) = last + 1;
            }
            plan.push_back({.origin = rows.at(index),
                            .cell = cells.at(span.column),
                            .column = span.column,
                            .columns = wide,
                            .first = index,
                            .last = last});
            for (std::size_t between = index; between <= last; ++between) {
                auto const coveredCells = cellsOf(rows.at(between));
                for (std::size_t column = span.column; column < span.column + wide; ++column) {
                    if ((between != index || column != span.column) && column < coveredCells.size()) {
                        covered.insert(coveredCells.at(column));
                    }
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
    auto const widths = columnWidths(size.width - menuRoom());
    int const header = headerLines();
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
        // Across the columns it covers and the gaps between them.
        int across = widths.at(place.column);
        for (std::size_t index = place.column + 1; index < place.column + place.columns && index < widths.size();
             ++index) {
            across += kColumnGap + widths.at(index);
        }
        area = {.x = left, .y = top + alignedOffset(down, height, space), .width = across, .height = height};
        lines = {.x = left, .y = top, .width = across, .height = space};
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
        return candidate < rows.size() && !isPassive(rows.subspan(candidate).front());
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

int TableImpl::gutter() const noexcept {
    return (_selectColumn ? kSelectGutter : kGutter) + (_markGutter ? kMarkCells : 0);
}

std::string TableImpl::headerBox() const {
    auto const rows = shownChildren();
    auto const data = std::ranges::count_if(rows, [this](WidgetBase const* row) { return !isPassive(row); });
    auto const marked = std::ranges::count_if(rows, [this](WidgetBase const* row) { return isMarked(row); });
    if (marked == 0) {
        return "[ ]";
    }
    return marked == data ? "[x]" : "[-]";
}

void TableImpl::setSelection(std::vector<ui::Key> const& selection) {
    _viewExcept.reset();
    _selection = selection;
    refresh();
}

void TableImpl::setSelectionView(std::vector<ui::Key> const& except) {
    _viewExcept = except;
    refresh();
}

void TableImpl::setOnStatusEntry(std::function<void(std::string entryId)> onStatusEntry) {
    _onStatusEntry = std::move(onStatusEntry);
}

void TableImpl::setRowMarks(ui::Widget& row, ui::RowMarks const& marks) {
    auto& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setRowMarks of a widget that is not this table's row"};
    }
    if (marks == ui::RowMarks{}) {
        _marks.erase(&base);
    } else {
        _marks.insert_or_assign(&base, marks);
    }
    base.applyRowMuted(marks.unmatched);
    // The marks' text wins over the row's own tooltip while it lasts; a busy or locked row always says why.
    std::string_view said = marks.tooltip;
    if (said.empty()) {
        if (marks.locked) {
            said = kLockText;
        } else if (marks.busy) {
            said = kBusyText;
        }
    }
    base.applyMarksTooltip(said);
    // The gutter takes cells for the glyph from the first busy or locked row on, and keeps it, so the columns do not
    // move as rows start and stop being busy.
    _markGutter = _markGutter || marks.busy || marks.locked;
    refresh();
}

void TableImpl::setOnSelectionChange(
    std::function<void(std::vector<ui::Key>, ui::SelectionChange)> onSelectionChange) {
    _onSelectionChange = std::move(onSelectionChange);
}

void TableImpl::setOnSelectAll(std::function<void()> onSelectAll) { _onSelectAll = std::move(onSelectAll); }

void TableImpl::setOnActivate(std::function<void(ui::Key)> onActivate) { _onActivate = std::move(onActivate); }

void TableImpl::setSort(std::vector<ui::TableSort> const& chain) {
    _chain = chain;
    refresh();
}

void TableImpl::setOnSort(std::function<void(std::size_t, ui::HeaderClick)> onSort) { _onSort = std::move(onSort); }

void TableImpl::setMenu(std::function<std::vector<ui::MenuEntry>(std::optional<std::size_t>)> entries,
                        std::function<void(std::optional<std::size_t>, std::vector<std::size_t>)> onChoose) {
    _menu = std::move(entries);
    _onMenu = std::move(onChoose);
    refresh();
}

void TableImpl::setVisibleColumns(std::vector<std::size_t> const& visible) {
    _visible = visible;
    auto const order = visibleOrder();
    if (_headerFocus && std::ranges::find(order, *_headerFocus) == order.end()) {
        _headerFocus.reset();
    }
    refresh();
}

void TableImpl::setOnColumnMove(std::function<void(std::size_t, std::size_t)> handler) {
    _onColumnMove = std::move(handler);
}

void TableImpl::setOnFilter(std::function<void(std::size_t)> handler) { _onFilter = std::move(handler); }

void TableImpl::setFiltered(std::vector<std::size_t> const& columns) {
    _filtered = columns;
    refresh();
}

void TableImpl::setColumnGroups(std::vector<ui::TableColumnGroup> const& groups) {
    _groups = groups;
    refresh();
}

void TableImpl::setBands(std::vector<ui::BandRun> const& runs) {
    _bands = runs;
    refresh();
}

void TableImpl::setOnGroupFilter(std::function<void(std::size_t)> handler) { _onGroupFilter = std::move(handler); }

void TableImpl::setOnCopy(std::function<void()> onCopy) { _onCopy = std::move(onCopy); }

std::optional<std::size_t> TableImpl::chainPlace(std::size_t index) const {
    auto const found = std::ranges::find(_chain, index, &ui::TableSort::column);
    if (found == _chain.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(_chain.begin(), found));
}

std::string TableImpl::headerText(std::size_t index) const {
    std::string text = ui::headerText(_columns.at(index).label, _columns.at(index).unit);
    if (auto const place = chainPlace(index)) {
        text += ui::sortMarkText(_chain.at(*place).direction, *place + 1, _chain.size());
    }
    return text;
}

std::string TableImpl::headerName(std::size_t index) const {
    std::string name = ui::headerAccessibleName(_columns.at(index).label, _columns.at(index).unit);
    if (_columns.at(index).filterable && std::ranges::find(_filtered, index) != _filtered.end()) {
        name += ", filtered";
    }
    return name;
}

std::string TableImpl::headerDescription(std::size_t index) const {
    auto const place = chainPlace(index);
    return place ? ui::sortDescription(_chain.at(*place).direction, *place + 1, _chain.size()) : std::string{};
}

// A one-key mark fits the room a sortable header keeps; a longer chain's places widen the headers it marks.
int TableImpl::headerWidth(ui::TableColumn const& column) const {
    int mark = column.sortable ? kSortMark : 0;
    if (column.sortable && _chain.size() > 1) {
        mark = displayWidth(ui::sortMarkText(ui::SortDirection::Ascending, _chain.size(), _chain.size()));
    }
    return displayWidth(ui::headerText(column.label, column.unit)) + mark + (column.filterable ? kFilterMark : 0);
}

int TableImpl::headerLines() const noexcept {
    if (!_showHeader) {
        return 0;
    }
    return bandLines() + (_groups.empty() ? 1 : 2);
}

int TableImpl::bandLines() const noexcept {
    std::size_t levels = 0;
    for (ui::BandRun const& run : _bands) {
        levels = std::max(levels, run.level + 1);
    }
    return static_cast<int>(levels);
}

std::vector<std::size_t> TableImpl::visibleOrder() const {
    if (!_visible.empty()) {
        return _visible;
    }
    std::vector<std::size_t> order(_columns.size());
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from Emscripten's libc++.
    std::iota(order.begin(), order.end(), std::size_t{0});
    return order;
}

void TableImpl::clickHeader(int x, ::core::tui::Modifier modifiers) {
    if (_menu && x == _size.width - 1) {
        openMenu(std::nullopt);
        return;
    }
    for (DrawnColumn const& drawn : drawnColumns(_size.width)) {
        int const left = gutter() + drawn.x;
        if (x < left || x >= left + drawn.width) {
            continue;
        }
        if (std::optional<std::size_t> const place = visiblePlace(drawn.column);
            _onCellSelect && place &&
            ::core::tui::hasModifier(::core::tui::withoutLockKeys(modifiers), ::core::tui::Modifier::Ctrl)) {
            if (_total > 0) {
                auto const handler = _onCellSelect;
                handler(0, *place, _total - 1, *place);
            }
            return;
        }
        ui::TableColumn const& column = _columns.at(drawn.column);
        if (column.filterable && x == left + drawn.width - 1) {
            if (auto const handler = _onFilter) {
                handler(drawn.column);
            }
            return;
        }
        auto const handler = _onSort;
        if (column.sortable && handler) {
            bool const shift =
                ::core::tui::hasModifier(::core::tui::withoutLockKeys(modifiers), ::core::tui::Modifier::Shift);
            handler(drawn.column, shift ? ui::HeaderClick::Shift : ui::HeaderClick::Plain);
        }
        return;
    }
}

// A group's span is the drawn columns at the visible places it names; its filter button is the span's last cell.
std::optional<int> TableImpl::groupFilterX(std::size_t group) const {
    if (group >= _groups.size()) {
        return std::nullopt;
    }
    auto const order = visibleOrder();
    ui::TableColumnGroup const& shown = _groups.at(group);
    int right = -1;
    for (DrawnColumn const& column : drawnColumns(_size.width)) {
        auto const place = std::ranges::find(order, column.column);
        auto const index = static_cast<std::size_t>(std::distance(order.begin(), place));
        if (place != order.end() && index >= shown.first && index < shown.first + shown.count) {
            right = std::max(right, gutter() + column.x + column.width - 1);
        }
    }
    return right >= 0 ? std::optional<int>{right} : std::nullopt;
}

void TableImpl::clickGroups(int x) {
    for (std::size_t group = 0; group < _groups.size(); ++group) {
        if (groupFilterX(group) == x) {
            if (auto const handler = _onGroupFilter) {
                handler(group);
            }
            return;
        }
    }
}

void TableImpl::focusHeader() {
    auto const order = visibleOrder();
    if (order.empty()) {
        return;
    }
    _headerFocus = order.front();
    if (!_chain.empty() && std::ranges::find(order, _chain.front().column) != order.end()) {
        _headerFocus = _chain.front().column;
    }
    refresh();
}

void TableImpl::openMenu(std::optional<std::size_t> column) {
    if (!_menu) {
        return;
    }
    auto const bounds = view().screenBounds();
    int const line = bounds.y + headerLines() - 1;
    ::core::tui::Rect anchor{.x = bounds.x + bounds.width - 1, .y = line, .width = 1, .height = 1};
    if (column) {
        for (DrawnColumn const& drawn : drawnColumns(_size.width)) {
            if (drawn.column == *column) {
                anchor = {.x = bounds.x + gutter() + drawn.x, .y = line, .width = drawn.width, .height = 1};
            }
        }
    }
    auto const entries = _menu;
    auto const onChoose = _onMenu;
    // A menu asked for while another is open replaces it: a right click on another header moves the menu there.
    _menuPopup.close();
    static_cast<void>(_menuPopup.open(entries(column), anchor, [onChoose, column](std::vector<std::size_t> path) {
        if (onChoose) {
            onChoose(column, std::move(path));
        }
    }));
}

void TableImpl::setCellMenu(std::function<std::vector<ui::MenuEntry>(ui::Key const&, std::size_t)> entries,
                            std::function<void(ui::Key const&, std::size_t, std::vector<std::size_t> const&)> chosen) {
    _cellMenu = std::move(entries);
    _cellChosen = std::move(chosen);
}

void TableImpl::openCellMenu(WidgetBase const& row, std::size_t column) {
    std::optional<ui::Key> const rowKey = keyOf(&row);
    if (!_cellMenu || !rowKey || isPassive(&row)) {
        return;
    }
    // A row already selected keeps the selection, so the menu acts on every selected row.
    if (_mode != ui::SelectionMode::None && !isMarked(&row) &&
        !change(std::vector<ui::Key>{*rowKey}, ui::SelectionChange::Replace)) {
        return;
    }
    auto const bounds = view().screenBounds();
    ::core::tui::Rect anchor{.x = bounds.x, .y = bounds.y, .width = 1, .height = 1};
    for (auto const [child, area] : std::views::zip(children(), childAreas(_size))) {
        if (child == &row) {
            anchor.y = bounds.y + area.y;
        }
    }
    for (DrawnColumn const& drawn : drawnColumns(_size.width)) {
        if (drawn.column == column) {
            anchor.x = bounds.x + gutter() + drawn.x;
            anchor.width = std::max(1, drawn.width);
        }
    }
    auto const entries = _cellMenu;
    auto const chosen = _cellChosen;
    std::vector<ui::MenuEntry> shown = entries(*rowKey, column);
    _menuPopup.close();
    static_cast<void>(_menuPopup.open(std::move(shown), anchor,
                                      [chosen, key = *rowKey, column](std::vector<std::size_t> const& path) {
                                          if (chosen) {
                                              chosen(key, column, path);
                                          }
                                      }));
}

EventResult TableImpl::cellKey(::core::tui::KeyEvent const& key) {
    WidgetBase const* const row = _focusCell ? rowAtView(_focusCell->first) : nullptr;
    std::optional<ui::Key> const rowKey = row == nullptr ? std::nullopt : keyOf(row);
    if (!_cellMenu || !rowKey || !_focusCell) {
        return EventResult::Ignored;
    }
    std::size_t const column = _focusCell->second;
    if (matchesChord("Shift+F10", key) || isKey(key, KeyCode::Menu)) {
        openCellMenu(*row, column);
        return EventResult::Handled;
    }
    if (std::ranges::none_of(_cellMenuKeys, [&key](std::string const& chord) { return matchesChord(chord, key); })) {
        return EventResult::Ignored;
    }
    auto const entries = _cellMenu;
    auto const chosen = _cellChosen;
    std::vector<std::size_t> path;
    std::function<bool(std::vector<ui::MenuEntry> const&)> const find = [&](std::vector<ui::MenuEntry> const& level) {
        for (std::size_t at = 0; at < level.size(); ++at) {
            ui::MenuEntry const& entry = level.at(at);
            if (entry.separator || entry.heading || !entry.enabled) {
                continue;
            }
            path.push_back(at);
            if (entry.items.empty() ? !entry.keys.empty() && matchesChord(entry.keys, key) : find(entry.items)) {
                return true;
            }
            path.pop_back();
        }
        return false;
    };
    if (!chosen || !find(entries(*rowKey, column))) {
        return EventResult::Ignored;
    }
    chosen(*rowKey, column, path);
    return EventResult::Handled;
}

// Left and Right walk the visible columns; a move is reported one place over and the table waits for the mount to
// rearrange it, the focus staying on the moved column.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): Left, Right, Down and the chords each move the header focus; the branches share the visible order and the focused place
EventResult TableImpl::headerKey(::core::tui::KeyEvent const& key) {
    if (!_headerFocus) {
        return EventResult::Ignored;
    }
    auto const order = visibleOrder();
    auto const found = std::ranges::find(order, *_headerFocus);
    if (found == order.end()) {
        _headerFocus.reset();
        return EventResult::Ignored;
    }
    auto const place = static_cast<std::size_t>(std::distance(order.begin(), found));
    std::size_t const column = *_headerFocus;
    if (isKey(key, KeyCode::Down) && ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None) {
        _headerFocus.reset();
        refresh();
        return EventResult::Handled;
    }
    if (matchesChord("Left", key) || matchesChord("Right", key)) {
        bool const right = matchesChord("Right", key);
        if (right && place + 1 < order.size()) {
            _headerFocus = order.at(place + 1);
        } else if (!right && place > 0) {
            _headerFocus = order.at(place - 1);
        }
        refresh();
        return EventResult::Handled;
    }
    bool const shifted = matchesChord("Shift+Enter", key) || matchesChord("Shift+Space", key);
    if (shifted || matchesChord("Enter", key) || matchesChord("Space", key)) {
        if (auto const handler = _onSort; handler && _columns.at(column).sortable) {
            handler(column, shifted ? ui::HeaderClick::Shift : ui::HeaderClick::Plain);
        }
        return EventResult::Handled;
    }
    bool const later = matchesChord("Ctrl+Shift+Right", key);
    if (later || matchesChord("Ctrl+Shift+Left", key)) {
        if (auto const handler = _onColumnMove; handler && (later ? place + 1 < order.size() : place > 0)) {
            handler(column, later ? place + 1 : place - 1);
        }
        return EventResult::Handled;
    }
    if (matchesChord("Shift+F10", key) || isKey(key, KeyCode::Menu)) {
        openMenu(column);
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

// The cursor goes to the row that took the removed one's place, or to the last row.
void TableImpl::childForgotten(WidgetBase& child) {
    _keys.erase(&child);
    _headings.erase(&child);
    _summaries.erase(&child);
    _marks.erase(&child);
    _spans.erase(&child);
    _extraLines.erase(&child);
    _viewRows.erase(&child);
    _cellColumns.erase(&child);
    _loads.erase(&child);
    _accents.erase(&child);
    _editors.erase(&child);
    _cellMarks.erase(&child);
    _modifiedRows.erase(&child);
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
    int width = gutter() + _totalWidth + menuRoom();
    if (!_arranged) {
        auto const widths = naturalWidths();
        width = gutter() + std::accumulate(widths.begin(), widths.end(), 0) +
                (kColumnGap * std::max(0, static_cast<int>(widths.size()) - 1)) + menuRoom();
    }
    std::int64_t height = headerLines() + (_status.shown ? 1 : 0);
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
    if (isPassive(row)) {
        return false;
    }
    auto const rowKey = keyOf(row);
    if (_viewExcept) {
        return rowKey && !std::ranges::contains(*_viewExcept, *rowKey);
    }
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
    int line = headerLines();
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
    int const line = headerLines() - 1;
    if (!_bands.empty()) {
        paintBands(canvas, columns);
    }
    if (!_groups.empty()) {
        paintGroups(canvas, columns);
    }
    if (_selectColumn) {
        canvas.putString(line, 0, headerBox(), theme.textNormal);
    }
    bool const focused = hasFocus();
    for (DrawnColumn const& drawn : columns) {
        ui::TableColumn const& column = _columns.at(drawn.column);
        // The filter button takes the column's last cell; the label is aligned in the cells before it.
        int const button = column.filterable ? kFilterMark : 0;
        auto label = canvas.subcanvas(
            {.x = gutter() + drawn.x, .y = line, .width = std::max(0, drawn.width - button), .height = 1});
        // Over its cells: a number column's header ends where its digits do.
        std::string const text = headerText(drawn.column);
        bool const current = focused && _headerFocus == drawn.column;
        label.putString(0, alignedOffset(column.align, displayWidth(text), std::max(0, drawn.width - button)), text,
                        current ? theme.listItemSelected : theme.textBold);
        if (column.filterable && drawn.width > 0) {
            bool const filtered = std::ranges::find(_filtered, drawn.column) != _filtered.end();
            canvas.putString(line, gutter() + drawn.x + drawn.width - 1, filtered ? kFilterFilled : kFilterEmpty,
                             theme.textNormal);
        }
    }
    if (_menu && canvas.width() > 0) {
        canvas.putString(line, canvas.width() - 1, kTableMenu, theme.textNormal);
    }
}

void TableImpl::paintGroups(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& columns) const {
    auto const& theme = canvas.theme();
    auto const order = visibleOrder();
    for (ui::TableColumnGroup const& group : _groups) {
        int left = -1;
        int right = -1;
        for (DrawnColumn const& drawn : columns) {
            auto const place = std::ranges::find(order, drawn.column);
            auto const index = static_cast<std::size_t>(std::distance(order.begin(), place));
            if (place != order.end() && index >= group.first && index < group.first + group.count) {
                left = left < 0 ? gutter() + drawn.x : std::min(left, gutter() + drawn.x);
                right = std::max(right, gutter() + drawn.x + drawn.width);
            }
        }
        if (left < 0 || right <= left) {
            continue;
        }
        int const y = bandLines();
        auto span =
            canvas.subcanvas({.x = left, .y = y, .width = std::max(0, right - left - kFilterMark), .height = 1});
        span.putString(0, 0, group.label, theme.textBold);
        canvas.putString(y, right - 1, group.filtered ? kFilterFilled : kFilterEmpty, theme.textNormal);
    }
}

// A run spans the drawn columns at the visible places it names, as the mount cut it: a band never covers a column that
// is not its own.
void TableImpl::paintBands(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& columns) const {
    auto const& theme = canvas.theme();
    auto const order = visibleOrder();
    for (ui::BandRun const& run : _bands) {
        int left = -1;
        int right = -1;
        for (DrawnColumn const& drawn : columns) {
            auto const place = std::ranges::find(order, drawn.column);
            auto const index = static_cast<std::size_t>(std::distance(order.begin(), place));
            if (place != order.end() && index >= run.first && index < run.first + run.count) {
                left = left < 0 ? gutter() + drawn.x : std::min(left, gutter() + drawn.x);
                right = std::max(right, gutter() + drawn.x + drawn.width);
            }
        }
        if (left < 0 || right <= left) {
            continue;
        }
        auto span =
            canvas.subcanvas({.x = left, .y = static_cast<int>(run.level), .width = right - left, .height = 1});
        span.putString(0, 0, ui::headerText(run.label, run.unit), theme.textBold);
    }
}

std::string TableImpl::rowMark(WidgetBase const* row, bool cursor) const {
    // With a select column the box shows the mark; without, `*` does, for a selected row or a row of the selected
    // cells.
    std::string mark;
    if (isHeading(row)) {
        mark.assign(static_cast<std::size_t>(gutter()), ' ');
        return mark;
    }
    if (_summaries.contains(row)) {
        mark = "\u03A3";
        mark.append(static_cast<std::size_t>(std::max(0, gutter() - 1)), ' ');
        return mark;
    }
    bool inCells = false;
    if (auto const index = _viewRows.find(row); _cellRect && index != _viewRows.end()) {
        inCells = index->second >= _cellRect->at(0) && index->second < _cellRect->at(0) + _cellRect->at(1);
    }
    if (_selectColumn) {
        mark = isMarked(row) ? "[x]" : "[ ]";
    } else {
        std::string_view marker = " ";
        if (isMarked(row) || inCells) {
            marker = "*";
        } else if (_modifiedRows.contains(row)) {
            marker = "+";
        }
        mark = marker;
    }
    mark += cursor ? ">" : " ";
    // A busy or locked row's glyph takes the gutter's own cells, after the selection and cursor marks.
    if (_markGutter) {
        auto const found = _marks.find(row);
        bool const busy = found != _marks.end() && found->second.busy;
        bool const locked = found != _marks.end() && found->second.locked;
        if (busy) {
            mark += kBusyGlyph;
        } else if (locked) {
            mark += kLockGlyph;
        } else {
            mark += ' ';
        }
        mark += ' ';
    }
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one paint walks the header, the rows and the status line against a shared canvas and column layout
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
        paintMarks(canvas, drawn, areas);
        // Focus that left the table and everything in it, an open popup of a widget in it (an editor's list)
        // included, is reported before the next frame, outside this one.
        auto const* const focusedNow = context().screen->focusedComponent();
        bool within = isWithin(focusedNow, view());
        for (WidgetBase const* const owner : context().popups) {
            if (!within && owner->openPopup() != nullptr && isWithin(&owner->view(), view())) {
                within = isWithin(focusedNow, *owner->openPopup());
            }
        }
        if (std::exchange(_focusWithin, within) && !within && _onFocusOut) {
            _focusLeft = true;
            if (std::ranges::find(context().windowTables, this) == context().windowTables.end()) {
                context().windowTables.push_back(this);
            }
            refresh();
        }
    }
    paintStatus(canvas);
}

void TableImpl::paintMarks(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& drawn,
                           std::vector<::core::tui::Rect> const& areas) const {
    auto const& theme = canvas.theme();
    for (auto const [row, area] : std::views::zip(children(), areas)) {
        auto const marks = _cellMarks.find(row);
        if (area.empty() || marks == _cellMarks.end()) {
            continue;
        }
        for (DrawnColumn const& column : drawn) {
            auto const found = marks->second.find(column.column);
            if (found == marks->second.end() || column.width <= 0 ||
                (!found->second.error && !found->second.modified)) {
                continue;
            }
            canvas.putString(area.y, gutter() + column.x + column.width - 1, found->second.error ? "!" : "+",
                             found->second.error ? theme.error : theme.textMuted);
        }
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
        if (_onSelectAll) {
            // A copy: the handler may destroy this table.
            auto const handler = _onSelectAll;
            handler();
            return EventResult::Handled;
        }
        static_cast<void>(change(allOrNone(), ui::SelectionChange::Extend));
        return EventResult::Handled;
    }
    return EventResult::Ignored;
}

// Any key ends a double click: the click after it is a first click.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): maps each key to the table's mode (header, cell menu, source or plain rows); the modes share the focus and anchor state
EventResult TableImpl::key(::core::tui::KeyEvent const& key) {
    _lastClick = {};
    if (matchesChord("Ctrl+C", key) && _onCopy) {
        // A copy: the handler may destroy this table.
        auto const handler = _onCopy;
        handler();
        return EventResult::Handled;
    }
    if (_headerFocus) {
        return headerKey(key);
    }
    // Up on the first row goes to the header.
    if (_showHeader && headerActs() && isKey(key, KeyCode::Up) &&
        ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None) {
        bool const first = _source ? (!_focusCell || _focusCell->first == 0) : cursorPlace(shownChildren()) == 0;
        if (first) {
            focusHeader();
            return EventResult::Handled;
        }
    }
    // The status line's entries, by the key each shows: `Alt+<n>`.
    for (std::size_t at = 0; at < _entriesDrawn.size() && at < 9; ++at) {
        if (matchesChord("Alt+" + std::to_string(at + 1), key) && _onStatusEntry) {
            auto const handler = _onStatusEntry;
            handler(_entriesDrawn.at(at).entryId);
            return EventResult::Handled;
        }
    }
    if (_source && _onText && _focusCell && key.codepoint >= U' ' && key.codepoint != 0x7f &&
        (::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None ||
         ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::Shift)) {
        // A copy: the handler may destroy this table.
        auto const handler = _onText;
        handler(utf8Of(key.codepoint));
        return EventResult::Handled;
    }
    if (_source) {
        if (auto const result = sourceKey(key); result == EventResult::Handled) {
            return result;
        }
        if (cellKey(key) == EventResult::Handled) {
            return EventResult::Handled;
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a click resolves to the header, the gutter, a cell or the status line from one hit test, each target needing the modifier state
void TableImpl::clickWith(::core::tui::Point cell, ::core::tui::Modifier modifiers) {
    if (_status.shown && cell.y == _size.height - 1) {
        if (_retryX >= 0 && cell.x >= _retryX && cell.x < _retryX + displayWidth(kRetry) && _onRetry) {
            auto const handler = _onRetry;
            handler();
            return;
        }
        for (DrawnEntry const& entry : _entriesDrawn) {
            if (cell.x >= entry.x && cell.x < entry.x + entry.width && _onStatusEntry) {
                auto const handler = _onStatusEntry;
                handler(entry.entryId);
                return;
            }
        }
        return;
    }
    bool const onBox = _selectColumn && cell.x < kBoxWidth;
    // The header box is Ctrl+A; it does nothing while the table shows no rows. Over a source it selects the view.
    int const headerLine = headerLines() - 1;
    if (_showHeader && !_groups.empty() && cell.y == bandLines()) {
        clickGroups(cell.x);
        return;
    }
    if (onBox && _showHeader && cell.y == headerLine) {
        if (_source) {
            if (_mode == ui::SelectionMode::Multiple && _onSelectAll) {
                auto const handler = _onSelectAll;
                handler();
            } else if (_total > 0) {
                static_cast<void>(selectRange(0, _total - 1, ui::SelectionChange::Extend));
            }
            return;
        }
        if (_mode == ui::SelectionMode::Multiple && !shownChildren().empty()) {
            if (_onSelectAll) {
                auto const handler = _onSelectAll;
                handler();
                return;
            }
            static_cast<void>(change(allOrNone(), ui::SelectionChange::Extend));
        }
        return;
    }
    if (_showHeader && cell.y == headerLine) {
        clickHeader(cell.x, modifiers);
        return;
    }
    // A spanning cell drawn over a covered row's line is its origin row's.
    auto* const spanning = spanOriginAt(cell);
    auto* const row = spanning != nullptr ? spanning : rowAtLine(cell.y);
    if (row == nullptr || isPassive(row)) {
        return;
    }
    auto const held = ::core::tui::withoutLockKeys(modifiers);
    if (_onCellSelect && selectCellsAt(row, cell, held)) {
        return;
    }
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
    if (!twice && !choose(*row, click)) {
        return;
    }
    if (clickCell(row, cell.x, twice) || !twice) {
        return;
    }
    if (auto const rowKey = keyOf(row)) {
        activateRow(*rowKey);
    }
}

bool TableImpl::selectCellsAt(WidgetBase const* row, ::core::tui::Point cell, ::core::tui::Modifier held) {
    auto const index = _viewRows.find(row);
    std::size_t const places = visibleOrder().size();
    if (index == _viewRows.end() || places == 0) {
        return false;
    }
    // A copy: the handler may destroy this table.
    auto const handler = _onCellSelect;
    if (cell.x < gutter()) {
        handler(index->second, 0, index->second, places - 1);
        return true;
    }
    if (!::core::tui::hasModifier(held, ::core::tui::Modifier::Shift) || !_focusCell) {
        return false;
    }
    std::optional<std::size_t> const anchor = visiblePlace(_focusCell->second);
    int const across = cell.x - gutter();
    // NOLINTNEXTLINE(readability-use-anyofallof): the loop calls the range handler for the first matching column and returns; `any_of` would hide the call in its predicate
    for (DrawnColumn const& column : drawnColumns(_size.width)) {
        std::optional<std::size_t> const place = visiblePlace(column.column);
        if (across >= column.x && across <= column.x + column.width && anchor && place) {
            handler(_focusCell->first, *anchor, index->second, *place);
            return true;
        }
    }
    return false;
}

std::optional<std::size_t> TableImpl::visiblePlace(std::size_t column) const {
    std::vector<std::size_t> const order = visibleOrder();
    auto const found = std::ranges::find(order, column);
    return found == order.end() ? std::nullopt : std::optional{static_cast<std::size_t>(found - order.begin())};
}

void TableImpl::setCellSelection(std::optional<std::array<std::size_t, 4>> rect) {
    _cellRect = rect;
    refresh();
}

void TableImpl::setOnCellSelect(std::function<void(std::size_t, std::size_t, std::size_t, std::size_t)> onSelect) {
    _onCellSelect = std::move(onSelect);
}

bool TableImpl::clickCell(WidgetBase const* row, int x, bool twice) {
    auto const index = _viewRows.find(row);
    if (!_source || !_onCellClick || index == _viewRows.end()) {
        return false;
    }
    int const across = x - gutter();
    // NOLINTNEXTLINE(readability-use-anyofallof): the loop runs the cell handler for the first column under the click; `any_of` would hide the call in its predicate
    for (DrawnColumn const& column : drawnColumns(_size.width)) {
        if (across >= column.x && across <= column.x + column.width) {
            // A copy: the handler may destroy this table.
            auto const handler = _onCellClick;
            std::size_t const viewRow = index->second;
            // The first click of a double click was reported on its own: the second is reported once, as the second.
            handler(viewRow, column.column, twice);
            return true;
        }
    }
    return false;
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
    if (_mode == ui::SelectionMode::None || !rowKey || !row.actionable() || isPassive(&row)) {
        return true;
    }
    if (_mode == ui::SelectionMode::Single || click == ui::RowClick::Only) {
        _anchor = *rowKey;
        return change({*rowKey}, ui::SelectionChange::Replace);
    }
    if (click == ui::RowClick::Range) {
        return change(rangeTo(row), ui::SelectionChange::Replace);
    }
    // A selection of the view: the keys now marked among the rows held, the clicked one toggled.
    if (_viewExcept) {
        std::vector<ui::Key> next;
        for (WidgetBase const* const shown : shownChildren()) {
            auto const key = keyOf(shown);
            if (key && !isPassive(shown) && !std::ranges::contains(*_viewExcept, *key) && *key != *rowKey) {
                next.push_back(*key);
            }
        }
        if (std::ranges::contains(*_viewExcept, *rowKey)) {
            next.push_back(*rowKey);
        }
        _anchor = *rowKey;
        auto const handler = _onSelectionChange;
        if (handler) {
            handler(std::move(next), ui::SelectionChange::Extend);
        }
        return true;
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
        if (auto const betweenKey = keyOf(between); betweenKey && between->actionable() && !isPassive(between)) {
            keys.push_back(*betweenKey);
        }
    }
    return keys;
}

std::vector<ui::Key> TableImpl::allOrNone() const {
    std::vector<ui::Key> keys;
    for (auto const* const row : shownChildren()) {
        if (auto const rowKey = keyOf(row); rowKey && row->actionable() && !isPassive(row)) {
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
    if (row == nullptr || !row->actionable() || isPassive(row)) {
        return;
    }
    auto const handler = _onActivate;
    if (handler) {
        handler(key);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`first`, `count`, `total`): the window then the full row count, passed by name at both call sites in the mount
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
    auto const& base = WidgetBase::of(row);
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
    auto const& base = WidgetBase::of(row);
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

void TableImpl::setCellAccent(ui::Widget& row, std::size_t column, ui::CellAccent const& accent) {
    auto& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setCellAccent of a widget that is not this table's row"};
    }
    if (accent == ui::CellAccent{}) {
        if (auto const found = _accents.find(&base); found != _accents.end()) {
            found->second.erase(column);
        }
    } else {
        _accents[&base].insert_or_assign(column, accent);
    }
    // The cell's own tone wins over its row's: the nearest tone is the one drawn.
    if (auto const* const stack = dynamic_cast<StackImpl*>(&base)) {
        for (auto* const cell : stack->children()) {
            if (columnOf(&base, cell, 0) == column) {
                cell->applyRowTone(accent.tone);
            }
        }
    }
    refresh();
}

void TableImpl::setColumnWindow(
    std::span<std::size_t const> columns, std::span<int const> offsets,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): overrides `Table::setColumnWindow`; the declaration says how its totals are told apart
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

ui::TableMeasure TableImpl::tableMeasure() const {
    return ui::TableMeasure{.unit = 1.0,
                            .monoUnit = 1.0,
                            .contentEstimate = 12,
                            .padding = kColumnGap,
                            .shrinkFloor = kShrunkColumn + kColumnGap};
}

void TableImpl::setOnColumnMeasured(
    std::function<void(std::size_t column, ui::ColumnMeasure const& measure)> handler) {
    _onColumnMeasured = std::move(handler);
    _reportedMeasures.clear();
}

std::vector<std::pair<std::size_t, ui::ColumnMeasure>> TableImpl::grownMeasures() const {
    std::vector<std::pair<std::size_t, ui::ColumnMeasure>> grown;
    if (!_onColumnMeasured) {
        return grown;
    }
    auto const naturals = naturalWidths();
    auto const leasts = leastWidths();
    for (std::size_t column = 0; column < _columns.size(); ++column) {
        ui::ColumnMeasure const before =
            column < _reportedMeasures.size() ? _reportedMeasures.at(column) : ui::ColumnMeasure{};
        // The widths of the cells and the header each carry the gap after the column, as the mount's widths do.
        ui::ColumnMeasure const now{.header = std::max(before.header, headerWidth(_columns.at(column)) + kColumnGap),
                                    .natural = std::max(before.natural, naturals.at(column) + kColumnGap),
                                    .least = std::max(before.least, leasts.at(column) + kColumnGap)};
        if (now != before) {
            grown.emplace_back(column, now);
        }
    }
    return grown;
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the source table's keys share the focus, the selection range and the edit state
EventResult TableImpl::sourceKey(::core::tui::KeyEvent const& key) {
    // The whole view: through the select-all handler when the table has one, else through the mount, which resolves
    // it over the rows not realized; in Single mode it goes on outwards.
    if (matchesChord("Ctrl+A", key) && _mode == ui::SelectionMode::Multiple) {
        if (_onSelectAll) {
            // A copy: the handler may destroy this table.
            auto const handler = _onSelectAll;
            handler();
            return EventResult::Handled;
        }
        if (_total > 0) {
            _anchorRow = 0;
            static_cast<void>(selectRange(0, _total - 1, ui::SelectionChange::Extend));
        }
        return EventResult::Handled;
    }
    bool const upward = matchesChord("Shift+Up", key);
    if ((upward || matchesChord("Shift+Down", key)) && _onFocusMove && !_onCellSelect) {
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
    WidgetBase const* const row = _focusCell ? rowAtView(_focusCell->first) : nullptr;
    if (::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None && row != nullptr && _focusCell) {
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

void TableImpl::setOnText(std::function<void(std::string)> onText) { _onText = std::move(onText); }

void TableImpl::setOnCellClick(std::function<void(std::size_t, std::size_t, bool)> onClick) {
    _onCellClick = std::move(onClick);
}

void TableImpl::setOnFocusOut(std::function<void()> onFocusOut) { _onFocusOut = std::move(onFocusOut); }

void TableImpl::setCellEditor(ui::Widget& row, std::size_t column, ui::Widget* editor) {
    auto const& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setCellEditor of a widget that is not this table's row"};
    }
    if (editor == nullptr) {
        auto const found = _editors.find(&base);
        bool const focused = found != _editors.end() && found->second.second->hasFocus();
        _editors.erase(&base);
        if (focused) {
            takeFocus();
        }
    } else {
        _editors.insert_or_assign(&base, std::pair{column, &WidgetBase::of(*editor)});
    }
    refresh();
}

void TableImpl::setCellMarks(ui::Widget& row, std::size_t column, ui::CellMarks const& marks) {
    auto const& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setCellMarks of a widget that is not this table's row"};
    }
    if (marks == ui::CellMarks{}) {
        if (auto const found = _cellMarks.find(&base); found != _cellMarks.end()) {
            found->second.erase(column);
        }
    } else {
        _cellMarks[&base].insert_or_assign(column, marks);
    }
    refresh();
}

void TableImpl::setRowModified(ui::Widget& row, bool modified) {
    auto const& base = WidgetBase::of(row);
    if (base.container() != this) {
        throw std::logic_error{"morph::tui: setRowModified of a widget that is not this table's row"};
    }
    if (modified) {
        _modifiedRows.insert(&base);
    } else {
        _modifiedRows.erase(&base);
    }
    refresh();
}

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
    auto const& base = WidgetBase::of(row);
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
    if (std::exchange(_focusLeft, false) && _onFocusOut) {
        // A copy: the handler may destroy this table, and then nothing more is reported.
        auto const handler = _onFocusOut;
        Context& held = context();
        WidgetBase* alive = this;
        held.watches.push_back(&alive);
        handler();
        std::erase(held.watches, &alive);
        if (alive == nullptr) {
            return;
        }
    }
    if (!_source || _size.width <= 0) {
        return;
    }
    std::pair<int, int> const columns{_scrollX, viewport()};
    if (_onHorizontalRange && _reportedColumns != columns) {
        _reportedColumns = columns;
        auto const handler = _onHorizontalRange;
        handler(columns.first, columns.second);
    }
    // The mount fits the columns to the table from these, so they go before the rows are asked for.
    if (auto const grown = grownMeasures(); !grown.empty()) {
        _reportedMeasures.resize(_columns.size());
        auto const handler = _onColumnMeasured;
        Context& held = context();
        WidgetBase* alive = this;
        held.watches.push_back(&alive);
        for (auto const& [column, measure] : grown) {
            _reportedMeasures.at(column) = measure;
            handler(column, measure);
            if (alive == nullptr) {
                break;
            }
        }
        std::erase(held.watches, &alive);
        if (alive == nullptr) {
            return;
        }
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
             std::views::zip(std::views::iota(std::size_t{0}), columnWidths(width - menuRoom()))) {
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
    int const room = std::max(0, width - gutter() - menuRoom());
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`row`, `cell`): the row's stack then the cell inside it, as the two callers pass them
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
    if (auto const cells = _loads.find(row); cells != _loads.end()) {
        if (auto const found = cells->second.find(column); found != cells->second.end()) {
            return found->second;
        }
    }
    if (auto const accents = _accents.find(row); accents != _accents.end()) {
        if (auto const found = accents->second.find(column); found != accents->second.end() && found->second.invalid) {
            return ui::CellLoad::Failed;
        }
    }
    return ui::CellLoad::Loaded;
}

std::string TableImpl::focusedTooltip() const {
    if (!_focusCell) {
        return {};
    }
    WidgetBase const* const row = rowAtView(_focusCell->first);
    auto const accents = row != nullptr ? _accents.find(row) : _accents.end();
    if (accents == _accents.end()) {
        return {};
    }
    auto const found = accents->second.find(_focusCell->second);
    return found == accents->second.end() ? std::string{} : found->second.tooltip;
}

// A cell takes its column's slot; a pooled cell, one whose column is not drawn and one a mark stands for take none.
// A heading's one cell takes every column drawn.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): assigns every cell its slot in one pass over the rows; pooled, undrawn, marked and heading cells each take none or all of the columns
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
        auto const editor = _editors.find(row);
        for (auto* const cell : stack->children()) {
            CellSlot slot;
            auto const column = columnOf(row, cell, 0);
            if (editor != _editors.end() && (cell == editor->second.second || column == editor->second.first)) {
                // An open editor is drawn in its cell's slot, the cell it edits in none.
                if (auto const found = byColumn.find(editor->second.first);
                    cell == editor->second.second && found != byColumn.end()) {
                    slot = CellSlot{.x = found->second->x,
                                    .width = found->second->width,
                                    .place = CellPlace{.across = ui::Align::Stretch, .down = ui::Align::Center}};
                }
            } else if (isHeading(row)) {
                if (!_cellColumns.contains(row) || !_cellColumns.at(row).contains(cell)) {
                    slot = CellSlot{.x = 0, .width = across, .place = CellPlace{.across = ui::Align::Start}};
                }
            } else if (auto const found = column ? byColumn.find(*column) : byColumn.end();
                       column && found != byColumn.end() && loadOf(row, *column) == ui::CellLoad::Loaded) {
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
    return std::max(0, height - headerLines() - (_status.shown ? 1 : 0));
}

int TableImpl::viewport() const noexcept { return std::max(0, _size.width - gutter() - menuRoom()); }

std::optional<::core::tui::Rect> TableImpl::headerLabelArea(std::size_t column) const {
    if (!_showHeader || column >= _columns.size()) {
        return std::nullopt;
    }
    for (DrawnColumn const& drawn : drawnColumns(_size.width)) {
        if (drawn.column == column) {
            int const button = _columns.at(column).filterable ? kFilterMark : 0;
            return ::core::tui::Rect{.x = gutter() + drawn.x,
                                     .y = headerLines() - 1,
                                     .width = std::max(0, drawn.width - button),
                                     .height = 1};
        }
    }
    return std::nullopt;
}

int TableImpl::menuRoom() const noexcept { return _menu && _showHeader ? kTableMenuRoom : 0; }

bool TableImpl::headerActs() const {
    return _menu || _onColumnMove ||
           std::ranges::any_of(_columns, [](ui::TableColumn const& column) { return column.sortable; });
}

// A right press on a header opens its menu; on the header line's `⋯`, the table's.
bool TableImpl::contextPress(::core::tui::Point cell) {
    // A right press on a row's cell opens the cell's menu.
    if (_cellMenu && cell.y >= headerLines()) {
        WidgetBase const* const row = rowAtLine(cell.y);
        if (row == nullptr) {
            return false;
        }
        // NOLINTNEXTLINE(readability-use-anyofallof): the loop opens the cell menu for the first column under the click; `any_of` would hide the call in its predicate
        for (DrawnColumn const& drawn : drawnColumns(_size.width)) {
            int const left = gutter() + drawn.x;
            if (cell.x >= left && cell.x < left + drawn.width) {
                openCellMenu(*row, drawn.column);
                return true;
            }
        }
        return false;
    }
    if (!_menu || !_showHeader || cell.y != headerLines() - 1) {
        return false;
    }
    if (cell.x == _size.width - 1) {
        openMenu(std::nullopt);
        return true;
    }
    // NOLINTNEXTLINE(readability-use-anyofallof): the loop opens the column menu for the first header under the click; `any_of` would hide the call in its predicate
    for (DrawnColumn const& drawn : drawnColumns(_size.width)) {
        int const left = gutter() + drawn.x;
        if (cell.x >= left && cell.x < left + drawn.width) {
            openMenu(drawn.column);
            return true;
        }
    }
    return false;
}

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
    bool const measuresDue = !grownMeasures().empty();
    if (!columnsDue && !rowsDue && !measuresDue) {
        return;
    }
    if (std::ranges::find(context().windowTables, this) == context().windowTables.end()) {
        context().windowTables.push_back(this);
    }
    refresh();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): draws the status text, the retry button and the gap in one left-to-right pass over a running column
void TableImpl::paintStatus(::core::tui::Canvas& canvas) {
    _retryX = -1;
    _entriesDrawn.clear();
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
        canvas.putString(line, x, kRetry, theme.textAccent);
        x += displayWidth(kRetry);
    }
    // What stays while its cause lasts, then each choice as `[Alt+<n> label]`, the key that chooses it.
    auto const entries = [&](std::vector<ui::StatusEntry> const& list) {
        for (ui::StatusEntry const& entry : list) {
            std::string const drawn = "[Alt+" + std::to_string(_entriesDrawn.size() + 1) + " " + entry.label + "]";
            x += x == 0 ? 0 : displayWidth(kStatusGap);
            canvas.putString(line, x, drawn, theme.textAccent);
            _entriesDrawn.push_back(DrawnEntry{.x = x, .width = displayWidth(drawn), .entryId = entry.id});
            x += displayWidth(drawn);
        }
    };
    if (!_status.standing.empty()) {
        x += x == 0 ? 0 : displayWidth(kStatusGap);
        canvas.putString(line, x, _status.standing, theme.textNormal);
        x += displayWidth(_status.standing);
    }
    entries(_status.standingEntries);
    if (!_status.entries.empty()) {
        if (!_status.message.empty()) {
            x += x == 0 ? 0 : displayWidth(kStatusGap);
            canvas.putString(line, x, _status.message, theme.textNormal);
            x += displayWidth(_status.message);
        }
        entries(_status.entries);
        return;
    }
    // The latest message, else the focused cell's tooltip: a terminal has no hover to show it on.
    std::string const message = _status.message.empty() ? focusedTooltip() : _status.message;
    if (!message.empty()) {
        // At the trailing edge, or after the rest when the line is too short for both.
        int const width = displayWidth(message);
        int const gap = x == 0 ? 0 : displayWidth(kStatusGap);
        canvas.putString(line, std::max(x + gap, canvas.width() - width), message, theme.textNormal);
    }
}

void TableImpl::paintLoads(::core::tui::Canvas& canvas, std::vector<DrawnColumn> const& drawn,
                           std::vector<::core::tui::Rect> const& areas) const {
    auto const& theme = canvas.theme();
    for (auto const [row, area] : std::views::zip(children(), areas)) {
        if (area.empty() || isHeading(row)) {
            continue;
        }
        for (DrawnColumn const& column : drawn) {
            ui::CellLoad const load = loadOf(row, column.column);
            if (load == ui::CellLoad::Loaded) {
                continue;
            }
            bool const failed = load == ui::CellLoad::Failed;
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
