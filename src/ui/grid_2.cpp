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

FixedGridSource::BlockRows FixedGridSource::blockRows() const {
    BlockRows out;
    for (Part const& part : _parts) {
        if (FixedBlock const* const block = std::get_if<FixedBlock>(&part)) {
            std::vector<std::pair<std::uint64_t, forms::RowState const*>> rows;
            for (auto const& row : _form->field(block->forEach).rows()) {
                rows.emplace_back(row->key(), row.get());
            }
            out.push_back(std::move(rows));
        }
    }
    return out;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): rebuilds the rows from the parts and the collections in one pass, each part kind appending in the order the document lists them
void FixedGridSource::expand() const {
    std::vector<Expanded> next;
    _blockRows = _rt->untracked([&] { return blockRows(); });
    std::size_t block = 0;
    for (Part const& part : _parts) {
        if (FixedRow const* const row = std::get_if<FixedRow>(&part)) {
            Expanded made{.key = Key{row->key}, .row = row, .specimen = nullptr, .fields = {}};
            for (auto const& [column, cell] : row->cells) {
                if (!cell.field.empty()) {
                    made.fields.insert_or_assign(column, &_form->field(cell.field));
                }
            }
            next.push_back(std::move(made));
            continue;
        }
        auto const& repeated = std::get<FixedBlock>(part);
        for (auto const& [rowKey, specimen] : _blockRows.at(block)) {
            for (FixedRow const& row : repeated.rows) {
                Expanded made{.key = Key{std::to_string(rowKey) + "/" + row.key},
                              .row = &row,
                              .specimen = specimen,
                              .fields = {}};
                for (auto const& [column, cell] : row.cells) {
                    if (cell.field.empty()) {
                        continue;
                    }
                    std::string const path = repeated.forEach + "#" + std::to_string(rowKey) + "." + cell.field;
                    try {
                        made.fields.insert_or_assign(column, &_form->field(path));
                    } catch (std::out_of_range const&) {
                        made.fields.insert_or_assign(column, nullptr);
                    }
                }
                next.push_back(std::move(made));
            }
        }
        ++block;
    }
    _expanded = std::move(next);
    _index.clear();
    std::vector<table::RowId> ids;
    std::vector<std::vector<table::Cell>> cells;
    for (std::size_t at = 0; at < _expanded.size(); ++at) {
        Key const& key = _expanded.at(at).key;
        _index.emplace(key, at);
        ids.push_back(key);
        cells.push_back({table::Cell{std::get<std::string>(key)}});
    }
    ++_expansion;
    _data->setRows(std::move(ids), std::move(cells));
}

void FixedGridSource::sync() const {
    if (_blockRows.empty() || blockRows() == _blockRows) {
        return;
    }
    _rt->untracked([&] { expand(); });
    _rows->engine().flush();
}

void FixedGridSource::watch() {
    static_cast<void>(blockRows());
    if (_watching && !_posted) {
        _posted = true;
        _rt->owner().post(_alive.guard([this] {
            _posted = false;
            sync();
        }));
    }
    _watching = true;
}

FixedGridSource::Expanded const* FixedGridSource::expandedOf(Key const& key) const {
    sync();
    auto const found = _index.find(key);
    return found == _index.end() ? nullptr : &_expanded.at(found->second);
}

std::pair<FixedCell const*, forms::RowState const*> FixedGridSource::cellOf(Key const& key,
                                                                            std::string_view column) const {
    Expanded const* const row = expandedOf(key);
    if (row == nullptr) {
        return {nullptr, nullptr};
    }
    auto const found = row->row->cells.find(column);
    return {found == row->row->cells.end() ? nullptr : &found->second, row->specimen};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per overlay.
void FixedGridSource::applyOverlays() {
    sync();
    if (std::uint64_t const generation = _form->loadGeneration(); generation != _unitGeneration) {
        // A field the user changed keeps the unit they left it in.
        _rt->untracked(
            [&] { std::erase_if(_unitApplied, [this](auto const& entry) { return !_form->modified(entry.first); }); });
        _unitGeneration = generation;
    }
    std::vector<std::pair<forms::FieldState*, std::size_t>> asLoaded;
    for (Expanded const& row : _expanded) {
        for (auto const& [column, state] : row.fields) {
            auto const found = row.row->cells.find(column);
            if (state == nullptr || found == row.row->cells.end()) {
                continue;
            }
            FixedCell const& cell = found->second;
            std::optional<std::uint32_t> const places = cell.decimals ? cell.decimals(row.specimen) : std::nullopt;
            std::optional<std::string> const unit = cell.unit ? cell.unit(row.specimen) : std::nullopt;
            std::optional<std::vector<std::string>> options = cell.options ? cell.options(row.specimen) : std::nullopt;
            std::string placeholder = cell.placeholder ? cell.placeholder(row.specimen) : std::string{};
            _rt->untracked([&] {
                if (cell.decimals && state->places().peek() != places) {
                    state->places().set(places);
                }
                if (cell.unit) {
                    detail::applyUnitOverlay(_unitApplied, *state, unit, asLoaded);
                }
                if (cell.options) {
                    state->narrowOptions(std::move(options));
                }
                if (cell.placeholder) {
                    state->overridePlaceholder(std::move(placeholder));
                }
            });
        }
    }
    if (!asLoaded.empty()) {
        _rt->untracked([&] { _form->switchUnitsAsLoaded(asLoaded); });
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): recomputes the coverage once from the rows and the layout; the span walk and the cache check share the running coverage
FixedGridSource::Coverage const& FixedGridSource::coverageNow() const {
    sync();
    if (_coverage && _coverage->expansion == _expansion && _coverage->layout == _layout) {
        return *_coverage;
    }
    Coverage next{.expansion = _expansion, .layout = _layout, .covered = {}, .given = {}};
    std::vector<std::string> declared;
    declared.reserve(_columns.size());
    for (GridColumn const& column : _columns) {
        declared.push_back(column.id);
    }
    for (std::size_t at = 0; at < _expanded.size(); ++at) {
        Expanded const& row = _expanded.at(at);
        for (auto const& [column, cell] : row.row->cells) {
            if (cell.rows <= 1 && cell.columns <= 1) {
                continue;
            }
            CellSpan span{.column = column, .rows = cell.rows, .columns = cell.columns};
            span.columns = cutColumns(span, declared, _layout);
            auto const place = std::ranges::find(_layout, span.column);
            // A span past the last row is refused by the mount and drawn as nothing, so it covers nothing here
            // either.
            if (span.columns == 0 || span.rows == 0 || place == _layout.end() || span.rows > _expanded.size() - at) {
                continue;
            }
            auto const first = static_cast<std::size_t>(place - _layout.begin());
            for (std::size_t down = 0; down < span.rows; ++down) {
                for (std::size_t right = 0; right < span.columns; ++right) {
                    if (down != 0 || right != 0) {
                        next.covered[_expanded.at(at + down).key].push_back(_layout.at(first + right));
                    }
                }
            }
            next.given[row.key].push_back(std::move(span));
        }
    }
    _coverage = std::move(next);
    return *_coverage;
}

ModelGridSource::ModelGridSource(std::shared_ptr<TableRows> rows, table::CellEdits& edits,
                                 std::vector<ModelColumn> columns, EditMode mode, ModelGridContext context)
    : _rows{std::move(rows)},
      _edits{&edits},
      _columns{std::move(columns)},
      _mode{mode},
      _context{std::move(context)},
      _rt{_context.runtime},
      _version{*_context.runtime, std::uint64_t{0}},
      _open{*_context.runtime, std::optional<CellRef>{}} {
    for (ModelColumn const& column : _columns) {
        _layout.push_back(column.id);
        if (column.edit && column.model != nullptr && !column.path.empty()) {
            // The submitter is never called: an edit is sent as a row commit, by the table's own `CellEdits`.
            exec::IExecutor* const owner = &_rt->owner();
            auto session = std::make_unique<forms::FormSession>(
                *_rt, *column.model,
                [owner](std::string_view, std::string const&) {
                    return async::Completion<std::string>::makeSettleable(owner).first;
                },
                _context.choices, _context.session);
            _sessions.insert_or_assign(column.id, Session{.form = std::move(session), .loadedFor = {}, .base = {}});
        }
    }
    _view = std::make_unique<Rows>(*this);
    _edits->onRowSettled(_alive.guard([this](table::RowId const& row, bool refused) { rowSettled(row, refused); }));
}

void ModelGridSource::setDescription(table::TableDescription const& description) {
    _description = description;
    auto units = table::registryOf(description);
    _units = units ? *std::move(units) : nullptr;
    std::vector<std::string> ids;
    ids.reserve(description.columns.size());
    for (table::TableColumn const& column : description.columns) {
        ids.push_back(column.info.id);
    }
    _edits->keepColumns(ids);
    for (auto& [row, cells] : _held) {
        std::erase_if(cells, [&ids](table::EditCell const& cell) { return !std::ranges::contains(ids, cell.column); });
    }
    std::erase_if(_resend, [&ids](CellRef const& cell) { return !std::ranges::contains(ids, cell.column); });
    _positionsVersion.reset();
    bump();
}

void ModelGridSource::setLayout(std::vector<std::string> visible) {
    if (visible != _layout) {
        _layout = std::move(visible);
        bump();
    }
}

void ModelGridSource::setFormat(FormatContext format) {
    _context.format = std::move(format);
    bump();
}

void ModelGridSource::rowsAbsent(std::span<table::RowId const> keys) {
    for (table::RowId const& key : keys) {
        _edits->rowAbsent(key);
        _held.erase(key);
        std::erase_if(_resend, [&key](CellRef const& cell) { return cell.key == key; });
    }
    bump();
}

void ModelGridSource::moveKey(Key const& from, Key const& destination) {
    if (from == destination) {
        return;
    }
    _rows->moveKey(from, destination);
    _edits->moveRow(from, destination);
    if (auto const held = _held.extract(from); !held.empty()) {
        std::vector<table::EditCell>& target = _held[destination];
        for (table::EditCell& cell : held.mapped()) {
            if (!std::ranges::contains(target, cell.column, &table::EditCell::column)) {
                target.push_back(std::move(cell));
            }
        }
    }
    for (CellRef& cell : _resend) {
        if (cell.key == from) {
            cell.key = destination;
        }
    }
    _positionsVersion.reset();
    bump();
    if (_focus.moved) {
        std::function<void(Key const&, Key const&)> const moved = _focus.moved;
        moved(from, destination);
    }
}

std::optional<std::string> ModelGridSource::heldDraft(Key const& key, std::string_view column) const {
    static_cast<void>(_version.get());
    if (std::optional<CellRef> const open = _open.get(); open && open->key == key && open->column == column) {
        if (Session const* const session = sessionOf(column); session != nullptr && session->loadedFor == key) {
            return fieldOf(column)->text().get();
        }
    }
    return _edits->draft(key, column);
}

std::optional<std::string> ModelGridSource::heldText(Key const& key, std::string_view column) const {
    std::optional<std::string> held = heldDraft(key, column);
    table::TableColumn const* const shown = describedColumn(column);
    forms::FieldState const* const field = fieldOf(column);
    if (held && !held->empty() && shown != nullptr && shown->info.kind == table::ColumnKind::Measure &&
        field != nullptr && !field->spec().unit.empty()) {
        *held += " " + field->spec().unit;
    }
    return held;
}

std::size_t ModelGridSource::rowCount() const {
    static_cast<void>(_version.get());
    return static_cast<std::size_t>(std::max<std::int64_t>(_rows->total().rows, 0));
}

std::optional<Key> ModelGridSource::keyAt(std::size_t viewRow) const {
    static_cast<void>(_version.get());
    std::optional<TableRowRef> const row = _rows->row(static_cast<std::int64_t>(viewRow));
    return row ? std::optional<Key>{row->key} : std::nullopt;
}

std::optional<std::size_t> ModelGridSource::positionOf(Key const& key) const {
    if (_positionsVersion != _rowsVersion) {
        _positions.clear();
        for (auto const& [first, count] : _rows->heldRanges()) {
            for (std::int64_t at = first; at < first + count; ++at) {
                if (std::optional<TableRowRef> const row = _rows->row(at)) {
                    _positions.insert_or_assign(row->key, static_cast<std::size_t>(at));
                }
            }
        }
        _positionsVersion = _rowsVersion;
    }
    auto const found = _positions.find(key);
    return found == _positions.end() ? std::nullopt : std::optional{found->second};
}

Node ModelGridSource::editor(Key const& key, std::string_view column) {
    Session* const session = loaded(key, column);
    if (session == nullptr) {
        return ui::text(Text{.text = std::string{}, .role = TextRole::Muted});
    }
    return forms::detail::engine::scalarControl(*session->form, *fieldOf(column));
}

bool ModelGridSource::toggles(Key const& key, std::string_view column) const {
    static_cast<void>(key);
    forms::FieldState const* const field = fieldOf(column);
    return field != nullptr && field->spec().kind == forms::FieldKind::Boolean;
}

bool ModelGridSource::covered(Key const& key, std::string_view column) const {
    static_cast<void>(key);
    static_cast<void>(column);
    return false;
}

std::string ModelGridSource::draft(Key const& key, std::string_view column) const {
    return _rt->untracked([&] {
        if (std::optional<std::string> held = heldDraft(key, column)) {
            return std::move(*held);
        }
        // The column's one session holds an open editor's cell: another cell of the column is read as it shows.
        if (std::optional<CellRef> const open = _open.peek(); open && open->column == column && open->key != key) {
            return text(key, column);
        }
        // With no draft held the cell is the row's value now: a session still holding an earlier edit of the
        // cell, whose draft a fetch since ended, is loaded again.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the method is const for readers but refreshes the lazily loaded sessions, which are cache state this object owns
        auto* const self = const_cast<ModelGridSource*>(this);
        if (auto const found = self->_sessions.find(column); found != self->_sessions.end()) {
            found->second.loadedFor.reset();
        }
        Session const* const session = self->loaded(key, column);
        return session == nullptr ? std::string{} : fieldOf(column)->draft();
    });
}

std::string ModelGridSource::text(Key const& key, std::string_view column) const {
    if (std::optional<std::string> held = heldText(key, column)) {
        return std::move(*held);
    }
    table::TableColumn const* const described = describedColumn(column);
    std::optional<Located> const row = locate(key);
    if (described == nullptr || !row) {
        return {};
    }
    return formatCell(cellOf(row->record.field(column)), *described, _context.format);
}

std::optional<std::string> ModelGridSource::error(Key const& key, std::string_view column) const {
    static_cast<void>(_version.get());
    return _edits->error(key, column);
}

void ModelGridSource::write(Key const& key, std::string_view column, std::string draft) {
    Session const* const session = loaded(key, column);
    if (session == nullptr) {
        return;
    }
    _open.set(CellRef{.key = key, .column = std::string{column}});
    fieldOf(column)->text().set(std::move(draft));
    CellRef const cell{.key = key, .column = std::string{column}};
    if (!std::ranges::contains(_dirty, cell)) {
        _dirty.push_back(cell);
    }
    hold(key, column);
}

void ModelGridSource::committed() {
    std::vector<CellRef> cells = std::exchange(_dirty, {});
    // An editor's input writes its field without `write`: the open cell is committed too.
    if (std::optional<CellRef> const open = _open.peek(); open && !std::ranges::contains(cells, *open)) {
        cells.push_back(*open);
    }
    for (CellRef const& cell : cells) {
        std::optional<table::EditCell> const made = encode(cell.key, cell.column);
        if (!made) {
            continue;
        }
        if (_mode == EditMode::Cell) {
            _edits->commitRow(table::RowEditRequest{.row = cell.key, .cells = {*made}});
            continue;
        }
        std::vector<table::EditCell>& held = _held[cell.key];
        std::erase_if(held, [&](table::EditCell const& other) { return other.column == made->column; });
        held.push_back(*made);
    }
    _open.set(std::nullopt);
    bump();
}

void ModelGridSource::editorOpened(Key const& key, std::string_view column) {
    if (loaded(key, column) == nullptr) {
        return;
    }
    _open.set(CellRef{.key = key, .column = std::string{column}});
    fieldOf(column)->openOptions();
}

CellMarks ModelGridSource::marks(Key const& key, std::string_view column) const {
    static_cast<void>(_version.get());
    return CellMarks{.readOnly = readOnly(key, column),
                     .modified = _edits->modified(key, column),
                     .error = _edits->error(key, column).has_value(),
                     .stale = _edits->stale(key, column)};
}

std::optional<Key> ModelGridSource::keyOf(Value const& record) const {
    if (!_description) {
        return std::nullopt;
    }
    Value const* const key = record.field(_description->key);
    if (key == nullptr) {
        return std::nullopt;
    }
    if (std::int64_t const* const number = key->asInt()) {
        return Key{*number};
    }
    if (std::string const* const text = key->asString()) {
        return Key{*text};
    }
    return std::nullopt;
}

bool ModelGridSource::rowModified(Key const& key) const {
    static_cast<void>(_version.get());
    return _edits->rowEdited(key);
}

std::optional<std::string> ModelGridSource::refusal(Key const& key, std::string_view column) const {
    CellFacts const facts = factsOf(key, column);
    if (facts.unloaded) {
        return std::string{kUnloadedText};
    }
    if (facts.foreignUnit) {
        return std::string{kForeignUnitText};
    }
    if (facts.busy) {
        return std::string{kBusyText};
    }
    if (facts.locked) {
        std::optional<Located> const row = locate(key);
        if (row && _description) {
            return lockText(TableRowRef{.key = key, .row = row->record}, *_description);
        }
    }
    return std::nullopt;
}

void ModelGridSource::focusRow(std::optional<Key> const& key) {
    if (_mode != EditMode::Row) {
        return;
    }
    std::vector<Key> leaving;
    for (auto const& [row, cells] : _held) {
        if (!key || row != *key) {
            leaving.push_back(row);
        }
    }
    for (Key const& row : leaving) {
        send(row);
    }
}

void ModelGridSource::commitHeld() {
    std::vector<Key> rows;
    rows.reserve(_held.size());
    for (auto const& [row, cells] : _held) {
        rows.push_back(row);
    }
    for (Key const& row : rows) {
        send(row);
    }
}

bool ModelGridSource::resends(Key const& key, std::string_view column) const {
    return std::ranges::any_of(_resend, [&](CellRef const& cell) { return cell.key == key && cell.column == column; });
}

void ModelGridSource::Rows::changed(table::ViewChange const& change) {
    for (std::size_t const index : broughtRows(change)) {
        if (std::optional<TableRowRef> const row = _inner->row(static_cast<std::int64_t>(index))) {
            _source->_edits->rowFetched(row->key);
        }
    }
    ++_source->_rowsVersion;
    _source->bump();
    // Last: the mount's handler may end the mount that owns this source.
    if (_handler) {
        std::function<void(table::ViewChange const&)> const handler = _handler;
        handler(change);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): walks the change's operations once, shifting the collected indices as each insert or removal passes; the shift depends on the operation kind
std::vector<std::size_t> ModelGridSource::broughtRows(table::ViewChange const& change) {
    std::vector<std::size_t> out;
    for (table::ViewOp const& op : change.ops) {
        switch (op.kind) {
            case table::ViewOp::Kind::Reset:
                out.clear();
                break;
            case table::ViewOp::Kind::Removed:
                std::erase_if(out,
                              [&op](std::size_t index) { return index >= op.first && index < op.first + op.count; });
                for (std::size_t& index : out) {
                    index -= index >= op.first + op.count ? op.count : 0;
                }
                break;
            case table::ViewOp::Kind::Inserted:
                for (std::size_t& index : out) {
                    index += index >= op.first ? op.count : 0;
                }
                for (std::size_t at = op.first; at < op.first + op.count; ++at) {
                    out.push_back(at);
                }
                break;
            case table::ViewOp::Kind::Changed:
                for (std::size_t at = op.first; at < op.first + op.count; ++at) {
                    if (!std::ranges::contains(out, at)) {
                        out.push_back(at);
                    }
                }
                break;
            case table::ViewOp::Kind::Moved:
                for (std::size_t& index : out) {
                    if (index == op.first) {
                        index = op.to;
                        continue;
                    }
                    std::size_t const removed = index > op.first ? index - 1 : index;
                    index = removed >= op.to ? removed + 1 : removed;
                }
                break;
            default:
                break;
        }
    }
    return out;
}

ModelColumn const* ModelGridSource::columnOf(std::string_view columnId) const {
    auto const found = std::ranges::find(_columns, columnId, &ModelColumn::id);
    return found == _columns.end() ? nullptr : &*found;
}

table::TableColumn const* ModelGridSource::describedColumn(std::string_view columnId) const {
    if (!_description) {
        return nullptr;
    }
    auto const found = std::ranges::find_if(
        _description->columns, [columnId](table::TableColumn const& column) { return column.info.id == columnId; });
    return found == _description->columns.end() ? nullptr : &*found;
}

ModelGridSource::Session const* ModelGridSource::sessionOf(std::string_view column) const {
    auto const found = _sessions.find(column);
    return found == _sessions.end() ? nullptr : &found->second;
}

forms::FieldState* ModelGridSource::fieldOf(std::string_view column) const {
    Session const* const session = sessionOf(column);
    ModelColumn const* const described = columnOf(column);
    return session == nullptr || described == nullptr ? nullptr : &session->form->field(described->path);
}

std::optional<ModelGridSource::Located> ModelGridSource::locate(Key const& key) const {
    static_cast<void>(_version.get());
    std::optional<std::size_t> const index = positionOf(key);
    if (!index) {
        return std::nullopt;
    }
    std::optional<TableRowRef> const row = _rows->row(static_cast<std::int64_t>(*index));
    if (!row || row->key != key) {
        return std::nullopt;
    }
    return Located{.index = static_cast<std::int64_t>(*index), .record = row->row};
}

CellFacts ModelGridSource::factsOf(Key const& key, std::string_view column) const {
    static_cast<void>(_version.get());
    ModelColumn const* const described = columnOf(column);
    CellFacts facts;
    facts.field = fieldOf(column);
    facts.hasEdit =
        described != nullptr && described->edit && (!_context.may || _context.may(described->edit->action));
    std::optional<Located> const row = locate(key);
    if (!row) {
        facts.unloaded = true;
        return facts;
    }
    if (described != nullptr && described->edit && !described->edit->readonly.empty()) {
        Value const* const companion = row->record.field(described->edit->readonly);
        bool const* const frozen = companion == nullptr ? nullptr : companion->asBool();
        facts.companionReadOnly = frozen != nullptr && *frozen;
    }
    facts.busy = _rows->states().isBusy(key);
    facts.locked = _description && lockText(TableRowRef{.key = key, .row = row->record}, *_description).has_value();
    table::TableColumn const* const shown = describedColumn(column);
    table::UnloadedLevels const levels = _rows->unloadedLevels(row->index);
    Value const* const value = row->record.field(column);
    facts.unloaded = value == nullptr || (shown != nullptr && !shown->info.level.empty() &&
                                          std::ranges::contains(levels.levels, shown->info.level));
    if (Quantity const* const measure = value == nullptr ? nullptr : value->asQuantity();
        measure != nullptr && facts.field != nullptr && facts.field->spec().kind == forms::FieldKind::Quantity) {
        facts.foreignUnit = !inFieldUnit(*measure, facts.field->spec());
    }
    return facts;
}

ModelGridSource::Session* ModelGridSource::loaded(Key const& key, std::string_view column) {
    auto const found = _sessions.find(column);
    ModelColumn const* const described = columnOf(column);
    if (found == _sessions.end() || described == nullptr) {
        return nullptr;
    }
    Session& session = found->second;
    if (session.loadedFor == key) {
        return &session;
    }
    _rt->untracked([&] {
        std::optional<Located> const row = locate(key);
        Value const* const value = row ? row->record.field(column) : nullptr;
        std::string const json = value == nullptr ? std::string{"null"} : wireOf(*value, fieldOf(column)->spec());
        std::string const body = described->edit->member.empty()
                                     ? R"({"value":)" + json + "}"
                                     : R"({"values":{")" + described->edit->member + R"(":)" + json + "}}";
        _rt->batch([&] {
            session.form->prefill(body);
            forms::FieldState& field = *fieldOf(column);
            session.base = field.draft();
            if (std::optional<std::string> held = _edits->draft(key, column)) {
                field.text().set(std::move(*held));
            }
        });
    });
    session.loadedFor = key;
    return &session;
}

void ModelGridSource::hold(Key const& key, std::string_view column) {
    Session const* const session = sessionOf(column);
    forms::FieldState const* const field = fieldOf(column);
    if (session == nullptr || field == nullptr) {
        return;
    }
    _rt->untracked([&] {
        std::string const text = field->draft();
        if (text == session->base && !_edits->stale(key, column) && !_edits->modified(key, column)) {
            _edits->clearDraft(key, column);
            return;
        }
        std::optional<forms::FieldError> const& failed = field->error().get();
        _edits->setDraft(key, column, text, failed ? std::optional{failed->message} : std::nullopt);
    });
    bump();
}

std::optional<table::EditCell> ModelGridSource::encode(Key const& key, std::string const& column) {
    if (loaded(key, column) == nullptr) {
        return std::nullopt;
    }
    std::erase_if(_resend, [&](CellRef const& cell) { return cell.key == key && cell.column == column; });
    hold(key, column);
    auto const draft = _edits->draft(key, column);
    if (!draft || _edits->error(key, column)) {
        return std::nullopt;
    }
    if (_rt->untracked([&] { return readOnly(key, column); })) {
        // Refused at the cell, as a held cell is: the draft keeps why, and its next commit sends it.
        std::optional<std::string> const why = _rt->untracked([&] { return refusal(key, column); });
        _edits->setDraft(key, column, *draft, why.value_or(std::string{kReadOnlyText}));
        _resend.push_back(CellRef{.key = key, .column = column});
        return std::nullopt;
    }
    forms::Encoding const encoding = _rt->untracked([&] { return fieldOf(column)->encoded().get(); });
    ModelColumn const* const described = columnOf(column);
    std::string member = described != nullptr && described->edit && !described->edit->member.empty()
                             ? described->edit->member
                             : std::string{"value"};
    return table::EditCell{.column = column,
                           .json = encoding && encoding->has_value() ? (*encoding)->json : std::string{"null"},
                           .member = std::move(member)};
}

void ModelGridSource::send(Key const& row) {
    auto const found = _held.find(row);
    if (found == _held.end()) {
        return;
    }
    std::vector<table::EditCell> cells = std::move(found->second);
    _held.erase(found);
    std::vector<std::string> refused;
    std::erase_if(cells, [this, &row, &refused](table::EditCell const& cell) {
        bool const readOnlyNow = _rt->untracked([&] { return readOnly(row, cell.column); });
        if (readOnlyNow) {
            refused.push_back(cell.column);
        }
        return readOnlyNow;
    });
    // Refused at the cell: the draft stays, with why as its error, until a commit of the cell sends it.
    for (std::string const& column : refused) {
        std::optional<std::string> const why = _rt->untracked([&] { return refusal(row, column); });
        _edits->setDraft(row, column, _edits->draft(row, column).value_or(std::string{}),
                         why.value_or(std::string{kReadOnlyText}));
        CellRef cell{.key = row, .column = column};
        if (!std::ranges::contains(_resend, cell)) {
            _resend.push_back(std::move(cell));
        }
    }
    if (!cells.empty()) {
        _edits->commitRow(table::RowEditRequest{.row = row, .cells = std::move(cells)});
    }
    bump();
}

void ModelGridSource::rowSettled(table::RowId const& row, bool refused) {
    bump();
    if (_mode != EditMode::Row || !refused || !_refocus) {
        return;
    }
    for (std::string const& column : _layout) {
        if (_edits->error(row, column)) {
            std::function<void(CellRef const&)> const refocus = _refocus;
            refocus(CellRef{.key = row, .column = column});
            return;
        }
    }
}

std::string ModelGridSource::wireOf(Value const& value, forms::FieldSpec const& spec) const {
    forms::FieldKind const kind = spec.kind;
    if (Quantity const* const measure = value.asQuantity(); measure != nullptr && kind == forms::FieldKind::Quantity) {
        std::optional<math::Rational> const amount = inFieldUnit(*measure, spec);
        return amount ? glz::write_json(valueToJson(Value{Decimal{*amount}})).value_or("null") : std::string{"null"};
    }
    if (kind == forms::FieldKind::EpochDate) {
        std::string const* const text = value.asString();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `text->size() == 10` is the previous operand of this condition, so indices 4 and 7 are in range
        if (text != nullptr && text->size() == 10 && (*text)[4] == '-' && (*text)[7] == '-') {
            unsigned year = 0;
            unsigned month = 0;
            unsigned day = 0;
            // Each part is all digits: a part read only in its leading digits ("20xx") is not a date.
            auto const whole = [](char const* first, char const* last, unsigned& out) {
                auto const [end, error] = std::from_chars(first, last, out);
                return error == std::errc{} && end == last;
            };
            char const* const cursor = text->data();
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars takes a pointer range; the ranges are fixed offsets into the 10-character `YYYY-MM-DD` text the size and separator checks above validated
            bool const read = whole(cursor, cursor + 4, year) && whole(cursor + 5, cursor + 7, month) &&
                              whole(cursor + 8, cursor + 10, day);
            // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            std::chrono::year_month_day const date{std::chrono::year{static_cast<int>(year)},
                                                   std::chrono::month{month}, std::chrono::day{day}};
            if (read && date.ok()) {
                return std::to_string(std::chrono::sys_days{date}.time_since_epoch().count());
            }
        }
    }
    if (kind == forms::FieldKind::EpochDateTime) {
        if (time::DateTime const* const instant = value.asTimestamp()) {
            return std::to_string(
                std::chrono::duration_cast<std::chrono::seconds>(instant->value.time_since_epoch()).count());
        }
    }
    return glz::write_json(valueToJson(value)).value_or("null");
}

std::optional<math::Rational> ModelGridSource::inFieldUnit(Quantity const& measure,
                                                           forms::FieldSpec const& spec) const {
    std::optional<table::UnitId> const from = _units ? _units->find(measure.unit) : std::nullopt;
    std::optional<table::UnitId> const toUnit = _units ? _units->find(spec.unitAscii) : std::nullopt;
    if (!from || !toUnit || _units->info(*from).dimension != _units->info(*toUnit).dimension) {
        return std::nullopt;
    }
    auto const factor = math::checkedDiv(_units->info(*from).toBase, _units->info(*toUnit).toBase);
    auto const amount = factor ? math::checkedMul(measure.value, *factor) : std::unexpected(factor.error());
    return amount ? std::optional{*amount} : std::nullopt;
}

table::Cell ModelGridSource::cellOf(Value const* value) {
    if (value == nullptr || value->isNull()) {
        return std::monostate{};
    }
    if (std::int64_t const* const integer = value->asInt()) {
        return *integer;
    }
    if (Decimal const* const decimal = value->asDecimal()) {
        return decimal->value;
    }
    if (bool const* const flag = value->asBool()) {
        return *flag;
    }
    if (Quantity const* const quantity = value->asQuantity()) {
        return quantity->value;
    }
    if (std::string const* const text = value->asString()) {
        return *text;
    }
    return std::monostate{};
}

GridController::GridController(reactive::Runtime& runtime, GridSource& source, GridOptions options)
    : _rt{&runtime},
      _source{&source},
      _options{std::move(options)},
      _place{runtime, std::optional<GridFocus>{}},
      _editing{runtime, std::optional<CellRef>{}},
      _message{runtime, std::string{}},
      _notices{runtime, std::uint64_t{0}},
      _messageEntries{runtime, std::vector<StatusEntry>{}},
      _standing{runtime, std::string{}},
      _standingEntries{runtime, std::vector<StatusEntry>{}},
      _selection{runtime, std::optional<CellRange>{}} {
    _source->refocusWith([this](CellRef const& cell) {
        if (_hasFocus) {
            focusCell(cell);
        }
    });
    _source->followFocusWith(
        FocusLink{.moved = [this](Key const& from, Key const& target) { keyMoved(from, target); }});
}

GridController::~GridController() {
    _source->refocusWith({});
    _source->followFocusWith({});
}

std::optional<CellRef> GridController::focus() const {
    std::optional<GridFocus> const shown = place();
    if (!shown || !shown->key) {
        return std::nullopt;
    }
    return CellRef{.key = *shown->key, .column = shown->column};
}

std::optional<GridFocus> GridController::place() const {
    std::optional<GridFocus> shown = _place.get();
    if (shown && !shown->key) {
        // Tracked: a binding over the place runs again when the row arrives, and names its key.
        shown->key = _source->keyAt(shown->viewRow);
    }
    return shown;
}

void GridController::focusPlace(GridFocus target) {
    _hasFocus = true;
    if (target.key) {
        focusCell(CellRef{.key = *target.key, .column = target.column});
        return;
    }
    commit();
    letGo();
    setPlace(std::move(target));
    setSelection(std::nullopt);
    clearNotice();
}

void GridController::focusCell(CellRef cell) {
    _hasFocus = true;
    if (_source->covered(cell.key, cell.column)) {
        return;
    }
    if (std::optional<CellRef> const open = _editing.peek(); open && *open != cell) {
        commit();
        letGo();
    }
    std::optional<std::size_t> const position = _source->positionOf(cell.key);
    if (!position) {
        return;
    }
    setPlace(GridFocus{.key = cell.key, .viewRow = *position, .column = cell.column});
    if (_selection.peek() && !inBlock(cell)) {
        setSelection(std::nullopt);
    }
    clearNotice();
}

void GridController::activate(CellRef cell) {
    focusCell(cell);
    if (_rt->untracked([&] { return _source->readOnly(cell.key, cell.column); })) {
        if (_options.onActivate) {
            _options.onActivate(cell.key);
        }
        return;
    }
    // A Boolean opens no editor: a click or `Space` flips it, and the second click of a double click, which
    // activates, flips nothing more.
    if (_source->toggles(cell.key, cell.column)) {
        return;
    }
    _kept = _source->draft(cell.key, cell.column);
    _source->holdRow(cell.key);
    _editing.set(cell);
    _source->editorOpened(cell.key, cell.column);
}

std::vector<std::string> GridController::keyChords() const {
    if (_editing.get()) {
        return {"Enter", "Tab", "Shift+Tab", "Escape", "Ctrl+Enter"};
    }
    if (!_source->selectsCells()) {
        return {"Tab",    "Shift+Tab", "Enter",      "F2",           "Space",
                "Delete", "Escape",    "Ctrl+Enter", "Ctrl+Shift+=", "Ctrl+-"};
    }
    return {"Tab",    "Shift+Tab",  "Enter",    "F2",           "Space",      "Delete",
            "Escape", "Ctrl+Enter", "Shift+Up", "Shift+Down",   "Shift+Left", "Shift+Right",
            "Ctrl+A", "Ctrl+V",     "Ctrl+D",   "Ctrl+Shift+=", "Ctrl+-"};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): dispatches one chord across the focus, editing, selection and clipboard states; each branch checks the state the one before left
KeyOutcome GridController::press(std::string_view chord) {
    // The row an editor was open on is let go once the key is handled: the walk moves by view position at the
    // moment of the commit, so the edited row stays where it was while focus moves on.
    struct LetGo {
        GridController* grid;
        explicit LetGo(GridController* held) : grid{held} {}
        LetGo(LetGo const&) = delete;
        LetGo& operator=(LetGo const&) = delete;
        LetGo(LetGo&&) = delete;
        LetGo& operator=(LetGo&&) = delete;
        ~LetGo() { grid->letGo(); }
    } const letting{this};
    _hasFocus = true;
    std::optional<CellRef> const open = _editing.peek();
    if (chord == "Tab" || chord == "Shift+Tab") {
        commit();
        return walkFrom(chord == "Tab");
    }
    if (chord == "Escape") {
        if (open) {
            _source->write(open->key, open->column, _kept);
            _editing.set(std::nullopt);
        }
        return KeyOutcome::Handled;
    }
    if (chord == "Ctrl+Enter") {
        commit();
        _source->commitHeld();
        return KeyOutcome::Handled;
    }
    if (chord == "Enter" && open) {
        commit();
        enterMove(*open);
        return KeyOutcome::Handled;
    }
    std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); });
    if (chord == "Enter" || chord == "F2") {
        if (focused) {
            activate(*focused);
        }
        return KeyOutcome::Handled;
    }
    if (chord == "Space") {
        if (focused && _source->toggles(focused->key, focused->column)) {
            toggle(*focused);
        }
        return KeyOutcome::Handled;
    }
    if (chord == "Delete") {
        clear();
        return KeyOutcome::Handled;
    }
    if (chord == "Shift+Up" || chord == "Shift+Down" || chord == "Shift+Left" || chord == "Shift+Right") {
        int rows = 0;
        int columns = 0;
        if (chord == "Shift+Up") {
            rows = -1;
        } else if (chord == "Shift+Down") {
            rows = 1;
        } else if (chord == "Shift+Left") {
            columns = -1;
        } else {
            columns = 1;
        }
        extend(rows, columns);
        return KeyOutcome::Handled;
    }
    if (chord == "Ctrl+A") {
        selectAll();
        return KeyOutcome::Handled;
    }
    if (chord == "Ctrl+V") {
        if (_readClipboard) {
            if (std::optional<std::string> const text = _readClipboard()) {
                static_cast<void>(paste(*text));
            }
        }
        return KeyOutcome::Handled;
    }
    if (chord == "Ctrl+D") {
        fillDown();
        return KeyOutcome::Handled;
    }
    if (chord == "Ctrl+Shift+=") {
        insertRow(true);
        return KeyOutcome::Handled;
    }
    if (chord == "Ctrl+-") {
        deleteRows();
        return KeyOutcome::Handled;
    }
    return KeyOutcome::Ignored;
}

void GridController::type(std::string_view text) {
    _hasFocus = true;
    if (std::optional<CellRef> const open = _editing.peek()) {
        _source->write(open->key, open->column, std::string{text});
        return;
    }
    std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); });
    if (!focused || _source->covered(focused->key, focused->column)) {
        return;
    }
    if (_rt->untracked([&] { return _source->readOnly(focused->key, focused->column); })) {
        notice(_rt->untracked([&] { return _source->refusal(focused->key, focused->column); })
                   .value_or(std::string{kReadOnlyText}));
        return;
    }
    if (_source->toggles(focused->key, focused->column)) {
        return;
    }
    _kept = _source->draft(focused->key, focused->column);
    _source->holdRow(focused->key);
    _source->write(focused->key, focused->column, std::string{text});
    _editing.set(focused);
    _source->editorOpened(focused->key, focused->column);
}

void GridController::leave() {
    commit();
    letGo();
    _hasFocus = false;
    _focusedRow.reset();
    _source->focusRow(std::nullopt);
}

std::string GridController::message() const {
    if (std::string const& notice = _message.get(); !notice.empty()) {
        return notice;
    }
    std::optional<CellRef> const focused = focus();
    return focused ? _source->error(focused->key, focused->column).value_or(std::string{}) : std::string{};
}

void GridController::click(CellRef const& cell) {
    focusCell(cell);
    if (_rt->untracked([&] { return focus(); }) == cell && _source->toggles(cell.key, cell.column)) {
        toggle(cell);
    }
}

std::string GridController::standing() const {
    if (_source->deriveFailed()) {
        return std::string{kDeriveFailedText};
    }
    return _standing.get();
}

std::vector<StatusEntry> GridController::standingEntries() const {
    if (_source->deriveFailed()) {
        return {StatusEntry{.id = "recalculate", .label = "Recalculate"}};
    }
    return _standingEntries.get();
}

void GridController::chooseEntry(std::string_view entryId) {
    if (entryId == "recalculate") {
        _source->recalculate();
    } else if (entryId == "undo") {
        undo();
    }
}

std::vector<MenuItem> GridController::cellEntries(Key const& row, std::string_view column) {
    GridController* const self = this;
    GridSource* const source = _source;
    std::string const columnId{column};
    MenuItem cell;
    cell.label = std::string{"Revert cell"};
    cell.enabled = Prop<bool>{[source, row, columnId] { return source->marks(row, columnId).modified; }};
    cell.onSelect = [self, source, row, columnId] {
        self->noteChange();
        source->revertCell(row, columnId);
        source->committed();
    };
    MenuItem whole;
    whole.label = std::string{"Revert row"};
    whole.enabled = Prop<bool>{[source, row] { return source->rowModified(row); }};
    whole.onSelect = [self, source, row] {
        self->noteChange();
        source->revertRow(row);
        source->committed();
    };
    std::vector<MenuItem> entries;
    entries.push_back(std::move(cell));
    entries.push_back(std::move(whole));
    std::vector<MenuItem> rows = rowEntries(row);
    if (!rows.empty()) {
        MenuItem separator;
        separator.separator = true;
        entries.push_back(std::move(separator));
        std::ranges::move(rows, std::back_inserter(entries));
    }
    return entries;
}

std::vector<MenuItem> GridController::rowEntries(Key const& row) {
    if (fixedRows()) {
        return {};
    }
    GridController* const self = this;
    std::vector<Key> doomed = _rt->untracked([&] { return selectedRows(); });
    if (doomed.size() < 2 || !std::ranges::contains(doomed, row)) {
        doomed = {row};
    }
    std::size_t const count = doomed.size();
    auto const insertEntry = [self, row](std::string label, bool below) {
        MenuItem item;
        item.label = std::move(label);
        item.enabled = Prop<bool>{[self] { return self->rowOps().insert; }};
        item.description = Prop<std::string>{[self] { return self->insertRefusal(); }};
        item.onSelect = [self, row, below] { self->insertAt(row, below); };
        return item;
    };
    MenuItem removal;
    removal.label = std::string{count > 1 ? "Delete rows" : "Delete row"};
    removal.enabled = Prop<bool>{[self, count] { return self->removeRefusal(count).empty(); }};
    removal.description = Prop<std::string>{[self, count] { return self->removeRefusal(count); }};
    removal.onSelect = [self, doomed = std::move(doomed)] { self->removeKeys(doomed); };
    std::vector<MenuItem> entries;
    entries.push_back(insertEntry("Insert row above", false));
    entries.push_back(insertEntry("Insert row below", true));
    entries.push_back(std::move(removal));
    return entries;
}

void GridController::copySummaries(std::shared_ptr<SummaryRows> rows,
                                   std::function<std::string(Value const& record, std::string const& column)> text) {
    _summaries = std::move(rows);
    _summaryText = std::move(text);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): builds the clipboard text for the selection or the focus in one walk over the rows and the columns in layout order
std::string GridController::copy() const {
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): the untracked body of `copy`: its row and column walk shares the running text, so it is one lambda
    return _rt->untracked([&] {
        std::optional<Block> const cells = block();
        if (!cells) {
            return std::string{};
        }
        std::vector<std::vector<std::optional<std::string>>> rows;
        if (_summaries != nullptr && _summaryText && !cells->keys.empty()) {
            std::optional<std::size_t> const first = _source->positionOf(cells->keys.front());
            std::optional<std::size_t> const last = _source->positionOf(cells->keys.back());
            std::optional<std::int64_t> const from =
                first ? _summaries->mounted(static_cast<std::int64_t>(*first)) : std::nullopt;
            std::optional<std::int64_t> const target =
                last ? _summaries->mounted(static_cast<std::int64_t>(*last)) : std::nullopt;
            if (from && target) {
                // The rectangle's rows as the table draws them: a summary row between its corners is copied with
                // its cells' text, a heading row is not a row of cells.
                for (std::int64_t at = *from; at <= *target; ++at) {
                    std::optional<TableRowRef> const ref = _summaries->row(at);
                    if (!ref) {
                        continue;
                    }
                    std::vector<std::optional<std::string>> line;
                    if (_summaries->summary(at)) {
                        for (std::string const& column : cells->columns) {
                            line.emplace_back(_summaryText(ref->row, column));
                        }
                    } else if (_source->positionOf(ref->key)) {
                        for (std::string const& column : cells->columns) {
                            line.emplace_back(_source->text(ref->key, column));
                        }
                    } else {
                        continue;
                    }
                    rows.push_back(std::move(line));
                }
                return copyText({}, rows).text;
            }
        }
        for (Key const& key : cells->keys) {
            std::vector<std::optional<std::string>> line;
            for (std::string const& column : cells->columns) {
                line.emplace_back(_source->text(key, column));
            }
            rows.push_back(std::move(line));
        }
        return copyText({}, rows).text;
    });
}

std::optional<std::string> GridController::copyRefusal(std::string_view separator) const {
    std::optional<Block> const cells = _rt->untracked([&] { return block(); });
    if (cells && cells->keys.size() > _options.maxCopyRows) {
        return tableText::copyBound(_options.maxCopyRows, separator);
    }
    return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): parses the block, writes each value and counts the written and the dropped in one pass; both tallies depend on each cell's outcome
PasteResult GridController::paste(std::string_view text) {
    commit();
    letGo();
    if (!_source->selectsCells()) {
        return {};
    }
    std::vector<std::vector<std::string>> const lines = pasteLines(text);
    std::size_t values = 0;
    for (std::vector<std::string> const& line : lines) {
        values += line.size();
    }
    if (values > _options.maxPasteCells) {
        notice("A paste holds at most " + groupedCount(static_cast<std::int64_t>(_options.maxPasteCells)) + " values");
        return {};
    }
    PasteResult result;
    std::vector<std::string> const columns = _source->columns();
    std::size_t const rows = _source->rowCount();
    std::size_t firstRow = 0;
    std::size_t firstColumn = 0;
    // The focused cell, else the selection's top left corner.
    std::optional<CellRef> start = _rt->untracked([&] { return focus(); });
    if (!start) {
        if (std::optional<Block> const cells = _rt->untracked([&] { return block(); });
            cells && !cells->keys.empty() && !cells->columns.empty()) {
            start = CellRef{.key = cells->keys.front(), .column = cells->columns.front()};
        }
    }
    if (start) {
        firstRow = _source->positionOf(start->key).value_or(0);
        auto const found = std::ranges::find(columns, start->column);
        firstColumn = found == columns.end() ? 0 : static_cast<std::size_t>(found - columns.begin());
    }
    // The keys each line lands on: the rows there, then the rows appended for the lines past the last.
    std::vector<std::optional<Key>> keys;
    bool appended = false;
    for (std::size_t line = 0; line < lines.size(); ++line) {
        std::size_t const viewRow = firstRow + line;
        if (viewRow < rows) {
            keys.push_back(_source->keyAt(viewRow));
            continue;
        }
        std::optional<Key> added;
        if (_options.appendRows && !fixedRows() && _rt->untracked([&] { return insertRefusal(); }).empty()) {
            added = _source->appendRow();
        }
        appended = appended || added.has_value();
        keys.push_back(std::move(added));
    }
    _rt->batch([&] {
        for (std::size_t line = 0; line < lines.size(); ++line) {
            std::vector<std::string> const& cells = lines.at(line);
            if (!keys.at(line)) {
                result.pastRows += cells.size();
                continue;
            }
            for (std::size_t at = 0; at < cells.size(); ++at) {
                std::size_t const column = firstColumn + at;
                if (column >= columns.size()) {
                    ++result.pastColumns;
                    continue;
                }
                CellRef const cell{.key = *keys.at(line), .column = columns.at(column)};
                if (!editable(cell)) {
                    ++result.readOnly;
                    continue;
                }
                _source->write(cell.key, cell.column, cells.at(at));
                ++result.written;
            }
        }
    });
    if (result.written > 0 || appended) {
        noteChange();
        _source->committed();
    }
    notice(pasteMessage(result));
    return result;
}

void GridController::fillDown() {
    commit();
    letGo();
    if (!_source->selectsCells()) {
        return;
    }
    std::optional<Block> const cells = _rt->untracked([&] { return block(); });
    if (!cells || cells->keys.size() < 2) {
        return;
    }
    bool changed = false;
    _rt->batch([&] {
        for (std::string const& column : cells->columns) {
            std::string const first = _source->draft(cells->keys.front(), column);
            for (std::size_t row = 1; row < cells->keys.size(); ++row) {
                CellRef const cell{.key = cells->keys.at(row), .column = column};
                if (editable(cell) && _source->draft(cell.key, column) != first) {
                    _source->write(cell.key, column, first);
                    changed = true;
                }
            }
        }
    });
    if (changed) {
        noteChange();
        _source->committed();
    }
}

void GridController::clear() {
    commit();
    letGo();
    std::optional<Block> const cells = _rt->untracked([&] { return block(); });
    if (!cells) {
        return;
    }
    bool changed = false;
    _rt->batch([&] {
        for (Key const& key : cells->keys) {
            for (std::string const& column : cells->columns) {
                CellRef const cell{.key = key, .column = column};
                if (editable(cell) && !_source->draft(key, column).empty()) {
                    _source->write(key, column, std::string{});
                    changed = true;
                }
            }
        }
    });
    if (changed) {
        noteChange();
        _source->committed();
    }
}

void GridController::insertRow(bool below) {
    if (std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); })) {
        insertAt(focused->key, below);
    }
}

}  // namespace morph::ui
