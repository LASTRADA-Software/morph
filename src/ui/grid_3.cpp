// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/grid.hpp>.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/forms/engine/field_control.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/edits.hpp>
#include <morph/ui/cell_format.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/form_rows.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_config.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

void GridController::deleteRows() {
    std::vector<Key> doomed = _rt->untracked([&] { return selectedRows(); });
    if (doomed.empty()) {
        if (std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); })) {
            doomed.push_back(focused->key);
        }
    }
    removeKeys(doomed);
}

void GridController::undo() {
    commit();
    letGo();
    if (!_undoOffered) {
        return;
    }
    std::size_t const before = _rt->untracked([&] { return _source->heldRows(); });
    _source->undoRemove();
    clearUndo();
    if (_rt->untracked([&] { return _source->heldRows(); }) != before) {
        _source->committed();
    }
}

RowOps GridController::rowOps() const {
    if (fixedRows()) {
        return {};
    }
    std::string const insert = insertRefusal();
    std::string const remove = removeRefusal(1);
    bool const anyRow = _source->heldRows() > 0;
    return RowOps{
        .insert = insert.empty(), .remove = remove.empty() && anyRow, .why = !insert.empty() ? insert : remove};
}

void GridController::addRow() {
    commit();
    letGo();
    if (fixedRows()) {
        return;
    }
    if (std::optional<Key> const key = _source->appendRow()) {
        noteChange();
        _source->committed();
        focusFirstEditable(*key);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one wire per seam of the table.
Node GridController::node(std::vector<TableColumn> columns, TableOptions options, std::optional<TableSource> base) {
    std::shared_ptr<GridController> const keep = weak_from_this().lock();
    GridController* const self = this;
    GridSource* const source = _source;
    auto const ids = std::make_shared<std::vector<std::string>>();
    for (TableColumn const& column : columns) {
        ids->push_back(column.id);
    }
    auto const idOf = [ids](std::size_t column) { return column < ids->size() ? ids->at(column) : std::string{}; };
    auto const marksOf = [source](reactive::Signal<Value> const* row, std::string const& columnId) {
        return Prop<CellMarks>{[source, row, columnId] {
            std::optional<Key> const key = source->keyOf(row->get());
            return key ? source->marks(*key, columnId) : CellMarks{};
        }};
    };
    TableSource made;
    if (base && base->cellView) {
        made = *base;
        made.cellView = [inner = base->cellView, idOf, marksOf](std::size_t column,
                                                                reactive::Signal<Value> const& row) {
            return markedCell(inner(column, row), marksOf(&row, idOf(column)));
        };
    } else {
        if (base) {
            made = *base;
        } else {
            made.rows = std::shared_ptr<TableRows>(keep, &source->view());
            made.rowView = {};
            made.alwaysFetched = {};
            made.levels = {};
        }
        made.cellView = [source, idOf](std::size_t column, reactive::Signal<Value> const& row) {
            return source->cellView(idOf(column), row);
        };
    }
    if (source->spanning()) {
        // A grid's rows are all on the client: under the threshold every row is realized, and the spans drawn.
        made.alwaysWindowed = false;
    }
    made.rowView = [inner = made.rowView, source, ids](reactive::Signal<Value> const& row) {
        TableRowLook look = inner ? inner(row) : TableRowLook{};
        reactive::Signal<Value> const* const record = &row;
        look.modified = Prop<bool>{[source, record] {
            std::optional<Key> const key = source->keyOf(record->get());
            return key.has_value() && source->rowModified(*key);
        }};
        if (source->spanning() && !look.spans) {
            look.spans = Prop<std::vector<RowSpan>>{[source, record, ids] {
                std::vector<RowSpan> out;
                std::optional<Key> const key = source->keyOf(record->get());
                if (!key) {
                    return out;
                }
                for (CellSpan const& span : source->spans(*key)) {
                    auto const found = std::ranges::find(*ids, span.column);
                    if (found != ids->end()) {
                        out.push_back(RowSpan{.column = static_cast<std::size_t>(found - ids->begin()),
                                              .rows = span.rows,
                                              .columns = span.columns});
                    }
                }
                return out;
            }};
        }
        return look;
    };
    // Over summary rows the mount counts mounted rows and the grid data rows: a focus's view row is mapped
    // between the two, and a click, a move or a rectangle's corner that lands on a summary or heading row goes to
    // the nearest data row.
    auto const summaries = std::dynamic_pointer_cast<SummaryRows>(made.rows);
    options.gridFocus = Prop<std::optional<GridFocus>>{[self, summaries] {
        std::optional<GridFocus> shown = self->place();
        if (shown && summaries != nullptr) {
            if (std::optional<std::int64_t> const mounted =
                    summaries->mounted(static_cast<std::int64_t>(shown->viewRow))) {
                shown->viewRow = static_cast<std::size_t>(*mounted);
            }
        }
        return shown;
    }};
    auto const toData = [source, summaries](Key const& key) -> std::optional<Key> {
        if (summaries == nullptr || source->positionOf(key)) {
            return key;
        }
        std::int64_t const count = summaries->mountedTotal().rows;
        for (std::int64_t at = 0; at < count; ++at) {
            std::optional<TableRowRef> const ref = summaries->row(at);
            if (ref && ref->key == key) {
                std::optional<std::int64_t> const data = summaries->data(at);
                return data ? source->keyAt(static_cast<std::size_t>(*data)) : std::nullopt;
            }
        }
        return std::nullopt;
    };
    // The table's `onFocusCell` hears where a click or a move put the grid's focus, as a table's does.
    options.onGridFocus = [self, keep, source, summaries, toData,
                           heard = std::move(options.onFocusCell)](GridFocus target) {
        if (summaries != nullptr) {
            if (target.key) {
                target.key = toData(*target.key);
                if (!target.key) {
                    return;
                }
            }
            std::optional<std::int64_t> const data = summaries->data(static_cast<std::int64_t>(target.viewRow));
            target.viewRow = data ? static_cast<std::size_t>(*data) : target.viewRow;
            if (target.key) {
                target.viewRow = source->positionOf(*target.key).value_or(target.viewRow);
            }
        }
        self->focusPlace(std::move(target));
        if (heard) {
            heard(self->_rt->untracked([&] { return self->focus(); }));
        }
    };
    options.onKey = [self](std::string const& chord) { return self->press(chord); };
    options.keyChords = Prop<std::vector<std::string>>{[self] { return self->keyChords(); }};
    options.onText = [self](std::string const& text) { self->type(text); };
    options.onActivateCell = [self](CellRef cell) { self->activate(std::move(cell)); };
    options.onClickCell = [self, toData](CellRef cell) {
        if (std::optional<Key> const key = toData(cell.key)) {
            self->click(CellRef{.key = *key, .column = std::move(cell.column)});
        }
    };
    options.onFocusOut = [self] { self->leave(); };
    options.editing = Prop<std::optional<CellRef>>{[self] { return self->editing(); }};
    options.editor = [source](CellRef const& cell) { return source->editor(cell.key, cell.column); };
    // A model's table selects rows: its selection gestures stay the table's own.
    if (source->selectsCells()) {
        options.cellSelection = Prop<std::optional<CellRange>>{[self] { return self->selection(); }};
        options.onCellSelect = [self, toData](std::optional<CellRange> range) {
            if (range) {
                std::optional<Key> const anchor = toData(range->anchor.key);
                std::optional<Key> const focus = toData(range->focus.key);
                if (!anchor || !focus) {
                    return;
                }
                range->anchor.key = *anchor;
                range->focus.key = *focus;
            }
            self->select(std::move(range));
        };
    }
    options.cellMenu = [self, idOf, outer = std::move(options.cellMenu)](Key const& row, std::size_t column) {
        std::vector<MenuItem> own = self->cellEntries(row, idOf(column));
        if (!outer) {
            return own;
        }
        std::vector<MenuItem> items = outer(row, column);
        auto const separator = std::ranges::find_if(items, [](MenuItem const& item) { return item.separator; });
        items.insert(separator, std::make_move_iterator(own.begin()), std::make_move_iterator(own.end()));
        return items;
    };
    if (!options.status.isBound() && options.status.constant() == TableStatus{}) {
        options.status = Prop<TableStatus>{[self] {
            TableStatus status{.message = self->message(),
                               .entries = self->messageEntries(),
                               .standing = self->standing(),
                               .standingEntries = self->standingEntries()};
            status.shown = !status.message.empty() || !status.entries.empty() || !status.standing.empty() ||
                           !status.standingEntries.empty();
            return status;
        }};
        if (!options.onStatusEntry) {
            options.onStatusEntry = [self](std::string const& entryId) { self->chooseEntry(entryId); };
        }
    }
    if (!_options.appendRows || fixedRows()) {
        return ui::table(std::move(columns), std::move(made), std::move(options));
    }
    // "Add row" under the grid, disabled at `maxItems` with the bound as its tooltip.
    Button add;
    add.label = std::string{"Add row"};
    add.onClick = [self, keep] { self->addRow(); };
    add.common.testId = options.common.testId.empty() ? std::string{} : options.common.testId + "_addRow";
    add.common.enabled = Prop<bool>{[self] { return self->rowOps().insert; }};
    add.common.tooltip = Prop<std::string>{[self] { return self->insertRefusal(); }};
    std::vector<Node> under;
    under.push_back(ui::button(std::move(add)));
    return besideTable(options.common, std::move(under),
                       [&] { return ui::table(std::move(columns), std::move(made), std::move(options)); });
}

std::optional<GridController::Block> GridController::block() const {
    std::optional<CellRange> range = _selection.peek();
    if (!range) {
        std::optional<CellRef> const focused = focus();
        if (!focused) {
            return std::nullopt;
        }
        range = CellRange{.anchor = *focused, .focus = *focused};
    }
    std::vector<std::string> const columns = _source->columns();
    std::optional<std::size_t> const anchorRow = _source->positionOf(range->anchor.key);
    std::optional<std::size_t> const focusRow = _source->positionOf(range->focus.key);
    auto const anchorColumn = std::ranges::find(columns, range->anchor.column);
    auto const focusColumn = std::ranges::find(columns, range->focus.column);
    if (!anchorRow || !focusRow || anchorColumn == columns.end() || focusColumn == columns.end()) {
        return std::nullopt;
    }
    Block out;
    for (std::size_t row = std::min(*anchorRow, *focusRow); row <= std::max(*anchorRow, *focusRow); ++row) {
        if (std::optional<Key> const key = _source->keyAt(row)) {
            out.keys.push_back(*key);
        }
    }
    out.columns.assign(std::min(anchorColumn, focusColumn), std::max(anchorColumn, focusColumn) + 1);
    return out;
}

bool GridController::inBlock(CellRef const& cell) const {
    std::optional<Block> const cells = _rt->untracked([&] { return block(); });
    return cells && std::ranges::contains(cells->keys, cell.key) && std::ranges::contains(cells->columns, cell.column);
}

std::vector<Key> GridController::selectedRows() const {
    if (!_selection.peek()) {
        return {};
    }
    std::optional<Block> const cells = block();
    return cells ? cells->keys : std::vector<Key>{};
}

void GridController::extend(int rows, int columns) {
    if (!_source->selectsCells()) {
        return;
    }
    std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); });
    if (!focused) {
        return;
    }
    CellRange range = _selection.peek().value_or(CellRange{.anchor = *focused, .focus = *focused});
    std::vector<std::string> const ids = _source->columns();
    std::optional<std::size_t> const row = _source->positionOf(range.focus.key);
    auto const column = std::ranges::find(ids, range.focus.column);
    std::size_t const count = _source->rowCount();
    if (!row || column == ids.end() || count == 0) {
        return;
    }
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`index`, `delta`, `size`): the lambda is called for the row axis and the column axis, each time with the position, the signed step and the extent in that order
    auto const clamp = [](std::size_t index, int delta, std::size_t size) {
        auto const moved = static_cast<std::int64_t>(index) + delta;
        return static_cast<std::size_t>(std::clamp<std::int64_t>(moved, 0, static_cast<std::int64_t>(size) - 1));
    };
    std::optional<Key> const key = _source->keyAt(clamp(*row, rows, count));
    if (!key) {
        return;
    }
    range.focus = CellRef{
        .key = *key, .column = ids.at(clamp(static_cast<std::size_t>(column - ids.begin()), columns, ids.size()))};
    setSelection(std::move(range));
}

void GridController::selectAll() {
    if (!_source->selectsCells()) {
        return;
    }
    std::vector<std::string> const ids = _source->columns();
    std::size_t const count = _source->rowCount();
    if (ids.empty() || count == 0) {
        return;
    }
    std::optional<Key> const first = _source->keyAt(0);
    std::optional<Key> const last = _source->keyAt(count - 1);
    if (first && last) {
        setSelection(CellRange{.anchor = CellRef{.key = *first, .column = ids.front()},
                               .focus = CellRef{.key = *last, .column = ids.back()}});
    }
}

std::vector<std::vector<std::string>> GridController::pasteLines(std::string_view text) {
    std::vector<std::string_view> lines;
    for (std::size_t start = 0;;) {
        std::size_t const end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    for (std::string_view& line : lines) {
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
    }
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    std::vector<std::vector<std::string>> out;
    for (std::string_view const line : lines) {
        std::vector<std::string> values;
        for (std::size_t start = 0;;) {
            std::size_t const end = line.find('\t', start);
            std::string_view value =
                line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            value.remove_prefix(std::min(value.find_first_not_of(' '), value.size()));
            std::size_t const last = value.find_last_not_of(' ');
            value = last == std::string_view::npos ? std::string_view{} : value.substr(0, last + 1);
            values.emplace_back(value);
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        out.push_back(std::move(values));
    }
    return out;
}

std::string GridController::insertRefusal() const {
    std::optional<std::size_t> const most = _source->bounds().second;
    if (most && _source->heldRows() >= *most) {
        return "A grid holds at most " + std::to_string(*most) + " rows";
    }
    return {};
}

std::string GridController::removeRefusal(std::size_t count) const {
    std::size_t const fewest = _source->bounds().first;
    if (_source->heldRows() < count + fewest) {
        return "A grid holds at least " + std::to_string(fewest) + " rows";
    }
    return {};
}

void GridController::insertAt(Key const& row, bool below) {
    commit();
    letGo();
    if (fixedRows() || !_rt->untracked([&] { return insertRefusal(); }).empty()) {
        return;
    }
    std::optional<std::size_t> const position = _source->positionOf(row);
    if (!position) {
        return;
    }
    if (_source->insertRow(below ? *position + 1 : *position)) {
        noteChange();
        _source->committed();
    }
}

void GridController::removeKeys(std::vector<Key> const& keys) {
    commit();
    letGo();
    if (fixedRows() || keys.empty() || !_rt->untracked([&] { return removeRefusal(keys.size()); }).empty()) {
        return;
    }
    std::optional<GridFocus> const shown = _rt->untracked([&] { return place(); });
    std::optional<std::size_t> const position = shown && shown->key ? _source->positionOf(*shown->key) : std::nullopt;
    noteChange();
    bool const held = _source->removeRows(keys);
    _source->committed();
    if (std::optional<CellRange> const range = _selection.peek();
        range && (!_source->positionOf(range->anchor.key) || !_source->positionOf(range->focus.key))) {
        setSelection(std::nullopt);
    }
    if (shown && shown->key && !_source->positionOf(*shown->key)) {
        std::size_t const rows = _source->rowCount();
        std::optional<Key> const next =
            rows == 0 ? std::nullopt : _source->keyAt(std::min(position.value_or(0), rows - 1));
        if (next) {
            focusCell(CellRef{.key = *next, .column = shown->column});
        } else {
            setPlace(std::nullopt);
        }
    }
    if (held) {
        std::size_t const count = keys.size();
        notice("Deleted " + std::to_string(count) + (count == 1 ? " row" : " rows"),
               {StatusEntry{.id = "undo", .label = "Undo"}});
        _undoOffered = true;
        if (_options.scheduler != nullptr) {
            _undoTimer = _options.scheduler->after(_options.undoHold, [this] { clearUndo(); });
        }
    }
}

void GridController::focusFirstEditable(Key const& key) {
    for (std::string const& column : _source->columns()) {
        if (editable(CellRef{.key = key, .column = column})) {
            focusCell(CellRef{.key = key, .column = column});
            return;
        }
    }
}

void GridController::setSelection(std::optional<CellRange> range) {
    if (_selection.peek() == range) {
        return;
    }
    _selection.set(std::move(range));
    if (_selectionObserver) {
        std::function<void(std::optional<CellRange> const&)> const observer = _selectionObserver;
        observer(_selection.peek());
    }
}

void GridController::notice(std::string text, std::vector<StatusEntry> entries) {
    if (entries.empty() && _undoOffered) {
        entries.push_back(StatusEntry{.id = "undo", .label = "Undo"});
    }
    _message.set(std::move(text));
    _messageEntries.set(std::move(entries));
    _notices.set(_notices.peek() + 1);
}

void GridController::clearNotice() {
    if (_messageEntries.peek().empty()) {
        _message.set({});
    }
}

void GridController::clearUndo() {
    _undoOffered = false;
    _undoTimer.cancel();
    if (!_messageEntries.peek().empty()) {
        _messageEntries.set({});
        _message.set({});
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`from`, `target`): the old key first, which the one call site (the source's move callback) spells out
void GridController::keyMoved(Key const& from, Key const& target) {
    if (std::optional<CellRef> const open = _editing.peek(); open && open->key == from) {
        _editing.set(CellRef{.key = target, .column = open->column});
    }
    std::optional<GridFocus> const shown = _place.peek();
    if (!shown || shown->key != from) {
        return;
    }
    _focusedRow = target;
    _place.set(GridFocus{
        .key = target, .viewRow = _source->positionOf(target).value_or(shown->viewRow), .column = shown->column});
}

void GridController::setPlace(std::optional<GridFocus> place) {
    std::optional<Key> const row = place ? place->key : std::nullopt;
    _place.set(std::move(place));
    if (row != _focusedRow) {
        _focusedRow = row;
        _source->focusRow(row);
    }
}

void GridController::letGo() {
    if (!_editing.peek()) {
        _source->holdRow(std::nullopt);
    }
}

bool GridController::editable(CellRef const& cell) const {
    return _rt->untracked(
        [&] { return !_source->readOnly(cell.key, cell.column) && !_source->covered(cell.key, cell.column); });
}

void GridController::commit() {
    std::optional<CellRef> const open = _editing.peek();
    if (!open) {
        return;
    }
    bool const changed = _source->draft(open->key, open->column) != _kept || _source->resends(open->key, open->column);
    _editing.set(std::nullopt);
    if (changed) {
        noteChange();
        _source->committed();
    }
}

void GridController::toggle(CellRef const& cell) {
    if (!editable(cell)) {
        return;
    }
    std::string const now = _source->draft(cell.key, cell.column);
    _source->write(cell.key, cell.column,
                   forms::detail::engine::trim(now) == "true" ? std::string{"false"} : std::string{"true"});
    noteChange();
    _source->committed();
}

GridController::Step GridController::next(std::optional<CellRef> const& from, bool forward) const {
    std::vector<std::string> const columns = _source->columns();
    std::size_t const rows = _source->rowCount();
    if (columns.empty() || rows == 0) {
        return {};
    }
    std::size_t const width = columns.size();
    std::size_t const total = rows * width;
    bool const byRows = _options.walk == Walk::Rows;
    auto const linear = [&](std::size_t row, std::size_t column) {
        return byRows ? (row * width) + column : (column * rows) + row;
    };
    std::int64_t position = forward ? -1 : static_cast<std::int64_t>(total);
    if (from) {
        std::optional<std::size_t> const row = _source->positionOf(from->key);
        auto const column = std::ranges::find(columns, from->column);
        if (row && column != columns.end()) {
            position = static_cast<std::int64_t>(linear(*row, static_cast<std::size_t>(column - columns.begin())));
        }
    }
    for (position += forward ? 1 : -1; position >= 0 && std::cmp_less(position, total); position += forward ? 1 : -1) {
        auto const index = static_cast<std::size_t>(position);
        std::size_t const row = byRows ? index / width : index % rows;
        std::size_t const column = byRows ? index % width : index / rows;
        std::optional<Key> const key = _source->keyAt(row);
        if (!key) {
            return Step{.kind = Step::Kind::Stop, .row = row, .column = columns.at(column), .key = std::nullopt};
        }
        if (editable(CellRef{.key = *key, .column = columns.at(column)})) {
            return Step{.kind = Step::Kind::Found, .row = row, .column = columns.at(column), .key = key};
        }
    }
    return {};
}

void GridController::land(Step const& step) {
    if (step.kind == Step::Kind::Found) {
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): a `Found` step is built only with its key set (see `Step`), so the optional holds a value here
        focusCell(CellRef{.key = *step.key, .column = step.column});
    } else if (step.kind == Step::Kind::Stop) {
        setPlace(GridFocus{.key = std::nullopt, .viewRow = step.row, .column = step.column});
        _message.set({});
    }
}

KeyOutcome GridController::walkFrom(bool forward) {
    std::optional<GridFocus> const shown = _rt->untracked([&] { return place(); });
    if (shown && !shown->key) {
        return KeyOutcome::Handled;
    }
    std::optional<CellRef> const from =
        shown ? std::optional<CellRef>{CellRef{.key = *shown->key, .column = shown->column}} : std::nullopt;
    Step const step = next(from, forward);
    if (step.kind == Step::Kind::None && forward && from && appendOnWalk()) {
        return KeyOutcome::Handled;
    }
    if (step.kind == Step::Kind::None) {
        return forward ? KeyOutcome::FocusNext : KeyOutcome::FocusPrevious;
    }
    land(step);
    return KeyOutcome::Handled;
}

bool GridController::appendOnWalk() {
    if (!_options.appendRows || fixedRows() || !_rt->untracked([&] { return insertRefusal(); }).empty()) {
        return false;
    }
    std::optional<Key> const key = _source->appendRow();
    if (!key) {
        return false;
    }
    noteChange();
    _source->committed();
    focusFirstEditable(*key);
    return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the three Enter modes each walk a different axis and share the focus and the editable-cell test
void GridController::enterMove(CellRef const& from) {
    if (_options.enterMoves == EnterMoves::None) {
        return;
    }
    std::optional<std::size_t> const row = _source->positionOf(from.key);
    if (!row) {
        return;
    }
    if (_options.enterMoves == EnterMoves::Down) {
        std::size_t const rows = _source->rowCount();
        for (std::size_t below = *row + 1; below < rows; ++below) {
            std::optional<Key> const key = _source->keyAt(below);
            if (!key) {
                land(Step{.kind = Step::Kind::Stop, .row = below, .column = from.column, .key = std::nullopt});
                return;
            }
            if (editable(CellRef{.key = *key, .column = from.column})) {
                focusCell(CellRef{.key = *key, .column = from.column});
                return;
            }
        }
        // Enter down from the last row adds a row with `appendRows`, focused in the same column when it is
        // editable.
        if (*row + 1 >= rows && _options.appendRows && !fixedRows() &&
            _rt->untracked([&] { return insertRefusal(); }).empty()) {
            if (std::optional<Key> const key = _source->appendRow()) {
                noteChange();
                _source->committed();
                if (editable(CellRef{.key = *key, .column = from.column})) {
                    focusCell(CellRef{.key = *key, .column = from.column});
                } else {
                    focusFirstEditable(*key);
                }
            }
        }
        return;
    }
    std::vector<std::string> const columns = _source->columns();
    auto const found = std::ranges::find(columns, from.column);
    if (found == columns.end()) {
        return;
    }
    for (auto column = std::next(found); column != columns.end(); ++column) {
        if (editable(CellRef{.key = from.key, .column = *column})) {
            focusCell(CellRef{.key = from.key, .column = *column});
            return;
        }
    }
}

FormGrid makeFormGrid(forms::FormSession& form, forms::FieldState const& array, std::vector<GridColumn> columns,
                      GridOptions options, FormatContext format, TableOptions table, std::vector<TableColumn> shown) {
    if (shown.empty()) {
        for (GridColumn const& column : columns) {
            shown.push_back(TableColumn{.id = column.id, .label = column.label});
        }
    }
    if (shown.size() != columns.size()) {
        throw std::invalid_argument{"morph::ui::makeFormGrid: one table column per grid column"};
    }
    auto source = std::make_shared<FormGridSource>(form, array.path(), std::move(columns), std::move(format));
    auto grid = std::make_shared<GridController>(form.runtime(), *source, std::move(options));
    grid->keepAlive(source);
    source->onBeforeSubmit([weak = std::weak_ptr<GridController>{grid}] {
        if (auto const live = weak.lock()) {
            live->leave();
        }
    });
    Node node = grid->node(std::move(shown), std::move(table));
    return FormGrid{.source = std::move(source), .grid = std::move(grid), .node = std::move(node)};
}

}  // namespace morph::ui
