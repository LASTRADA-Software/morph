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

std::string pasteMessage(PasteResult const& result) {
    std::string text = "Pasted " + std::to_string(result.written) + " values";
    std::vector<std::string> parts;
    if (result.readOnly > 0) {
        parts.push_back(std::to_string(result.readOnly) + " on read-only cells");
    }
    if (result.pastRows > 0) {
        parts.push_back(std::to_string(result.pastRows) + " past the last row");
    }
    if (result.pastColumns > 0) {
        parts.push_back(std::to_string(result.pastColumns) + " past the last column");
    }
    if (parts.empty()) {
        return text;
    }
    text += "; ";
    for (std::size_t at = 0; at < parts.size(); ++at) {
        text += (at == 0 ? "" : " and ") + parts.at(at);
    }
    return text + " were dropped";
}

bool cellReadOnly(CellFacts const& facts) {
    if (facts.field == nullptr || !facts.hasEdit) {
        return true;
    }
    return facts.field->readonly().get() || facts.overlayReadOnly || facts.companionReadOnly || facts.busy ||
           facts.locked || facts.unloaded || facts.foreignUnit;
}

std::vector<CellSpan> GridSource::spans(Key const& key) const {
    static_cast<void>(key);
    return {};
}

Node GridSource::cellContent(std::string const& column, reactive::Signal<Value> const& row) {
    reactive::Signal<Value> const* const record = &row;
    Node content = ui::text(Text{.text = Prop<std::string>{[this, record, column] {
                                     std::optional<Key> const key = keyOf(record->get());
                                     return key ? text(*key, column) : std::string{};
                                 }}});
    Prop<CellAccent> accent{[this, record, column] {
        std::optional<Key> const key = keyOf(record->get());
        std::optional<std::string> const failed = key ? error(*key, column) : std::nullopt;
        return CellAccent{
            .tone = std::nullopt, .tooltip = failed.value_or(std::string{}), .invalid = failed.has_value()};
    }};
    return accentedCell(std::move(content), std::move(accent));
}

Node GridSource::cellView(std::string const& column, reactive::Signal<Value> const& row) {
    reactive::Signal<Value> const* const record = &row;
    Prop<CellMarks> marked{[this, record, column] {
        std::optional<Key> const key = keyOf(record->get());
        return key ? marks(*key, column) : CellMarks{};
    }};
    return markedCell(cellContent(column, row), std::move(marked));
}

void GridSource::revertCell(Key const& key, std::string_view column) {
    static_cast<void>(key);
    static_cast<void>(column);
}

void GridSource::editorOpened(Key const& key, std::string_view column) {
    static_cast<void>(key);
    static_cast<void>(column);
}

std::optional<Key> GridSource::insertRow(std::size_t position) {
    static_cast<void>(position);
    return std::nullopt;
}

bool GridSource::removeRows(std::span<Key const> keys) {
    static_cast<void>(keys);
    return false;
}

std::optional<std::string> GridSource::refusal(Key const& key, std::string_view column) const {
    static_cast<void>(key);
    static_cast<void>(column);
    return std::nullopt;
}

bool GridSource::resends(Key const& key, std::string_view column) const {
    static_cast<void>(key);
    static_cast<void>(column);
    return false;
}

GridRows::GridRows(std::shared_ptr<FormRowsSource> source) : _source{checked(std::move(source))} {
    // Subscribed before the engine, so a row added while the view is ordered is known to be added when the
    // engine's change for it arrives.
    _source->subscribe(*this);
    table::EngineOptions options;
    options.owner = &_source->form().runtime().owner();
    options.worker = nullptr;
    options.services.collator = std::make_shared<ValueRowsCollator const>();
    _engine = std::make_unique<EngineRows>(_source, FormRowsSource::recordOf(), std::move(options));
    rebuild();
    // The engine's change is the view's while both views were the engine's own; a view that kept its keys in
    // order changed only in content; anything else is a reset.
    _engine->onChange([this](table::ViewChange const& change) {
        bool const alignedBefore = _aligned;
        std::vector<table::RowId> const before = keys();
        rebuild();
        if (!_onChange) {
            return;
        }
        if (alignedBefore && _aligned) {
            _onChange(change);
            return;
        }
        table::ViewChange out;
        if (keys() == before) {
            out.ops.push_back(
                table::ViewOp{.kind = table::ViewOp::Kind::Changed, .first = 0, .count = _items.size(), .to = 0});
        } else {
            out.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
        }
        _onChange(out);
    });
}

GridRows::~GridRows() {
    _engine.reset();
    _source->unsubscribe(*this);
}

void GridRows::sync() {
    if (_source->sync()) {
        _engine->engine().flush();
    }
}

TableTotal GridRows::total() const {
    return TableTotal{
        .rows = static_cast<std::int64_t>(_items.size()), .kind = table::TotalKind::Exact, .known = true};
}

std::optional<TableRowRef> GridRows::row(std::int64_t index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= _items.size()) {
        return std::nullopt;
    }
    Item const& item = _items.at(static_cast<std::size_t>(index));
    if (item.engine) {
        return _engine->row(static_cast<std::int64_t>(*item.engine));
    }
    RowRecord const* const record = heldRecord(item.key);
    if (record == nullptr) {
        return std::nullopt;
    }
    return TableRowRef{.key = record->key, .row = record->row};
}

table::UnloadedLevels GridRows::unloadedLevels(std::int64_t index) const {
    static_cast<void>(index);
    return {};
}

void GridRows::visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) {
    static_cast<void>(first);
    static_cast<void>(count);
    static_cast<void>(columns);
}

async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> GridRows::keysOf(std::int64_t first,
                                                                                                std::int64_t count) {
    using Result = std::expected<std::vector<table::RowId>, table::TableError>;
    auto [completion, promise] = async::Completion<Result>::makeSettleable(&_source->form().runtime().owner());
    auto const rows = static_cast<std::int64_t>(_items.size());
    if (first < 0 || count < 0 || count > rows - first) {
        promise.resolve(Result{std::unexpected(table::TableError{
            .code = table::TableErrorCode::InvalidSpec, .column = {}, .message = "a range outside the view"})});
        return std::move(completion);
    }
    std::vector<table::RowId> keys;
    for (std::int64_t at = first; at < first + count; ++at) {
        keys.push_back(_items.at(static_cast<std::size_t>(at)).key);
    }
    promise.resolve(Result{std::move(keys)});
    return std::move(completion);
}

void GridRows::locateRow(table::RowId key, ::core::async::StopToken stop,
                         std::function<void(RowLocation const&)> done) {
    std::optional<std::size_t> const position = positionOf(key);
    RowLocation const found{position ? std::optional{static_cast<std::int64_t>(*position)} : std::nullopt};
    _source->form().runtime().owner().post([stop = std::move(stop), done = std::move(done), found] {
        if (!stop.stop_requested()) {
            done(found);
        }
    });
}

std::expected<void, table::TableError> GridRows::setSort(SortChain sort) {
    sync();
    _sorted = !sort.empty();
    _appended.clear();
    auto result = _engine->setSort(std::move(sort));
    rebuild();
    return result;
}

std::expected<void, table::TableError> GridRows::setFilter(table::FilterSpec filter) {
    sync();
    _filtered = !(filter == table::FilterSpec{});
    _appended.clear();
    auto result = _engine->setFilter(std::move(filter));
    rebuild();
    return result;
}

TableCountsView GridRows::counts() const {
    TableCountsView counts = _engine->counts();
    counts.view = total();
    return counts;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`from`, `target`): the old key first, as every `moveKey` caller and override reads it
void GridRows::moveKey(table::RowId const& from, table::RowId const& target) {
    static_cast<void>(from);
    static_cast<void>(target);
}

void GridRows::hold(std::optional<table::RowId> key) {
    if (key == _held) {
        return;
    }
    sync();
    std::vector<table::RowId> const before = keys();
    _held = std::move(key);
    _heldAt = _held ? positionOf(*_held).value_or(_items.size()) : 0;
    rebuild();
    if (_onChange && keys() != before) {
        table::ViewChange reset;
        reset.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
        _onChange(reset);
    }
}

void GridRows::setGroup(std::function<Value(forms::RowState const&)> groupBy) {
    _source->setGroup(std::move(groupBy));
    _engine->engine().flush();
    rebuild();
}

std::optional<std::size_t> GridRows::positionOf(table::RowId const& key) const {
    auto const found = _positions.find(key);
    return found == _positions.end() ? std::nullopt : std::optional{found->second};
}

std::shared_ptr<FormRowsSource> GridRows::checked(std::shared_ptr<FormRowsSource> source) {
    if (source == nullptr) {
        throw std::invalid_argument{"morph::ui::GridRows: the source must not be null"};
    }
    return source;
}

void GridRows::rowsChanged(table::RowChange const& change) {
    _records.clear();
    if (change.kind != table::ChangeKind::Inserted || (!_sorted && !_filtered)) {
        return;
    }
    auto const snapshot = _source->snapshot();
    for (std::size_t const row : change.rows) {
        if (row < snapshot->rowCount()) {
            _appended.push_back(snapshot->rowId(row));
        }
    }
}

std::vector<table::RowId> GridRows::keys() const {
    std::vector<table::RowId> out;
    out.reserve(_items.size());
    for (Item const& item : _items) {
        out.push_back(item.key);
    }
    return out;
}

void GridRows::rebuild() {
    _items.clear();
    _positions.clear();
    std::unordered_set<table::RowId> appended(_appended.begin(), _appended.end());
    if (_held && _source->row(*_held) == nullptr) {
        _held.reset();
    }
    if (_held) {
        appended.insert(*_held);
    }
    TableTotal const engineTotal = _engine->total();
    std::unordered_map<table::RowId, std::size_t> inEngine;
    for (std::int64_t at = 0; at < engineTotal.rows; ++at) {
        std::optional<TableRowRef> const ref = _engine->row(at);
        if (!ref) {
            continue;
        }
        inEngine.emplace(ref->key, static_cast<std::size_t>(at));
        if (!appended.contains(ref->key)) {
            _items.push_back(Item{.engine = static_cast<std::size_t>(at), .key = ref->key});
        }
    }
    std::erase_if(_appended, [this](table::RowId const& key) { return _source->row(key) == nullptr; });
    auto const itemOf = [&inEngine](table::RowId const& key) {
        auto const found = inEngine.find(key);
        return Item{.engine = found == inEngine.end() ? std::nullopt : std::optional{found->second}, .key = key};
    };
    for (table::RowId const& key : _appended) {
        if (key != _held) {
            _items.push_back(itemOf(key));
        }
    }
    if (_held) {
        auto const place = static_cast<std::ptrdiff_t>(std::min(_heldAt, _items.size()));
        _items.insert(_items.begin() + place, itemOf(*_held));
    }
    _aligned = _items.size() == static_cast<std::size_t>(std::max<std::int64_t>(engineTotal.rows, 0));
    for (std::size_t at = 0; at < _items.size(); ++at) {
        _positions.emplace(_items.at(at).key, at);
        _aligned = _aligned && _items.at(at).engine == at;
    }
}

RowRecord const* GridRows::heldRecord(table::RowId const& key) const {
    if (auto const found = _records.find(key); found != _records.end()) {
        return &found->second;
    }
    auto const snapshot = _source->snapshot();
    for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
        if (snapshot->rowId(row) == key) {
            return &_records.insert_or_assign(key, FormRowsSource::recordOf()(*snapshot, row)).first->second;
        }
    }
    return nullptr;
}

ValueList summaryScopeRows(TableRows const& rows, std::int64_t first, std::int64_t count) {
    ValueList out;
    std::int64_t const last = std::min(first + std::max<std::int64_t>(count, 0), rows.total().rows);
    for (std::int64_t at = std::max<std::int64_t>(first, 0); at < last; ++at) {
        if (std::optional<TableRowRef> const ref = rows.row(at)) {
            out.push_back(ref->row);
        }
    }
    return out;
}

SummaryRows::SummaryRows(std::shared_ptr<TableRows> data, std::vector<SummarySpec> table,
                         std::vector<SummarySpec> perRun, Runs runs)
    : _data{checked(std::move(data))}, _table{std::move(table)}, _perRun{std::move(perRun)}, _runs{std::move(runs)} {
    rebuild();
    _data->onChange([this](table::ViewChange const& change) { heard(change); });
}

void SummaryRows::setHeadings(std::function<RowRecord(std::size_t run)> heading) {
    _heading = std::move(heading);
    relayout();
}

void SummaryRows::relayout() {
    ++_version;
    rebuild();
    if (_onChange) {
        table::ViewChange reset;
        reset.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
        _onChange(reset);
    }
}

TableTotal SummaryRows::mountedTotal() const {
    TableTotal out = _data->total();
    out.rows = static_cast<std::int64_t>(_entries.size());
    return out;
}

std::optional<TableRowRef> SummaryRows::row(std::int64_t index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= _entries.size()) {
        return std::nullopt;
    }
    Entry const& entry = _entries.at(static_cast<std::size_t>(index));
    if (entry.kind == Entry::Kind::Data) {
        return _data->row(entry.data);
    }
    return TableRowRef{.key = entry.key, .row = entry.record};
}

std::optional<std::int64_t> SummaryRows::mounted(std::int64_t dataRow) const {
    if (dataRow < 0 || static_cast<std::size_t>(dataRow) >= _mounted.size()) {
        return std::nullopt;
    }
    return _mounted.at(static_cast<std::size_t>(dataRow));
}

std::optional<std::int64_t> SummaryRows::data(std::int64_t mountedRow) const {
    if (mountedRow < 0 || static_cast<std::size_t>(mountedRow) >= _entries.size()) {
        return std::nullopt;
    }
    auto const index = static_cast<std::size_t>(mountedRow);
    for (std::size_t distance = 0; distance < _entries.size(); ++distance) {
        if (distance <= index && _entries.at(index - distance).kind == Entry::Kind::Data) {
            return _entries.at(index - distance).data;
        }
        if (index + distance < _entries.size() && _entries.at(index + distance).kind == Entry::Kind::Data) {
            return _entries.at(index + distance).data;
        }
    }
    return std::nullopt;
}

bool SummaryRows::summary(std::int64_t mountedRow) const {
    return mountedRow >= 0 && static_cast<std::size_t>(mountedRow) < _entries.size() &&
           _entries.at(static_cast<std::size_t>(mountedRow)).kind == Entry::Kind::Summary;
}

table::UnloadedLevels SummaryRows::unloadedLevels(std::int64_t index) const {
    std::optional<std::int64_t> const position = exact(index);
    return position ? _data->unloadedLevels(*position) : table::UnloadedLevels{};
}

void SummaryRows::visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) {
    auto const [from, rows] = dataRange(first, count);
    _data->visible(from, rows, columns);
}

async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> SummaryRows::keysOf(
    std::int64_t first, std::int64_t count) {
    auto const [from, rows] = dataRange(first, count);
    return _data->keysOf(from, rows);
}

void SummaryRows::locateRow(table::RowId key, ::core::async::StopToken stop,
                            std::function<void(RowLocation const&)> done) {
    _data->locateRow(std::move(key), std::move(stop), [this, done = std::move(done)](RowLocation const& found) {
        if (found && *found) {
            done(RowLocation{mounted(**found)});
            return;
        }
        done(found);
    });
}

async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> SummaryRows::rowsOf(
    std::int64_t first, std::int64_t count, std::span<std::string const> columns) {
    auto const [from, rows] = dataRange(first, count);
    return _data->rowsOf(from, rows, columns);
}

std::shared_ptr<TableRows> SummaryRows::checked(std::shared_ptr<TableRows> data) {
    if (data == nullptr) {
        throw std::invalid_argument{"morph::ui::SummaryRows: the data rows must not be null"};
    }
    return data;
}

std::optional<std::int64_t> SummaryRows::exact(std::int64_t index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= _entries.size()) {
        return std::nullopt;
    }
    Entry const& entry = _entries.at(static_cast<std::size_t>(index));
    return entry.kind == Entry::Kind::Data ? std::optional{entry.data} : std::nullopt;
}

std::pair<std::int64_t, std::int64_t> SummaryRows::dataRange(std::int64_t first, std::int64_t count) const {
    std::optional<std::int64_t> low;
    std::int64_t high = -1;
    auto const size = static_cast<std::int64_t>(_entries.size());
    std::int64_t const begin = std::clamp<std::int64_t>(first, 0, size);
    std::int64_t const end = std::clamp<std::int64_t>(first + std::max<std::int64_t>(count, 0), begin, size);
    for (std::int64_t at = begin; at < end; ++at) {
        if (std::optional<std::int64_t> const data = exact(at)) {
            low = low.value_or(*data);
            high = *data;
        }
    }
    if (low) {
        return {*low, high - *low + 1};
    }
    std::int64_t before = 0;
    for (std::int64_t at = 0; at < begin; ++at) {
        before += exact(at) ? 1 : 0;
    }
    return {before, 0};
}

SummaryRows::Entry SummaryRows::summaryEntry(SummarySpec const& spec, std::optional<std::size_t> run,
                                             std::int64_t first, std::int64_t count) const {
    ValueRecord record;
    record.insert_or_assign(std::string{kSummaryField}, Value{spec.key});
    record.insert_or_assign(std::string{kSummaryFirstField}, Value{first});
    record.insert_or_assign(std::string{kSummaryCountField}, Value{count});
    record.insert_or_assign(std::string{kSummaryVersionField}, Value{static_cast<std::int64_t>(_version)});
    std::string key = "\x1Fsummary:";
    if (run) {
        record.insert_or_assign(std::string{kSummaryRunField}, Value{static_cast<std::int64_t>(*run)});
        key += std::to_string(*run) + "/";
    }
    key += spec.key;
    return Entry{.kind = Entry::Kind::Summary,
                 .data = 0,
                 .perRun = run.has_value(),
                 .key = Key{std::move(key)},
                 .record = Value{std::move(record)}};
}

void SummaryRows::rebuild() {
    std::int64_t const total = std::max<std::int64_t>(_data->total().rows, 0);
    std::vector<std::pair<std::int64_t, std::int64_t>> const runs =
        _runs ? _runs() : std::vector<std::pair<std::int64_t, std::int64_t>>{};
    _entries.clear();
    auto const data = [this](std::int64_t index) {
        _entries.push_back(Entry{.kind = Entry::Kind::Data, .data = index, .perRun = false, .key = {}, .record = {}});
    };
    auto const summaries = [this](std::vector<SummarySpec> const& specs, bool top, std::optional<std::size_t> run,
                                  std::int64_t first, std::int64_t count) {
        for (SummarySpec const& spec : specs) {
            if (spec.top == top) {
                _entries.push_back(summaryEntry(spec, run, first, count));
            }
        }
    };
    summaries(_table, true, std::nullopt, 0, total);
    std::int64_t next = 0;
    for (std::size_t run = 0; run < runs.size(); ++run) {
        std::int64_t const first = std::clamp<std::int64_t>(runs.at(run).first, next, total);
        std::int64_t const last = std::clamp<std::int64_t>(first + runs.at(run).second, first, total);
        for (; next < first; ++next) {
            data(next);
        }
        if (_heading) {
            RowRecord heading = _heading(run);
            _entries.push_back(Entry{.kind = Entry::Kind::Heading,
                                     .data = 0,
                                     .perRun = true,
                                     .key = std::move(heading.key),
                                     .record = std::move(heading.row)});
        }
        summaries(_perRun, true, run, first, last - first);
        for (; next < last; ++next) {
            data(next);
        }
        summaries(_perRun, false, run, first, last - first);
    }
    for (; next < total; ++next) {
        data(next);
    }
    summaries(_table, false, std::nullopt, 0, total);
    _mounted.assign(static_cast<std::size_t>(total), std::int64_t{0});
    for (std::size_t at = 0; at < _entries.size(); ++at) {
        if (_entries.at(at).kind == Entry::Kind::Data) {
            _mounted.at(static_cast<std::size_t>(_entries.at(at).data)) = static_cast<std::int64_t>(at);
        }
    }
}

void SummaryRows::heard(table::ViewChange const& change) {
    std::vector<Entry> const before = std::move(_entries);
    auto const lead =
        static_cast<std::size_t>(std::ranges::count_if(_table, [](SummarySpec const& spec) { return spec.top; }));
    ++_version;
    rebuild();
    if (!_onChange) {
        return;
    }
    bool const reset =
        std::ranges::any_of(change.ops, [](table::ViewOp const& op) { return op.kind == table::ViewOp::Kind::Reset; });
    table::ViewChange out;
    if (!reset && !perRun(before) && !perRun(_entries)) {
        // Ungrouped: every data row sits `lead` rows down, before and after.
        for (table::ViewOp op : change.ops) {
            op.first += lead;
            if (op.kind == table::ViewOp::Kind::Moved) {
                op.to += lead;
            }
            out.ops.push_back(op);
        }
    } else {
        bool const sameShape =
            !reset && before.size() == _entries.size() &&
            std::ranges::equal(before, _entries,
                               [](Entry const& left, Entry const& right) {
                                   return left.kind == right.kind && left.data == right.data && left.key == right.key;
                               }) &&
            std::ranges::all_of(change.ops,
                                [](table::ViewOp const& op) { return op.kind == table::ViewOp::Kind::Changed; });
        if (!sameShape) {
            out.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
            _onChange(out);
            return;
        }
        for (table::ViewOp const& op : change.ops) {
            for (std::size_t at = op.first; at < op.first + op.count; ++at) {
                if (std::optional<std::int64_t> const now = mounted(static_cast<std::int64_t>(at))) {
                    out.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Changed,
                                                    .first = static_cast<std::size_t>(*now),
                                                    .count = 1,
                                                    .to = 0});
                }
            }
        }
    }
    for (std::size_t at = 0; at < _entries.size(); ++at) {
        if (_entries.at(at).kind != Entry::Kind::Data) {
            out.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Changed, .first = at, .count = 1, .to = 0});
        }
    }
    _onChange(out);
}

FormatContext formatOf(forms::FormSession const& form) {
    forms::FormSessionOptions const& options = form.options();
    FormatContext format;
    format.locale = options.bcp47;
    format.numeric = options.locale;
    format.datePattern = options.datePattern;
    format.dateTimePattern = options.dateTimePattern;
    return format;
}

}  // namespace morph::ui

namespace morph::ui::detail {

void applyUnitOverlay(std::unordered_map<std::string, std::optional<std::string>>& applied, forms::FieldState& state,
                      std::optional<std::string> const& unit,
                      std::vector<std::pair<forms::FieldState*, std::size_t>>& asLoaded) {
    auto const last = applied.find(state.path());
    bool const first = last == applied.end() || !last->second.has_value();
    bool const changed = first ? unit.has_value() : last->second != unit;
    applied.insert_or_assign(state.path(), unit);
    if (!changed) {
        return;
    }
    std::vector<forms::UnitOption> const& units = state.spec().units;
    auto const found = unit ? std::ranges::find(units, *unit, &forms::UnitOption::id) : units.end();
    std::size_t const index =
        found != units.end() ? static_cast<std::size_t>(found - units.begin()) : state.spec().entryUnit;
    if (state.unit().peek() == index) {
        return;
    }
    if (first) {
        asLoaded.emplace_back(&state, index);
    } else {
        state.switchUnit(index);
    }
}

std::string fieldText(forms::FieldState& state, std::string_view column, FormatContext const& format) {
    forms::Encoding const& encoding = state.encoded().get();
    if (!encoding.has_value() || !encoding->has_value()) {
        return state.draft();
    }
    forms::FieldSpec const& spec = state.spec();
    table::TableColumn shown;
    shown.info = table::ColumnInfo{.id = std::string{column}, .kind = columnKindOf(spec.kind)};
    if (std::optional<std::uint32_t> const places = state.places().get()) {
        shown.display.decimals = places;
    } else if (spec.decimalPlaces) {
        shown.display.decimals = spec.decimalPlaces;
    } else if (spec.displayDecimals) {
        shown.display.decimals = spec.displayDecimals;
    } else if (spec.kind == forms::FieldKind::Integer) {
        shown.display.decimals = 0;
    }
    return formatCell(memberCell(state, shown.info.kind), shown, format);
}

}  // namespace morph::ui::detail

namespace morph::ui {

FormGridSource::FormGridSource(forms::FormSession& form, std::string arrayPath, std::vector<GridColumn> columns,
                               FormatContext format)
    : _form{&form},
      _formAlive{form.alive()},
      _arrayPath{std::move(arrayPath)},
      _columns{std::move(columns)},
      _format{std::move(format)},
      _formatVersion{form.runtime(), std::uint64_t{0}},
      _layoutVersion{form.runtime(), std::uint64_t{0}} {
    forms::FieldState const& array = form.field(_arrayPath);
    if (array.spec().kind != forms::FieldKind::ObjectArray || array.spec().truncated) {
        throw std::invalid_argument{"morph::ui::FormGridSource: `" + _arrayPath + "` is not a collection"};
    }
    for (GridColumn const& column : _columns) {
        if (!column.fieldOf && !column.field.empty() &&
            std::ranges::none_of(array.spec().children,
                                 [&column](forms::FieldSpec const& member) { return member.name == column.field; })) {
            throw std::invalid_argument{"morph::ui::FormGridSource: no member `" + column.field + "` in `" +
                                        _arrayPath + "`"};
        }
        _layout.push_back(column.id);
    }
    // The rows' columns are the edit columns, by their own ids, so a sort or a filter names a column as the
    // table does; a display column has no cell there and neither sorts nor filters.
    std::vector<table::ColumnInfo> infos;
    std::vector<std::string> members;
    std::vector<std::function<std::string(forms::RowState const&)>> perRow;
    for (GridColumn const& column : _columns) {
        if (column.fieldOf) {
            // A member per row: the kind varies, so the column sorts and filters the drafts as text.
            infos.push_back(table::ColumnInfo{.id = column.id, .kind = table::ColumnKind::Text});
            members.emplace_back();
            perRow.push_back(column.fieldOf);
            continue;
        }
        auto const member = std::ranges::find(array.spec().children, column.field, &forms::FieldSpec::name);
        if (!column.field.empty() && member != array.spec().children.end()) {
            infos.push_back(table::ColumnInfo{.id = column.id, .kind = detail::columnKindOf(member->kind)});
            members.push_back(column.field);
            perRow.emplace_back();
        }
    }
    _rowsSource =
        std::make_shared<FormRowsSource>(form, _arrayPath, std::move(infos), std::move(members), std::move(perRow));
    _rows = std::make_unique<GridRows>(_rowsSource);
    if (std::ranges::any_of(_columns, [](GridColumn const& column) {
            return column.decimals || column.unit || column.options || column.placeholder;
        })) {
        _overlays = std::make_unique<reactive::Effect>(form.runtime(), [this] { applyOverlays(); });
    }
}

FormGridSource::~FormGridSource() {
    if (_hook && !_formAlive.expired()) {
        _form->removeBeforeSubmit(*_hook);
    }
}

std::size_t FormGridSource::rowCount() const {
    _rows->sync();
    return static_cast<std::size_t>(_rows->total().rows);
}

std::optional<Key> FormGridSource::keyAt(std::size_t viewRow) const {
    _rows->sync();
    std::optional<TableRowRef> const ref = _rows->row(static_cast<std::int64_t>(viewRow));
    return ref ? std::optional<Key>{ref->key} : std::nullopt;
}

std::optional<std::size_t> FormGridSource::positionOf(Key const& key) const {
    _rows->sync();
    return _rows->positionOf(key);
}

void FormGridSource::setFormat(FormatContext format) {
    _format = std::move(format);
    _formatVersion.set(_formatVersion.peek() + 1);
}

void FormGridSource::setLayout(std::vector<std::string> visible) {
    if (visible != _layout) {
        _layout = std::move(visible);
        _layoutVersion.set(_layoutVersion.peek() + 1);
    }
}

forms::FieldState* FormGridSource::field(Key const& row, std::string_view column) const {
    GridColumn const* const declared = columnOf(column);
    if (declared == nullptr || !edits(*declared)) {
        return nullptr;
    }
    forms::RowState const* const held = _rowsSource->row(row);
    return held == nullptr ? nullptr : memberOf(*declared, *held);
}

Node FormGridSource::editor(Key const& key, std::string_view column) {
    forms::FieldState* const state = field(key, column);
    if (state == nullptr) {
        return ui::text(Text{.text = std::string{}, .role = TextRole::Muted});
    }
    return forms::detail::engine::scalarControl(*_form, *state);
}

bool FormGridSource::toggles(Key const& key, std::string_view column) const {
    forms::FieldState const* const state = field(key, column);
    return state != nullptr && state->spec().kind == forms::FieldKind::Boolean;
}

bool FormGridSource::readOnly(Key const& key, std::string_view column) const {
    GridColumn const* const declared = columnOf(column);
    bool overlay = false;
    if (declared != nullptr && declared->readonly) {
        forms::RowState const* const held = _rowsSource->row(key);
        overlay = held != nullptr && declared->readonly(*held);
    }
    return cellReadOnly(CellFacts{
        .field = field(key, column), .hasEdit = declared != nullptr && edits(*declared), .overlayReadOnly = overlay});
}

bool FormGridSource::covered(Key const& key, std::string_view column) const {
    if (!_spansOf) {
        return false;
    }
    Coverage const& coverage = coverageNow();
    auto const found = coverage.covered.find(key);
    return found != coverage.covered.end() && std::ranges::contains(found->second, column);
}

std::vector<CellSpan> FormGridSource::spans(Key const& key) const {
    if (!_spansOf) {
        return {};
    }
    static_cast<void>(_layoutVersion.get());
    Coverage const& coverage = coverageNow();
    auto const found = coverage.given.find(key);
    return found == coverage.given.end() ? std::vector<CellSpan>{} : found->second;
}

void FormGridSource::setSpans(std::function<std::vector<CellSpan>(forms::RowState const&)> spansOf) {
    _spansOf = std::move(spansOf);
    _coverage.reset();
}

std::string FormGridSource::draft(Key const& key, std::string_view column) const {
    forms::FieldState const* const state = field(key, column);
    return state == nullptr ? std::string{} : _form->runtime().untracked([&] { return state->draft(); });
}

std::string FormGridSource::text(Key const& key, std::string_view column) const {
    GridColumn const* const declared = columnOf(column);
    if (declared == nullptr) {
        return {};
    }
    if (!edits(*declared)) {
        forms::RowState const* const held = _rowsSource->row(key);
        return held != nullptr && declared->value ? declared->value(*held) : std::string{};
    }
    forms::FieldState* const state = field(key, column);
    if (state == nullptr) {
        return {};
    }
    static_cast<void>(_formatVersion.get());
    return detail::fieldText(*state, column, _format);
}

std::optional<std::string> FormGridSource::error(Key const& key, std::string_view column) const {
    forms::FieldState const* const state = field(key, column);
    if (state == nullptr) {
        return std::nullopt;
    }
    std::optional<forms::FieldError> const& failed = state->error().get();
    return failed ? std::optional{failed->message} : _form->refusal(state->path());
}

void FormGridSource::write(Key const& key, std::string_view column, std::string draft) {
    if (forms::FieldState* const state = field(key, column)) {
        state->text().set(std::move(draft));
    }
}

void FormGridSource::committed() {
    _form->noteCommit();
    if (_committed) {
        _committed();
    }
}

void FormGridSource::editorOpened(Key const& key, std::string_view column) {
    if (forms::FieldState* const state = field(key, column)) {
        state->openOptions();
    }
}

std::pair<std::size_t, std::optional<std::size_t>> FormGridSource::bounds() const {
    forms::FieldSpec const& spec = _rowsSource->array().spec();
    return {spec.minItems.value_or(0), spec.maxItems};
}

std::optional<Key> FormGridSource::appendRow() {
    if (atMost()) {
        return std::nullopt;
    }
    std::uint64_t const key = _rowsSource->array().addRow();
    _rows->sync();
    return key == 0 ? std::nullopt : std::optional<Key>{Key{static_cast<std::int64_t>(key)}};
}

std::optional<Key> FormGridSource::insertRow(std::size_t position) {
    if (atMost()) {
        return std::nullopt;
    }
    forms::FieldState& array = _rowsSource->array();
    std::size_t place = _form->runtime().untracked([&] { return array.rows().size(); });
    if (std::optional<Key> const before = keyAt(position)) {
        std::int64_t const* const number = std::get_if<std::int64_t>(&*before);
        std::vector<std::shared_ptr<forms::RowState>> const& rows =
            _form->runtime().untracked([&] -> auto const& { return array.rows(); });
        auto const found = std::ranges::find_if(
            rows, [number](auto const& row) { return number != nullptr && std::cmp_equal(row->key(), *number); });
        place = static_cast<std::size_t>(found - rows.begin());
    }
    std::uint64_t const key = array.insertRow(place);
    _rows->sync();
    return key == 0 ? std::nullopt : std::optional<Key>{Key{static_cast<std::int64_t>(key)}};
}

bool FormGridSource::removeRows(std::span<Key const> keys) {
    std::vector<std::uint64_t> numbers;
    for (Key const& key : keys) {
        if (std::int64_t const* const number = std::get_if<std::int64_t>(&key); number != nullptr && *number > 0) {
            numbers.push_back(static_cast<std::uint64_t>(*number));
        }
    }
    forms::FieldState& array = _rowsSource->array();
    _removed = array.removeRows(numbers);
    _rows->sync();
    _afterRemove = array.snapshot();
    return _form->runtime().untracked(
        [&] { return std::ranges::any_of(_removed, [](forms::RemovedRow const& row) { return !row.row->blank(); }); });
}

void FormGridSource::undoRemove() {
    forms::FieldState& array = _rowsSource->array();
    std::vector<forms::RemovedRow> rows = std::exchange(_removed, {});
    if (rows.empty() || array.snapshot() != _afterRemove) {
        return;
    }
    array.restoreRows(std::move(rows));
    _rows->sync();
}

CellMarks FormGridSource::marks(Key const& key, std::string_view column) const {
    forms::FieldState const* const state = field(key, column);
    bool const derived = state != nullptr && (state->spec().computed || state->spec().derived);
    return CellMarks{.readOnly = readOnly(key, column),
                     .modified = state != nullptr && _form->modified(state->path()),
                     .error = error(key, column).has_value(),
                     .stale = derived && _form->deriveFailing(),
                     .muted = derived && _form->deriveState() == forms::DeriveState::Running};
}

std::optional<Key> FormGridSource::keyOf(Value const& record) const {
    Value const* const key = record.field(kGridKeyField);
    std::int64_t const* const number = key == nullptr ? nullptr : key->asInt();
    return number == nullptr ? std::nullopt : std::optional<Key>{Key{*number}};
}

bool FormGridSource::rowModified(Key const& key) const {
    std::int64_t const* const number = std::get_if<std::int64_t>(&key);
    return number != nullptr && *number >= 0 && _form->rowModified(_arrayPath, static_cast<std::uint64_t>(*number));
}

void FormGridSource::revertCell(Key const& key, std::string_view column) {
    if (forms::FieldState const* const state = field(key, column)) {
        _form->revert(state->path());
    }
}

void FormGridSource::revertRow(Key const& key) {
    if (std::int64_t const* const number = std::get_if<std::int64_t>(&key)) {
        _form->revert(_arrayPath + "#" + std::to_string(*number));
    }
}

void FormGridSource::onBeforeSubmit(std::function<void()> commit) {
    if (_hook) {
        _form->removeBeforeSubmit(*_hook);
    }
    _hook = _form->addBeforeSubmit(std::move(commit));
}

std::size_t FormGridSource::modifiedRows() const {
    std::size_t rows = 0;
    for (auto const& row : _rowsSource->array().rows()) {
        rows += _form->rowModified(_arrayPath, row->key()) ? 1U : 0U;
    }
    return rows;
}

std::size_t FormGridSource::errorCount() const {
    return countCells([this](forms::FieldState const& state) {
        return state.error().get().has_value() || _form->refusal(state.path()).has_value();
    });
}

std::optional<std::pair<table::Cell, table::TableColumn>> FormGridSource::filterCell(Key const& key,
                                                                                     std::string_view column) const {
    forms::FieldState const* const state = field(key, column);
    if (state == nullptr) {
        return std::nullopt;
    }
    table::TableColumn shown;
    GridColumn const* const declared = columnOf(column);
    if (declared != nullptr && declared->fieldOf) {
        // The rows' column holds text, whatever the row's member is.
        shown.info = table::ColumnInfo{.id = std::string{column}, .kind = table::ColumnKind::Text};
        return std::pair{detail::perRowCell(*state), shown};
    }
    shown.info = table::ColumnInfo{.id = std::string{column}, .kind = detail::columnKindOf(state->spec().kind)};
    return std::pair{detail::memberCell(*state, shown.info.kind), shown};
}

bool FormGridSource::atMost() const {
    std::optional<std::size_t> const most = bounds().second;
    return most && _form->runtime().untracked([&] { return heldRows(); }) >= *most;
}

GridColumn const* FormGridSource::columnOf(std::string_view columnId) const {
    auto const found = std::ranges::find(_columns, columnId, &GridColumn::id);
    return found == _columns.end() ? nullptr : &*found;
}

forms::FieldState* FormGridSource::memberOf(GridColumn const& column, forms::RowState const& row) {
    if (column.fieldOf) {
        return row.field(column.fieldOf(row));
    }
    return column.field.empty() ? nullptr : row.field(column.field);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): applies each column's overlay to each row's member in one pass over the rows; the places, entry unit, options and placeholder steps read the same row state
void FormGridSource::applyOverlays() {
    // Forget the units applied to members of rows that are gone: a reload builds every row anew.
    if (!_unitApplied.empty()) {
        std::unordered_set<std::string> present;
        for (auto const& row : _rowsSource->array().rows()) {
            for (auto const& member : row->fields()) {
                present.insert(member->path());
            }
        }
        std::erase_if(_unitApplied, [&present](auto const& entry) { return !present.contains(entry.first); });
    }
    // The first units a load's rows take, switched together after the walk (one batch, one pass over the form).
    std::vector<std::pair<forms::FieldState*, std::size_t>> asLoaded;
    for (auto const& row : _rowsSource->array().rows()) {
        for (GridColumn const& column : _columns) {
            forms::FieldState* const state = memberOf(column, *row);
            if (state == nullptr) {
                continue;
            }
            std::optional<std::uint32_t> const places = column.decimals ? column.decimals(*row) : std::nullopt;
            std::optional<std::string> const unit = column.unit ? column.unit(*row) : std::nullopt;
            std::optional<std::vector<std::string>> options = column.options ? column.options(*row) : std::nullopt;
            std::string placeholder = column.placeholder ? column.placeholder(*row) : std::string{};
            _form->runtime().untracked([&] {
                if (column.decimals && state->places().peek() != places) {
                    state->places().set(places);
                }
                if (column.unit) {
                    applyUnit(*state, unit, asLoaded);
                }
                if (column.options) {
                    state->narrowOptions(std::move(options));
                }
                if (column.placeholder) {
                    state->overridePlaceholder(std::move(placeholder));
                }
            });
        }
    }
    _form->runtime().untracked([&] { _form->switchUnitsAsLoaded(asLoaded); });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): rows, their spans, and the cells each covers.
FormGridSource::Coverage const& FormGridSource::coverageNow() const {
    _rows->sync();
    std::shared_ptr<table::RowSnapshot const> const snapshot = _rowsSource->snapshot();
    bool const sorted = _rows->sorted();
    if (_coverage && _coverage->snapshot == snapshot && _coverage->layout == _layout && _coverage->sorted == sorted) {
        return *_coverage;
    }
    Coverage next{.snapshot = snapshot, .layout = _layout, .sorted = sorted, .covered = {}, .given = {}};
    Virtualization const limits{};
    auto const rows = static_cast<std::size_t>(std::max<std::int64_t>(_rows->total().rows, 0));
    bool const applies = !sorted && rows < limits.threshold && _layout.size() < limits.columnThreshold &&
                         rows * _layout.size() < limits.cellThreshold;
    if (applies) {
        std::vector<std::string> declared;
        declared.reserve(_columns.size());
        for (GridColumn const& column : _columns) {
            declared.push_back(column.id);
        }
        // NOLINTNEXTLINE(readability-function-cognitive-complexity): one untracked read of the form: the overlay steps share the locals the reads fill, so they stay in one lambda
        _form->runtime().untracked([&] {
            for (std::size_t row = 0; row < rows; ++row) {
                std::optional<TableRowRef> const ref = _rows->row(static_cast<std::int64_t>(row));
                forms::RowState const* const held = ref ? _rowsSource->row(ref->key) : nullptr;
                if (held == nullptr) {
                    continue;
                }
                for (CellSpan span : _spansOf(*held)) {
                    span.columns = cutColumns(span, declared, _layout);
                    auto const place = std::ranges::find(_layout, span.column);
                    // A span past the last row is refused by the mount and drawn as nothing, so it covers
                    // nothing here either.
                    if (span.columns == 0 || span.rows == 0 || place == _layout.end() || span.rows > rows - row) {
                        continue;
                    }
                    auto const first = static_cast<std::size_t>(place - _layout.begin());
                    for (std::size_t down = 0; down < span.rows; ++down) {
                        std::optional<TableRowRef> const below = _rows->row(static_cast<std::int64_t>(row + down));
                        if (!below) {
                            continue;
                        }
                        for (std::size_t right = 0; right < span.columns; ++right) {
                            if (down != 0 || right != 0) {
                                next.covered[below->key].push_back(_layout.at(first + right));
                            }
                        }
                    }
                    next.given[ref->key].push_back(std::move(span));
                }
            }
        });
    }
    _coverage = std::move(next);
    return *_coverage;
}

FixedGridSource::FixedGridSource(reactive::Runtime& runtime, forms::FormSession* form, std::vector<Part> rows,
                                 std::vector<GridColumn> columns, FormatContext format)
    : _rt{&runtime},
      _form{form},
      _formAlive{form != nullptr ? form->alive() : std::weak_ptr<void const>{}},
      _parts{std::move(rows)},
      _columns{std::move(columns)},
      _format{std::move(format)},
      _formatVersion{runtime, std::uint64_t{0}},
      _layoutVersion{runtime, std::uint64_t{0}},
      _data{std::make_shared<table::VectorSource>(std::vector<table::ColumnInfo>{
          table::ColumnInfo{.id = std::string{kGridKeyField}, .kind = table::ColumnKind::Text}})} {
    for (GridColumn const& column : _columns) {
        _layout.push_back(column.id);
    }
    checkParts();
    expand();
    table::EngineOptions options;
    options.owner = &runtime.owner();
    _rows = std::make_unique<EngineRows>(
        _data,
        [](table::RowSnapshot const& snapshot, std::size_t row) {
            table::RowId key = snapshot.rowId(row);
            ValueRecord record;
            record.insert_or_assign(std::string{kGridKeyField}, Value{std::get<std::string>(key)});
            return RowRecord{.key = std::move(key), .row = Value{std::move(record)}};
        },
        std::move(options));
    _rows->engine().flush();
    if (std::ranges::any_of(_parts, [](Part const& part) { return std::holds_alternative<FixedBlock>(part); })) {
        _watch = std::make_unique<reactive::Effect>(runtime, [this] { watch(); });
    }
    if (std::ranges::any_of(_parts, [](Part const& part) {
            return std::ranges::any_of(rowsOf(part), [](FixedRow const& row) {
                return std::ranges::any_of(row.cells, [](auto const& cell) {
                    FixedCell const& given = cell.second;
                    return !given.field.empty() &&
                           (given.decimals || given.unit || given.options || given.placeholder);
                });
            });
        })) {
        _overlays = std::make_unique<reactive::Effect>(runtime, [this] { applyOverlays(); });
    }
}

FixedGridSource::~FixedGridSource() {
    _overlays.reset();
    _watch.reset();
    if (_hook && _form != nullptr && !_formAlive.expired()) {
        _form->removeBeforeSubmit(*_hook);
    }
}

std::size_t FixedGridSource::rowCount() const {
    sync();
    return _expanded.size();
}

std::optional<Key> FixedGridSource::keyAt(std::size_t viewRow) const {
    sync();
    return viewRow < _expanded.size() ? std::optional<Key>{_expanded.at(viewRow).key} : std::nullopt;
}

std::optional<std::size_t> FixedGridSource::positionOf(Key const& key) const {
    sync();
    auto const found = _index.find(key);
    return found == _index.end() ? std::nullopt : std::optional{found->second};
}

void FixedGridSource::setFormat(FormatContext format) {
    _format = std::move(format);
    _formatVersion.set(_formatVersion.peek() + 1);
}

void FixedGridSource::setLayout(std::vector<std::string> visible) {
    if (visible != _layout) {
        _layout = std::move(visible);
        _layoutVersion.set(_layoutVersion.peek() + 1);
    }
}

forms::FieldState* FixedGridSource::field(Key const& key, std::string_view column) const {
    Expanded const* const row = expandedOf(key);
    if (row == nullptr) {
        return nullptr;
    }
    auto const found = row->fields.find(column);
    return found == row->fields.end() ? nullptr : found->second;
}

std::string FixedGridSource::label(Key const& key) const {
    Expanded const* const row = expandedOf(key);
    return row == nullptr ? std::string{} : row->row->label;
}

Node FixedGridSource::editor(Key const& key, std::string_view column) {
    forms::FieldState* const state = field(key, column);
    if (state == nullptr || _form == nullptr) {
        return ui::text(Text{.text = std::string{}, .role = TextRole::Muted});
    }
    return forms::detail::engine::scalarControl(*_form, *state);
}

bool FixedGridSource::toggles(Key const& key, std::string_view column) const {
    forms::FieldState const* const state = field(key, column);
    return state != nullptr && state->spec().kind == forms::FieldKind::Boolean;
}

bool FixedGridSource::readOnly(Key const& key, std::string_view column) const {
    auto const [cell, specimen] = cellOf(key, column);
    bool const overlay = cell != nullptr && cell->readonly && cell->readonly(specimen);
    return cellReadOnly(CellFacts{
        .field = field(key, column), .hasEdit = cell != nullptr && !cell->field.empty(), .overlayReadOnly = overlay});
}

bool FixedGridSource::covered(Key const& key, std::string_view column) const {
    Coverage const& coverage = coverageNow();
    auto const found = coverage.covered.find(key);
    return found != coverage.covered.end() && std::ranges::contains(found->second, column);
}

std::vector<CellSpan> FixedGridSource::spans(Key const& key) const {
    static_cast<void>(_layoutVersion.get());
    Coverage const& coverage = coverageNow();
    auto const found = coverage.given.find(key);
    return found == coverage.given.end() ? std::vector<CellSpan>{} : found->second;
}

bool FixedGridSource::spanning() const {
    return std::ranges::any_of(_parts, [](Part const& part) {
        return std::ranges::any_of(rowsOf(part), [](FixedRow const& row) {
            return std::ranges::any_of(
                row.cells, [](auto const& cell) { return cell.second.rows > 1 || cell.second.columns > 1; });
        });
    });
}

std::string FixedGridSource::draft(Key const& key, std::string_view column) const {
    forms::FieldState const* const state = field(key, column);
    return state == nullptr ? std::string{} : _rt->untracked([&] { return state->draft(); });
}

std::string FixedGridSource::text(Key const& key, std::string_view column) const {
    auto const [cell, specimen] = cellOf(key, column);
    if (cell == nullptr) {
        return {};
    }
    if (cell->field.empty()) {
        return cell->value ? cell->value(specimen) : cell->text;
    }
    forms::FieldState* const state = field(key, column);
    if (state == nullptr) {
        return {};
    }
    static_cast<void>(_formatVersion.get());
    return detail::fieldText(*state, column, _format);
}

std::optional<std::string> FixedGridSource::error(Key const& key, std::string_view column) const {
    forms::FieldState const* const state = field(key, column);
    if (state == nullptr) {
        return std::nullopt;
    }
    std::optional<forms::FieldError> const& failed = state->error().get();
    return failed ? std::optional{failed->message} : _form->refusal(state->path());
}

void FixedGridSource::write(Key const& key, std::string_view column, std::string draft) {
    if (forms::FieldState* const state = field(key, column)) {
        state->text().set(std::move(draft));
    }
}

void FixedGridSource::committed() {
    if (_form != nullptr) {
        _form->noteCommit();
    }
}

void FixedGridSource::editorOpened(Key const& key, std::string_view column) {
    if (forms::FieldState* const state = field(key, column)) {
        state->openOptions();
    }
}

void FixedGridSource::recalculate() {
    if (_form != nullptr) {
        _form->recalculate();
    }
}

CellMarks FixedGridSource::marks(Key const& key, std::string_view column) const {
    forms::FieldState const* const state = field(key, column);
    bool const derived = state != nullptr && (state->spec().computed || state->spec().derived);
    return CellMarks{.readOnly = readOnly(key, column),
                     .modified = state != nullptr && _form->modified(state->path()),
                     .error = error(key, column).has_value(),
                     .stale = derived && _form->deriveFailing(),
                     .muted = derived && _form->deriveState() == forms::DeriveState::Running};
}

std::optional<Key> FixedGridSource::keyOf(Value const& record) const {
    Value const* const key = record.field(kGridKeyField);
    std::string const* const text = key == nullptr ? nullptr : key->asString();
    return text == nullptr ? std::nullopt : std::optional<Key>{Key{*text}};
}

bool FixedGridSource::rowModified(Key const& key) const {
    Expanded const* const row = expandedOf(key);
    return row != nullptr && std::ranges::any_of(row->fields, [this](auto const& entry) {
               return entry.second != nullptr && _form->modified(entry.second->path());
           });
}

EngineRows& FixedGridSource::view() {
    sync();
    return *_rows;
}

void FixedGridSource::revertCell(Key const& key, std::string_view column) {
    if (forms::FieldState const* const state = field(key, column)) {
        _form->revert(state->path());
    }
}

void FixedGridSource::revertRow(Key const& key) {
    Expanded const* const row = expandedOf(key);
    if (row == nullptr) {
        return;
    }
    std::vector<std::string> paths;
    for (auto const& [column, state] : row->fields) {
        if (state != nullptr) {
            paths.push_back(state->path());
        }
    }
    for (std::string const& path : paths) {
        _form->revert(path);
    }
}

void FixedGridSource::onBeforeSubmit(std::function<void()> commit) {
    if (_form == nullptr) {
        return;
    }
    if (_hook) {
        _form->removeBeforeSubmit(*_hook);
    }
    _hook = _form->addBeforeSubmit(std::move(commit));
}

std::size_t FixedGridSource::modifiedRows() const {
    sync();
    return static_cast<std::size_t>(
        std::ranges::count_if(_expanded, [this](Expanded const& row) { return rowModified(row.key); }));
}

std::size_t FixedGridSource::errorCount() const {
    return countCells([this](forms::FieldState const& state) {
        return state.error().get().has_value() || _form->refusal(state.path()).has_value();
    });
}

std::span<FixedRow const> FixedGridSource::rowsOf(Part const& part) {
    if (FixedRow const* const row = std::get_if<FixedRow>(&part)) {
        return {row, 1};
    }
    return std::get<FixedBlock>(part).rows;
}

forms::FieldSpec const* FixedGridSource::specAt(std::span<forms::FieldSpec const> fields, std::string_view path) {
    forms::FieldSpec const* found = nullptr;
    while (!path.empty()) {
        std::size_t const dot = path.find('.');
        std::string_view const name = path.substr(0, dot);
        path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
        auto const member = std::ranges::find(fields, name, &forms::FieldSpec::name);
        if (member == fields.end()) {
            return nullptr;
        }
        found = &*member;
        fields = member->children;
    }
    return found;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one refusal per rule of a part.
void FixedGridSource::checkParts() const {
    std::unordered_set<std::string> keys;
    for (Part const& part : _parts) {
        FixedBlock const* const block = std::get_if<FixedBlock>(&part);
        std::span<forms::FieldSpec const> members;
        if (block != nullptr) {
            if (_form == nullptr) {
                refuse("a block repeats rows of a form's collection, and there is no form");
            }
            forms::FieldState const* array = nullptr;
            try {
                array = &_form->field(block->forEach);
            } catch (std::out_of_range const&) {
                refuse("`" + block->forEach + "` is not a field of the form");
            }
            if (array->spec().kind != forms::FieldKind::ObjectArray || array->spec().truncated) {
                refuse("`" + block->forEach + "` is not a collection");
            }
            members = array->spec().children;
        }
        for (FixedRow const& row : rowsOf(part)) {
            if (row.key.contains('/')) {
                refuse("the key `" + row.key + "` holds a `/`, which joins a block row's keys");
            }
            if (row.key.empty() || !keys.insert(row.key).second) {
                refuse(row.key.empty() ? std::string{"a row has no key"} : "the key `" + row.key + "` is given twice");
            }
            for (auto const& [column, cell] : row.cells) {
                if (cell.field.empty()) {
                    continue;
                }
                if (_form == nullptr) {
                    refuse("the cell `" + row.key + "`, `" + column + "` edits a field, and there is no form");
                }
                bool const known = block != nullptr ? specAt(members, cell.field) != nullptr : [&] {
                    try {
                        static_cast<void>(_form->field(cell.field));
                        return true;
                    } catch (std::out_of_range const&) {
                        return false;
                    }
                }();
                if (!known) {
                    refuse("`" + cell.field + "` is not a field of the form");
                }
            }
        }
    }
}

}  // namespace morph::ui
