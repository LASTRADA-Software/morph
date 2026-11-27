// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/mount.hpp>.

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
#include <morph/attributes.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/stretch_columns.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <morph/ui/view.hpp>
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

namespace morph::ui::detail {

std::optional<std::int64_t> Mounter::followIndex(table::ViewChange const& change, std::int64_t index) {
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

void Mounter::moveFocus(SourceTable& table, FocusMove move) {
    TableTotal const total = table.source.rows->mountedTotal();
    std::int64_t const last = std::max<std::int64_t>(total.rows - 1, 0);
    std::int64_t const page = std::max<std::int64_t>(static_cast<std::int64_t>(focusRangeOf(table).count), 1);
    std::int64_t index = table.focused ? table.focusIndex : 0;
    std::optional<std::size_t> const focusPlace =
        table.focused && table.focusColumn < table.placeOf.size() ? table.placeOf.at(table.focusColumn) : std::nullopt;
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

std::optional<std::size_t> Mounter::viewRowOf(SourceTable const& table, Key const& key) {
    TableTotal const total = table.source.rows->mountedTotal();
    for (std::int64_t index = 0; index < total.rows; ++index) {
        std::optional<TableRowRef> const ref = table.source.rows->row(index);
        if (ref && ref->key == key) {
            return static_cast<std::size_t>(index);
        }
    }
    return std::nullopt;
}

void Mounter::showCells(SourceTable& table) {
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
                std::min(*anchorRow, *focusRow), std::max(*anchorRow, *focusRow) - std::min(*anchorRow, *focusRow) + 1,
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

std::optional<CellRange> Mounter::cellsAt(
    SourceTable const& table, std::size_t anchorRow,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`anchorPlace`, `focusRow`): the selection's anchor then its focus, passed in that order by the one caller
    std::size_t anchorPlace, std::size_t focusRow, std::size_t focusPlace) {
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

void Mounter::handFocus(SourceTable const& table, std::int64_t index, std::size_t column) {
    if (!table.onGridFocus || column >= table.columns.size()) {
        return;
    }
    std::optional<TableRowRef> const ref = table.source.rows->row(index);
    table.onGridFocus(GridFocus{.key = ref ? std::optional<Key>{ref->key} : std::nullopt,
                                .viewRow = static_cast<std::size_t>(std::max<std::int64_t>(index, 0)),
                                .column = table.columns.at(column).id});
}

void Mounter::gridFocusFrom(SourceTable& table, std::optional<GridFocus> const& focus) {
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`index`, `column`): row index then column, the order every focus seam in this file uses
void Mounter::placeFocus(SourceTable& table, std::int64_t index, std::size_t column) {
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

void Mounter::focusFrom(SourceTable& table, std::optional<CellRef> const& cell) {
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

void Mounter::focusAt(SourceTable& table, Key const& key, std::size_t column) {
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

void Mounter::applyKeyMoves(SourceTable& table, KeyMoves const& moves) {
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

void Mounter::highlightFrom(SourceTable& table, std::optional<TableHighlight> const& highlight) {
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

std::optional<std::int64_t> Mounter::placeOfKey(SourceTable const& table, Key const& key, std::int64_t also) {
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): follows the focused and the highlighted rows through each operation of a change, which shifts both by the operation kind
void Mounter::follow(SourceTable& table, table::ViewChange const& change) {
    bool const reset =
        std::ranges::any_of(change.ops, [](table::ViewOp const& op) { return op.kind == table::ViewOp::Kind::Reset; });
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

bool Mounter::namesUpdated(table::ViewChange const& change, std::int64_t index) {
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): binds what an editable table adds (focus, click, typed text, keys, focus loss), each handler capturing the same table state
void Mounter::bindGrid(reactive::Scope& scope, Table const& spec, TableWidget& widget, SourceTable& table) {
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
        widget.setOnCellClick(
            [this, &table, activate = spec.onActivateCell, clicked = spec.onClickCell](
                // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`viewRow`, `column`, `doubleClick`): the backend callback's contract, in the order the conformance suite fires it
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one handler per table seam.
Widget& Mounter::mountSourceTable(reactive::Scope& scope, Table const& spec, ContainerWidget* parent) {
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
            static_cast<void>(runCallback(*_rt, [&] { realizeWindow(table, [](std::int64_t) { return false; }); }));
        }
    };
    bind(scope, spec.arrangement, [&table, pass](TableArrangement const& arrangement) {
        if (arrangement != table.arrangement) {
            arrange(table, arrangement);
            pass();
            showCells(table);
        }
    });
    table.source.rows->onChange([this, &table, alive = std::weak_ptr{table.alive}](table::ViewChange const& change) {
        if (alive.expired()) {
            return;
        }
        ++_stats.viewChanges;
        if (std::ranges::any_of(change.ops,
                                [](table::ViewOp const& op) { return op.kind == table::ViewOp::Kind::Reset; })) {
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
    widget.setOnSelectionChange(inputEvent(
        spec.onSelectionChange, controlled(scope, spec.selection, [&widget, &viewing](std::vector<Key> const& keys) {
            if (!viewing) {
                widget.setSelection(keys);
            }
        })));
    bindSelectionView(scope, spec, widget, viewing);
    return widget;
}

void Mounter::measured(SourceTable& table, std::size_t column, ColumnMeasure const& reported) {
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
    std::optional<std::size_t> const placed = column < table.placeOf.size() ? table.placeOf.at(column) : std::nullopt;
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

void Mounter::mountRows(reactive::Scope& scope, std::shared_ptr<ForEachModel const> const& model,
                        ContainerWidget& container, RowHook onRow, Virtualization const* virtualize,
                        RowsObserver observer) {
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
                    reconcileWindowed(depth, session, rows, container, *table, keys, onRow, window, setting, windowed);
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
            underOverlay(overlay, [&] { applyWindow(depth, rows.list, container, window, setting.overscan, onRow); });
        });
        auto const [first, count] = realizedExtent(rows.list);
        table->setRealizedWindow(first, count, rows.list.size());
    });
}

std::vector<std::size_t> Mounter::firstOccurrences(std::vector<Key> const& keys) {
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

KeyedRow Mounter::mountRow(std::size_t depth, ForEachSession& session, ContainerWidget& container, Key const& key,
                           std::size_t index, RowHook const& onRow) {
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

void Mounter::realizeRowWidget(std::size_t depth, KeyedRow& row, ContainerWidget& container, RowHook const& onRow) {
    row.widgetScope = std::make_unique<reactive::Scope>(*_rt, depth);
    row.widget = mount(*row.widgetScope, row.slot->view(), &container);
    row.widgetState = row.widget != nullptr ? RowWidgetState::Realized : RowWidgetState::NullView;
    if (row.widget != nullptr && onRow) {
        onRow(*row.widgetScope, row.slot->view(), *row.widget, row.key);
    }
}

void Mounter::dewindowRowWidget(KeyedRow& row) {
    row.widgetScope.reset();
    row.widget = nullptr;
    row.widgetState = RowWidgetState::Dewindowed;
}

KeyedRow Mounter::mountRowState(std::size_t depth, ForEachSession& session, Key const& key, std::size_t index) {
    KeyedRow row{.key = key, .stateScope = std::make_unique<reactive::Scope>(*_rt, depth)};
    row.slot = &row.stateScope->adopt(session.makeRow(index));
    row.widgetState = RowWidgetState::Dewindowed;
    return row;
}

void Mounter::reorder(std::vector<KeyedRow>& rows, ContainerWidget& container, std::vector<Key> const& keys,
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

void Mounter::reconcile(std::size_t depth, ForEachSession& session, std::vector<KeyedRow>& rows,
                        ContainerWidget& container, std::vector<Key> const& keys, RowHook const& onRow,
                        std::pair<VisibleRange, std::size_t> const* windowAndOverscan) {
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

void Mounter::applyWindow(std::size_t depth, std::vector<KeyedRow>& rows, ContainerWidget& container,
                          VisibleRange const& window, std::size_t overscan, RowHook const& onRow) {
    std::size_t const low = window.first > overscan ? window.first - overscan : 0;
    std::size_t const high =
        window.first + window.count + overscan;  // exclusive; saturates harmlessly past rows.size()
    std::exception_ptr failure;
    for (KeyedRow& row : rows) {
        bool const shouldBeRealized = row.viewPosition >= low && row.viewPosition < high;
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
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from Emscripten's libc++.
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(order, {}, [&rows](std::size_t index) { return rows.at(index).viewPosition; });
    reorder(rows, container, keys, order, failure);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

std::pair<std::size_t, std::size_t> Mounter::realizedExtent(std::vector<KeyedRow> const& rows) {
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

Widget& Mounter::mountKind(reactive::Scope& scope, Busy const& spec, ContainerWidget* parent) {
    BusyWidget& widget = scope.adopt(_backend->createBusy(parent));
    applyCommon(scope, widget, spec.common);
    bind(scope, spec.active, [&widget](bool active) { widget.setActive(active); });
    bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Banner const& spec, ContainerWidget* parent) {
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

Widget& Mounter::mountKind(reactive::Scope& scope, Badge const& spec, ContainerWidget* parent) {
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
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks): the analyzer loses the owner of the callable `std::function` heap-allocates for this capture; the function object owns it and frees it
        bind(scope, label, [&widget, onRemove = event(spec.onRemove)](std::string const& shown) {
            widget.setOnRemove(onRemove, shown);
        });
    }
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Progress const& spec, ContainerWidget* parent) {
    ProgressWidget& widget = scope.adopt(_backend->createProgress(parent));
    applyCommon(scope, widget, spec.common);
    bind(scope, spec.value, [&widget](std::optional<double> value) { widget.setValue(value); });
    bind(scope, spec.label, [&widget](std::string const& label) { widget.setLabel(label); });
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Steps const& spec, ContainerWidget* parent) {
    StepsWidget& widget = scope.adopt(_backend->createSteps(parent));
    applyCommon(scope, widget, spec.common);
    bind(scope, spec.items, [&widget](std::vector<Step> const& steps) { widget.setSteps(steps); });
    bind(scope, spec.current, [&widget](std::optional<std::size_t> index) { widget.setCurrent(index); });
    if (spec.labelled.isBound() || !spec.labelled.constant()) {
        bind(scope, spec.labelled, [&widget](bool labelled) { widget.setLabelled(labelled); });
    }
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Chart const& spec, ContainerWidget* parent) {
    ChartWidget& widget = scope.adopt(_backend->createChart(parent));
    applyCommon(scope, widget, spec.common);
    bind(scope, spec.data, [&widget](ChartData const& data) { widget.setData(data); });
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, KeyValue const& spec, ContainerWidget* parent) {
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

Widget& Mounter::mountKind(reactive::Scope& scope, EmptyState const& spec, ContainerWidget* parent) {
    EmptyStateWidget& widget = scope.adopt(_backend->createEmptyState(parent));
    applyCommon(scope, widget, spec.common);
    bind(scope, spec.title, [&widget](std::string const& title) { widget.setTitle(title); });
    bind(scope, spec.text, [&widget](std::string const& text) { widget.setText(text); });
    widget.setIcon(spec.icon);
    bind(scope, spec.action.label, [&widget](std::string const& label) { widget.setActionLabel(label); });
    widget.setOnAction(event(spec.action.onClick));
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Splitter const& spec, ContainerWidget* parent) {
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

Widget& Mounter::mountKind(reactive::Scope& scope, Collapsible const& spec, ContainerWidget* parent) {
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

Widget& Mounter::mountKind(reactive::Scope& scope, DropZone const& spec, ContainerWidget* parent) {
    DropZoneWidget& widget = scope.adopt(_backend->createDropZone(parent));
    applyCommon(scope, widget, spec.common);
    widget.setAccept(spec.accept);
    widget.setMultiple(spec.multiple);
    widget.setOnDrop(event(spec.onDrop));
    static_cast<void>(mount(scope, spec.child, &widget));
    return widget;
}

std::logic_error Mounter::pageRefusal(Common const& common, std::string const& what) {
    return std::logic_error{"morph::ui::mount: " + what +
                            (common.testId.empty() ? std::string{} : " (node `" + common.testId + "`)")};
}

void Mounter::checkPageLayout(Common const& common, bool isPage) {
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

Widget& Mounter::mountKind(reactive::Scope& scope, Page const& spec, ContainerWidget* parent) {
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

Widget& Mounter::mountKind(reactive::Scope& scope, PageRegion const& spec, ContainerWidget* parent) {
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

void Mounter::bindCustomProp(reactive::Scope& scope, CustomWidget& widget, CustomProp const& prop) {
    std::string const& name = prop.name;
    if (auto const* const text = std::get_if<Prop<std::string>>(&prop.value)) {
        bind(scope, *text, [&widget, name](std::string const& value) { widget.setTextProp(name, value); });
    } else if (auto const* const flag = std::get_if<Prop<bool>>(&prop.value)) {
        bind(scope, *flag, [&widget, name](bool value) { widget.setBoolProp(name, value); });
    } else if (auto const* const number = std::get_if<Prop<double>>(&prop.value)) {
        bind(scope, *number, [&widget, name](double value) { widget.setNumberProp(name, value); });
    }
}

Widget& Mounter::mountKind(reactive::Scope& scope, ActionBar const& spec, ContainerWidget* parent) {
    ContainerWidget* const footer = overlayFooter();
    if (spec.wrap && footer != nullptr) {
        throw std::invalid_argument{
            "morph::ui::Mounted: an overlay's footer is one row of actions; `wrap` is for an action bar in place"};
    }
    StackWidget& widget = scope.adopt(_backend->createStack(footer != nullptr ? footer : parent, Axis::Horizontal));
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

Widget& Mounter::mountKind(reactive::Scope& scope, Custom const& spec, ContainerWidget* parent) {
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

Widget& Mounter::mountKind(reactive::Scope& scope, DateTimeInput const& spec, ContainerWidget* parent) {
    DateTimeInputWidget& widget = scope.adopt(_backend->createDateTimeInput(parent, spec.mode, spec.offsetMinutes));
    applyCommon(scope, widget, spec.common);
    applyField(scope, widget, spec.field);
    // Before the value, which is then shown in it; an input with no pattern is told none.
    if (spec.pattern.isBound() || !spec.pattern.constant().empty()) {
        bind(scope, spec.pattern, [&widget](std::string const& pattern) { widget.setPattern(pattern); });
    }
    widget.setOnChange(inputEvent(
        spec.onChange, controlled(scope, spec.value, [&widget](std::optional<morph::time::Timestamp> const& value) {
            widget.setValue(value);
        })));
    applyCaption(scope, widget, spec.label, spec.showLabel);
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, Slider const& spec, ContainerWidget* parent) {
    SliderWidget& widget = scope.adopt(_backend->createSlider(parent));
    applyCommon(scope, widget, spec.common);
    applyField(scope, widget, spec.field);
    widget.setRange(spec.minimum, spec.maximum, spec.step);
    widget.setOnChange(inputEvent(
        spec.onChange, controlled(scope, spec.value, [&widget](std::int64_t value) { widget.setValue(value); })));
    widget.setOnCommit(event(spec.onCommit));
    return widget;
}

Widget& Mounter::mountKind(reactive::Scope& scope, FilePicker const& spec, ContainerWidget* parent) {
    FilePickerWidget& widget = scope.adopt(_backend->createFilePicker(parent, spec.mode));
    applyCommon(scope, widget, spec.common);
    applyField(scope, widget, spec.field);
    widget.setOnPicked(inputEvent(
        spec.onPicked, controlled(scope, spec.path, [&widget](std::string const& path) { widget.setPath(path); })));
    return widget;
}

}  // namespace morph::ui::detail

namespace morph::ui {

Node Mounted::nonNull(Node root) {
    if (!root) {
        throw std::invalid_argument{"morph::ui::Mounted: the root node is null"};
    }
    return root;
}

}  // namespace morph::ui
