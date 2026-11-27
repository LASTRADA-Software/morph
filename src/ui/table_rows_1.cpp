// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/table_rows.hpp>.

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <glaze/glaze.hpp>
#include <limits>
#include <map>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/change_types.hpp>
#include <morph/core/completion.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/render/locale_format.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <morph/table/row_selection.hpp>
#include <morph/table/state_store.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/table_config.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

std::string selectionJsonOf(TableSelection const& selection) {
    return table::detail::selectionJson(
        selection.keys, selection.view ? &selection.view->filters : nullptr,
        selection.view ? std::span<table::RowId const>{selection.view->retained} : std::span<table::RowId const>{},
        selection.view ? std::string_view{selection.view->revision} : std::string_view{}, selection.except);
}

std::string selectAllRefusalText(SelectAllRefusal const& refusal, FormatContext const& locale) {
    if (refusal.undecided > 0) {
        return "Select all is available once " +
               render::formatCanonicalNumber(std::to_string(refusal.undecided), locale.numeric) +
               (refusal.undecided == 1 ? " row is decided" : " rows are decided");
    }
    if (refusal.levels.empty()) {
        return "Select all is available once the rows have loaded";
    }
    std::string levels;
    for (std::string const& level : refusal.levels) {
        levels += (levels.empty() ? "" : " and ") + level;
    }
    return "Select all is available once " + levels + " data has loaded";
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one walk down the reply along the catalog's path.
ChangedRowsValue changedRowsOf(Value const& reply, ActionInfo const& action, std::string_view record) {
    ChangedRowsValue out;
    auto const keyOf = [](Value const* key) -> std::optional<table::RowId> {
        if (key == nullptr) {
            return std::nullopt;
        }
        if (auto const* const number = key->asInt()) {
            return table::RowId{*number};
        }
        if (auto const* const text = key->asString()) {
            return table::RowId{*text};
        }
        return std::nullopt;
    };
    for (ActionRecordInfo const& info : action.records) {
        if (info.record != record) {
            continue;
        }
        for (std::string const& path : info.paths) {
            std::string_view parent;
            if (path == "rows") {
                parent = {};
            } else if (path.ends_with(".rows")) {
                parent = std::string_view{path}.substr(0, path.size() - std::string_view{".rows"}.size());
            } else {
                continue;
            }
            Value const* node = &reply;
            while (node != nullptr && !parent.empty()) {
                auto const dot = parent.find('.');
                node = node->field(parent.substr(0, dot));
                parent = dot == std::string_view::npos ? std::string_view{} : parent.substr(dot + 1);
            }
            if (node == nullptr) {
                continue;
            }
            if (Value const* const rows = node->field("rows"); rows != nullptr && rows->asList() != nullptr) {
                for (Value const& row : *rows->asList()) {
                    if (auto key = keyOf(row.field("key"))) {
                        out.rows.push_back(
                            table::ChangedRow{.key = std::move(*key), .from = keyOf(row.field("from"))});
                    }
                }
            }
            if (Value const* const failed = node->field("failed"); failed != nullptr && failed->asList() != nullptr) {
                for (Value const& row : *failed->asList()) {
                    if (auto key = keyOf(row.field("key"))) {
                        Value const* const message = row.field("message");
                        out.failed.push_back(table::FailedRow{
                            .key = std::move(*key),
                            .message = message != nullptr && message->asString() != nullptr ? *message->asString()
                                                                                            : std::string{}});
                    }
                }
            }
        }
    }
    return out;
}

void RowStates::busyKeys(std::uint64_t run, std::vector<table::RowId> keys) {
    std::vector<table::RowId>& held = _keyRuns[run];
    for (table::RowId& key : keys) {
        if (!std::ranges::contains(held, key)) {
            held.push_back(std::move(key));
        }
    }
}

void RowStates::busyView(std::uint64_t run, std::vector<table::RowId> except, std::int64_t count) {
    _viewRuns.insert_or_assign(run, ViewRun{.except = std::move(except), .count = std::max<std::int64_t>(count, 0)});
}

void RowStates::settle(std::uint64_t run) {
    _keyRuns.erase(run);
    _viewRuns.erase(run);
}

void RowStates::fail(std::span<table::FailedRow const> rows) {
    for (table::FailedRow const& row : rows) {
        auto const found = std::ranges::find(_failed, row.key, &table::FailedRow::key);
        if (found != _failed.end()) {
            found->message = row.message;
        } else {
            _failed.push_back(row);
        }
    }
}

void RowStates::move(table::RowId const& from, table::RowId const& target) {
    if (from == target) {
        return;
    }
    for (auto& [run, keys] : _keyRuns) {
        auto const old = std::ranges::find(keys, from);
        if (old == keys.end()) {
            continue;
        }
        if (std::ranges::contains(keys, target)) {
            keys.erase(old);
        } else {
            *old = target;
        }
    }
    // A view run holds a row unless its `except` names it: a busy old key makes the new key busy, and is then
    // left out itself.
    for (auto& [run, view] : _viewRuns) {
        if (!std::ranges::contains(view.except, from)) {
            std::erase(view.except, target);
            view.except.push_back(from);
        }
    }
    auto const old = std::ranges::find(_failed, from, &table::FailedRow::key);
    if (old == _failed.end()) {
        return;
    }
    if (std::ranges::contains(_failed, target, &table::FailedRow::key)) {
        _failed.erase(old);
    } else {
        old->key = target;
    }
}

bool RowStates::isBusy(table::RowId const& key) const {
    auto const holds = [&key](auto const& run) { return std::ranges::contains(run.second, key); };
    auto const leavesIn = [&key](auto const& run) { return !std::ranges::contains(run.second.except, key); };
    return std::ranges::any_of(_keyRuns, holds) || std::ranges::any_of(_viewRuns, leavesIn);
}

std::optional<std::string> RowStates::failure(table::RowId const& key) const {
    auto const found = std::ranges::find(_failed, key, &table::FailedRow::key);
    return found == _failed.end() ? std::nullopt : std::optional{found->message};
}

std::int64_t RowStates::busyCount() const {
    std::unordered_set<table::RowId> keys;
    for (auto const& [run, held] : _keyRuns) {
        for (table::RowId const& key : held) {
            auto const leavesOut = [&key](auto const& view) { return std::ranges::contains(view.second.except, key); };
            if (std::ranges::all_of(_viewRuns, leavesOut)) {
                keys.insert(key);
            }
        }
    }
    std::int64_t largest = 0;
    for (auto const& [run, view] : _viewRuns) {
        largest = std::max(largest, view.count);
    }
    return largest + static_cast<std::int64_t>(keys.size());
}

std::vector<table::RowId> RowStates::failedKeys() const {
    std::vector<table::RowId> out;
    out.reserve(_failed.size());
    for (table::FailedRow const& row : _failed) {
        out.push_back(row.key);
    }
    return out;
}

bool TableRows::leftView(table::RowId const& key) const {
    static_cast<void>(key);
    return false;
}

RetainResult TableRows::retain(std::span<table::RowId const> keys) {
    static_cast<void>(keys);
    return RetainResult::Retained;
}

bool TableRows::unmatched(table::RowId const& key) const {
    static_cast<void>(key);
    return false;
}

async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> TableRows::rowsOf(
    std::int64_t first, std::int64_t count, std::span<std::string const> columns) {
    static_cast<void>(first);
    static_cast<void>(count);
    static_cast<void>(columns);
    return fetchRows({}, {});
}

std::optional<std::string> lockText(TableRowRef const& row, table::TableDescription const& description) {
    if (description.rowLock.empty()) {
        return std::nullopt;
    }
    Value const* const cell = row.row.field(description.rowLock);
    std::string const* const text = cell == nullptr ? nullptr : cell->asString();
    return text == nullptr || text->empty() ? std::nullopt : std::optional{*text};
}

bool refusesEdit(TableRows const& rows, std::int64_t index, table::TableDescription const& description) {
    std::optional<TableRowRef> const row = rows.row(index);
    return row && (rows.states().isBusy(row->key) || lockText(*row, description).has_value());
}

}  // namespace morph::ui

namespace morph::ui::detail {

Value doubleValue(double value) {
    if (std::isfinite(value)) {
        // The longest fixed text of a finite double: 309 integer digits, a sign, a point and 767 fraction digits.
        std::array<char, 1100> fixed{};
        auto const written =
            std::to_chars(fixed.data(), std::to_address(fixed.end()), value, std::chars_format::fixed);
        if (written.ec == std::errc{}) {
            if (auto const parsed = table::parseDecimal(std::string_view{fixed.data(), written.ptr})) {
                return Value{Decimal{*parsed}};
            }
        }
    }
    std::array<char, 64> shortest{};
    auto const written = std::to_chars(shortest.data(), std::to_address(shortest.end()), value);
    return Value{std::string{shortest.data(), written.ptr}};
}

Value dateValue(std::int64_t days) {
    // Days of 0000-01-01 and 9999-12-31 from the epoch: the range a four-digit year spells.
    constexpr std::int64_t kFirst = -719'528;
    constexpr std::int64_t kLast = 2'932'896;
    if (days < kFirst || days > kLast) {
        return Value{std::to_string(days)};
    }
    std::chrono::year_month_day const date{std::chrono::sys_days{std::chrono::days{static_cast<int>(days)}}};
    return Value{std::format("{:04}-{:02}-{:02}", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                             static_cast<unsigned>(date.day()))};
}

Value dateTimeValue(std::int64_t seconds) {
    constexpr std::int64_t kMillis = 1000;
    if (seconds > std::numeric_limits<std::int64_t>::max() / kMillis ||
        seconds < std::numeric_limits<std::int64_t>::min() / kMillis) {
        return Value{std::to_string(seconds)};
    }
    return Value{time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{seconds * kMillis}}}};
}

}  // namespace morph::ui::detail

namespace morph::ui {

Value cellValue(table::Cell const& cell, table::ColumnInfo const& column, table::UnitRegistry const* units) {
    using table::ColumnKind;
    return std::visit(
        [&]<typename T>(T const& value) -> Value {
            if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, table::Unloaded>) {
                return Value{};
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                if (column.kind == ColumnKind::Date) {
                    return detail::dateValue(value);
                }
                if (column.kind == ColumnKind::DateTime) {
                    return detail::dateTimeValue(value);
                }
                return Value{value};
            } else if constexpr (std::is_same_v<T, math::Rational>) {
                return Value{Decimal{value}};
            } else if constexpr (std::is_same_v<T, double>) {
                return detail::doubleValue(value);
            } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, bool>) {
                return Value{value};
            } else if constexpr (std::is_same_v<T, table::Measure>) {
                if (units != nullptr && units->contains(value.unit)) {
                    return Value{Quantity{.value = value.amount, .unit = units->info(value.unit).id}};
                }
                return Value{Decimal{value.amount}};
            } else {
                static_assert(std::is_same_v<T, table::QuantityCell>);
                // A product that does not fit is shown as the amount's own text, as the engine keys it invalid.
                if (auto const canonical = math::checkedMul(value.amount, value.toCanonical)) {
                    return Value{Decimal{*canonical}};
                }
                return Value{table::displayText(cell)};
            }
        },
        cell);
}

std::string countsText(TableCountsView const& counts, std::int64_t selected, FormatContext const& format) {
    if (!counts.view.known) {
        return {};
    }
    auto const grouped = [&format](std::int64_t number) {
        return render::formatCanonicalNumber(std::to_string(number), format.numeric);
    };
    bool const lowerBound = counts.view.kind == table::TotalKind::AtLeast;
    std::string view = grouped(counts.view.rows) + (lowerBound ? " or more" : "");
    if (counts.undecided > 0) {
        return grouped(counts.view.rows) + " so far, " + grouped(counts.undecided) + " loading";
    }
    if (selected > 0) {
        return grouped(selected) + " of " + view + " selected";
    }
    if (lowerBound) {
        return view;
    }
    return counts.view.rows == 1 ? std::string{"1 row"} : view + " rows";
}

EngineRows::EngineRows(std::shared_ptr<table::DataSource> source, RecordOf recordOf, table::EngineOptions options)
    : _source{checked(std::move(source), options)},
      _recordOf{std::move(recordOf)},
      _owner{options.owner},
      _engine{std::make_unique<table::Engine>(_source, std::move(options))},
      _memoSnapshot{_engine->viewSnapshot()} {
    _source->subscribe(*this);
    _engine->onViewChange([this](table::ViewChange const& change) {
        sync();
        if (_onChange) {
            _onChange(change);
        }
    });
}

EngineRows::~EngineRows() {
    _engine->onViewChange({});
    _source->unsubscribe(*this);
}

void EngineRows::setRestoring(bool restoring) {
    if (std::exchange(_restoring, restoring) == restoring) {
        return;
    }
    if (_onChange) {
        table::ViewChange change;
        change.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
        _onChange(change);
    }
}

TableTotal EngineRows::total() const {
    if (_restoring) {
        return TableTotal{.rows = static_cast<std::int64_t>(_engine->viewRowCount()),
                          .kind = table::TotalKind::Exact,
                          .known = false};
    }
    return TableTotal{
        .rows = static_cast<std::int64_t>(_engine->viewRowCount()), .kind = table::TotalKind::Exact, .known = true};
}

std::optional<TableRowRef> EngineRows::row(std::int64_t index) const {
    if (_restoring) {
        return std::nullopt;
    }
    auto const* const entry = entryAt(index);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return TableRowRef{.key = entry->record.key, .row = entry->record.row};
}

table::UnloadedLevels EngineRows::unloadedLevels(std::int64_t index) const {
    auto const* const entry = entryAt(index);
    if (entry == nullptr) {
        return {};
    }
    return table::UnloadedLevels{.levels = entry->levels, .failed = {}};
}

void EngineRows::visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) {
    static_cast<void>(first);
    static_cast<void>(count);
    static_cast<void>(columns);
}

async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> EngineRows::keysOf(std::int64_t first,
                                                                                                  std::int64_t count) {
    using Result = std::expected<std::vector<table::RowId>, table::TableError>;
    auto [completion, promise] = async::Completion<Result>::makeSettleable(_owner);
    auto const rows = static_cast<std::int64_t>(_engine->viewRowCount());
    if (first < 0 || count < 0 || count > rows - first) {
        promise.resolve(Result{std::unexpected(table::TableError{
            .code = table::TableErrorCode::InvalidSpec,
            .column = {},
            .message = std::format("{} rows from view row {} are not in a view of {}", count, first, rows)})});
        return std::move(completion);
    }
    std::vector<table::RowId> keys;
    keys.reserve(static_cast<std::size_t>(count));
    for (std::int64_t at = first; at < first + count; ++at) {
        keys.push_back(_engine->rowIdAt(static_cast<std::size_t>(at)));
    }
    promise.resolve(Result{std::move(keys)});
    return std::move(completion);
}

async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> EngineRows::fetchRows(
    std::vector<table::RowId> keys, std::vector<std::string> columns) {
    using Result = std::expected<std::vector<FetchedRow>, table::TableError>;
    auto [completion, promise] = async::Completion<Result>::makeSettleable(_owner);
    auto const infos = _engine->columns();
    std::vector<std::optional<std::size_t>> places;
    places.reserve(columns.size());
    for (std::string const& columnId : columns) {
        auto const found = std::ranges::find(infos, columnId, &table::ColumnInfo::id);
        places.push_back(found == infos.end() ? std::nullopt
                                              : std::optional{static_cast<std::size_t>(found - infos.begin())});
    }
    auto const& snapshot = *_engine->viewSnapshot();
    std::vector<FetchedRow> rows;
    for (table::RowId& key : keys) {
        std::optional<std::size_t> const viewRow = _engine->viewRowOfKey(key);
        std::optional<std::size_t> const sourceRow = viewRow ? _engine->sourceRowOf(*viewRow) : std::nullopt;
        if (!sourceRow) {
            continue;
        }
        FetchedRow row{.key = std::move(key), .viewRow = static_cast<std::int64_t>(*viewRow), .cells = {}};
        row.cells.reserve(places.size());
        for (std::optional<std::size_t> const place : places) {
            row.cells.push_back(place ? snapshot.cell(*sourceRow, *place) : table::Cell{});
        }
        rows.push_back(std::move(row));
    }
    promise.resolve(Result{std::move(rows)});
    return std::move(completion);
}

std::expected<ViewSelection, SelectAllRefusal> EngineRows::selectView() const {
    table::MembershipDigest digest;
    for (std::size_t at = 0; at < _engine->viewRowCount(); ++at) {
        digest.add(_engine->rowIdAt(at));
    }
    auto const retained = _engine->retained();
    return ViewSelection{
        .filters = _engine->filter(), .retained = {retained.begin(), retained.end()}, .revision = digest.text()};
}

SelectedCount EngineRows::selectedCount(TableSelection const& selection) const {
    if (!selection.view) {
        return SelectedCount{.rows = static_cast<std::int64_t>(selection.keys.size()), .kind = CountKind::Exact};
    }
    auto rows = static_cast<std::int64_t>(_engine->viewRowCount());
    for (table::RowId const& key : selection.except) {
        if (_engine->viewRowOfKey(key)) {
            --rows;
        }
    }
    return SelectedCount{.rows = rows, .kind = CountKind::Exact};
}

async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> EngineRows::rowsOf(
    std::int64_t first, std::int64_t count, std::span<std::string const> columns) {
    auto const rows = static_cast<std::int64_t>(_engine->viewRowCount());
    std::vector<table::RowId> keys;
    for (std::int64_t at = std::max<std::int64_t>(first, 0); at < std::min(rows, first + count); ++at) {
        keys.push_back(_engine->rowIdAt(static_cast<std::size_t>(at)));
    }
    return fetchRows(std::move(keys), std::vector<std::string>(columns.begin(), columns.end()));
}

void EngineRows::locateRow(table::RowId key, ::core::async::StopToken stop,
                           std::function<void(RowLocation const&)> done) {
    std::optional<std::size_t> const viewRow = _engine->viewRowOfKey(key);
    RowLocation const found{viewRow ? std::optional{static_cast<std::int64_t>(*viewRow)} : std::nullopt};
    _owner->post([stop = std::move(stop), done = std::move(done), found] {
        if (!stop.stop_requested()) {
            done(found);
        }
    });
}

async::Completion<std::expected<table::DistinctValues, table::TableError>> EngineRows::distinctValues(
    table::DistinctRequest request) {
    using Result = std::expected<table::DistinctValues, table::TableError>;
    auto [completion, promise] = async::Completion<Result>::makeSettleable(_owner);
    auto const settle = std::make_shared<async::Completion<Result>::Promise>(std::move(promise));
    _owner->post(_scope.token().guard(
        [this, settle, request = std::move(request)] { settle->resolve(_engine->distinctValues(request)); }));
    return std::move(completion);
}

TableCountsView EngineRows::counts() const {
    std::int64_t retained = 0;
    for (table::RowId const& key : _engine->retained()) {
        if (auto const viewRow = _engine->viewRowOfKey(key); viewRow && _engine->isUnmatched(*viewRow)) {
            ++retained;
        }
    }
    return TableCountsView{.view = total(),
                           .source = static_cast<std::int64_t>(_engine->sourceRowCount()),
                           .undecided = static_cast<std::int64_t>(_engine->undecidedRowCount()),
                           .pending = _engine->pending(),
                           .awaiting = {},
                           .failed = {},
                           .retained = retained,
                           .busy = _states.busyCount()};
}

RetainResult EngineRows::retain(std::span<table::RowId const> keys) {
    auto const held = _engine->retained();
    std::vector<table::RowId> next(held.begin(), held.end());
    std::unordered_set<table::RowId> seen(held.begin(), held.end());
    for (table::RowId const& key : keys) {
        if (seen.insert(key).second) {
            next.push_back(key);
        }
    }
    if (next.size() > table::QueryLimits{}.maxRetained) {
        return RetainResult::OverBound;
    }
    if (next.size() != held.size()) {
        std::ignore = _engine->retain(std::move(next));
    }
    return RetainResult::Retained;
}

void EngineRows::clearRetained() {
    if (!_engine->retained().empty()) {
        std::ignore = _engine->retain({});
    }
}

bool EngineRows::unmatched(table::RowId const& key) const {
    auto const viewRow = _engine->viewRowOfKey(key);
    return viewRow && _engine->isUnmatched(*viewRow);
}

void EngineRows::moveKey(table::RowId const& from, table::RowId const& target) {
    _states.move(from, target);
    auto const held = _engine->retained();
    if (!std::ranges::contains(held, from)) {
        return;
    }
    std::vector<table::RowId> next;
    next.reserve(held.size());
    bool const toHeld = std::ranges::contains(held, target);
    for (table::RowId const& key : held) {
        if (key == from) {
            if (!toHeld) {
                next.push_back(target);
            }
        } else {
            next.push_back(key);
        }
    }
    std::ignore = _engine->retain(std::move(next));
}

std::shared_ptr<table::DataSource> EngineRows::checked(std::shared_ptr<table::DataSource> source,
                                                       table::EngineOptions const& options) {
    if (source == nullptr || options.owner == nullptr) {
        throw std::invalid_argument{"morph::ui::EngineRows: the source and the engine's owner must not be null"};
    }
    return source;
}

void EngineRows::rowsChanged(table::RowChange const& change) {
    _log.push_back(Logged{
        .snapshot = _source->snapshot(), .whole = change.kind != table::ChangeKind::Updated, .rows = change.rows});
}

void EngineRows::sync() const {
    auto const& current = _engine->viewSnapshot();
    if (current == _memoSnapshot) {
        return;
    }
    auto const made = std::ranges::find(_log, current, &Logged::snapshot);
    auto const end = made == _log.end() ? _log.end() : std::next(made);
    for (auto at = _log.begin(); at != end; ++at) {
        if (at->whole) {
            _memo.clear();
        } else {
            for (auto const row : at->rows) {
                _memo.erase(row);
            }
        }
    }
    _log.erase(_log.begin(), end);
    _memoSnapshot = current;
}

EngineRows::Entry const* EngineRows::entryAt(std::int64_t index) const {
    if (index < 0) {
        return nullptr;
    }
    auto const sourceRow = _engine->sourceRowOf(static_cast<std::size_t>(index));
    if (!sourceRow) {
        return nullptr;
    }
    sync();
    auto const found = _memo.find(*sourceRow);
    if (found != _memo.end()) {
        return &found->second;
    }
    auto const& snapshot = *_engine->viewSnapshot();
    Entry entry{.record = _recordOf(snapshot, *sourceRow), .levels = {}};
    ++_built;
    auto const columns = _engine->columns();
    for (std::size_t column = 0; column < columns.size(); ++column) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): column < columns.size() by the loop
        if (!columns[column].level.empty() &&
            std::holds_alternative<table::Unloaded>(snapshot.cell(*sourceRow, column))) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): column < columns.size() by the loop
            entry.levels.push_back(columns[column].level);
        }
    }
    std::ranges::sort(entry.levels);
    auto const [first, last] = std::ranges::unique(entry.levels);
    entry.levels.erase(first, last);
    return &_memo.insert_or_assign(*sourceRow, std::move(entry)).first->second;
}

std::string ValueRowsCollator::sortKey(std::string_view text) const {
    std::string key = folded(text);
    key.push_back('\0');
    key.append(text);
    return key;
}

MORPH_PROCESS_UNIQUE std::string ValueRowsCollator::folded(std::string_view text) const {
    struct Last {
        std::string text;
        std::string fold;
        bool held = false;
    };
    thread_local Last last;
    if (!last.held || last.text != text) {
        _folds.fetch_add(1, std::memory_order_relaxed);
        last.fold = table::CodePointCollator{}.fold(text);
        last.text.assign(text);
        last.held = true;
    }
    return last.fold;
}

}  // namespace morph::ui

namespace morph::ui::detail {

std::uint32_t exactPlacesOf(std::int64_t den) {
    std::int64_t power = 1;
    for (std::uint32_t places = 0; places < math::kMaxDecimalPlaces; ++places) {
        if (power % den == 0) {
            return places;
        }
        if (power > std::numeric_limits<std::int64_t>::max() / 10) {
            break;
        }
        power *= 10;
    }
    return math::kMaxDecimalPlaces;
}

std::optional<MeasureParts> measureOf(Value const& value) {
    if (auto const* const quantity = value.asQuantity()) {
        return MeasureParts{.amount = quantity->value, .unit = &quantity->unit};
    }
    auto const* const record = value.asRecord();
    constexpr std::size_t kFields = 3;
    if (record == nullptr || record->size() != kFields) {
        return std::nullopt;
    }
    auto const* const num = value.field("num");
    auto const* const den = value.field("den");
    auto const* const unit = value.field("unit");
    if (num == nullptr || den == nullptr || unit == nullptr || num->asInt() == nullptr || den->asInt() == nullptr ||
        unit->asString() == nullptr || *den->asInt() == 0) {
        return std::nullopt;
    }
    math::Rational const amount{math::Numerator{*num->asInt()}, math::Denominator{*den->asInt()},
                                math::DecimalPlaces{0}};
    return MeasureParts{
        .amount = math::Rational{math::Numerator{amount.numerator}, math::Denominator{amount.denominator},
                                 math::DecimalPlaces{exactPlacesOf(amount.denominator)}},
        .unit = unit->asString()};
}

ValueVote voteOf(Value const& value) {
    switch (value.kind()) {
        case ValueKind::Bool:
            return ValueVote::Bool;
        case ValueKind::Int:
            return ValueVote::Integer;
        case ValueKind::Decimal:
            return ValueVote::Decimal;
        case ValueKind::String:
            return ValueVote::Text;
        case ValueKind::Timestamp:
            return ValueVote::DateTime;
        case ValueKind::Quantity:
            return ValueVote::Measure;
        case ValueKind::Record:
            return measureOf(value) ? ValueVote::Measure : ValueVote::None;
        case ValueKind::Null:
        case ValueKind::List:
        default:
            return ValueVote::None;
    }
}

table::ColumnKind inferredKind(std::array<std::size_t, kValueVotes> const& counts, std::span<ValueVote const> votes) {
    auto const count = [&counts](ValueVote vote) { return counts.at(static_cast<std::size_t>(vote)); };
    std::size_t kinds = 0;
    ValueVote only = ValueVote::None;
    for (std::size_t at = 1; at < kValueVotes; ++at) {
        if (counts.at(at) > 0) {
            ++kinds;
            only = static_cast<ValueVote>(at);
        }
    }
    if (kinds == 0) {
        return table::ColumnKind::Text;
    }
    if (kinds == 1) {
        return kindOfVote(only);
    }
    bool const decimals = count(ValueVote::Decimal) > 0;
    if (kinds == 2 && decimals && count(ValueVote::Integer) > 0) {
        return table::ColumnKind::Decimal;
    }
    auto const first = std::ranges::find_if(votes, [](ValueVote vote) { return vote != ValueVote::None; });
    auto const kind = first == votes.end() ? table::ColumnKind::Text : kindOfVote(*first);
    return kind == table::ColumnKind::Integer && decimals ? table::ColumnKind::Decimal : kind;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one conversion per column kind, kept together.
ConvertedCell convertedCell(Value const& value, table::ColumnKind kind, table::UnitRegistry const* units) {
    using table::ColumnKind;
    if (value.isNull()) {
        return {};
    }
    auto const invalid = [kind] {
        return kind == ColumnKind::Text
                   ? ConvertedCell{.cell = table::Cell{}, .invalid = true}
                   : ConvertedCell{.cell = table::Cell{table::Measure{.amount = {}, .unit = table::kUnknownUnit}},
                                   .invalid = true};
    };
    auto const floorDiv = [](std::int64_t count, std::int64_t per) {
        std::int64_t const quotient = count / per;
        return (count % per) < 0 ? quotient - 1 : quotient;
    };
    constexpr std::int64_t kMillisPerSecond = 1000;
    constexpr std::int64_t kMillisPerDay = 86'400'000;
    switch (kind) {
        case ColumnKind::Text:
        case ColumnKind::Custom:
            if (auto const* const text = value.asString()) {
                return {.cell = table::Cell{*text}, .invalid = false};
            }
            return invalid();
        case ColumnKind::Integer:
        case ColumnKind::Key:
            if (auto const* const integer = value.asInt()) {
                return {.cell = table::Cell{*integer}, .invalid = false};
            }
            return invalid();
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
            if (auto const* const integer = value.asInt()) {
                return {.cell = table::Cell{math::Rational{*integer, math::DecimalPlaces{0}}}, .invalid = false};
            }
            if (auto const* const decimal = value.asDecimal()) {
                return {.cell = table::Cell{decimal->value}, .invalid = false};
            }
            return invalid();
        case ColumnKind::Number:
            if (auto const* const integer = value.asInt()) {
                return {.cell = table::Cell{static_cast<double>(*integer)}, .invalid = false};
            }
            if (auto const* const decimal = value.asDecimal()) {
                // One past the most places a decimal holds: the conversion is not rounded to any places.
                return {.cell = table::Cell{decimal->value.toDouble(math::kMaxDecimalPlaces + 1)}, .invalid = false};
            }
            return invalid();
        case ColumnKind::DateTime:
            if (auto const* const instant = value.asTimestamp()) {
                return {.cell = table::Cell{floorDiv(instant->value.time_since_epoch().count(), kMillisPerSecond)},
                        .invalid = false};
            }
            return invalid();
        case ColumnKind::Date:
            if (auto const* const instant = value.asTimestamp()) {
                return {.cell = table::Cell{floorDiv(instant->value.time_since_epoch().count(), kMillisPerDay)},
                        .invalid = false};
            }
            if (auto const* const text = value.asString()) {
                if (auto const days = table::IsoDateParser{}.parseDate(*text, {})) {
                    return {.cell = table::Cell{*days}, .invalid = false};
                }
            }
            return invalid();
        case ColumnKind::Bool:
            if (auto const* const flag = value.asBool()) {
                return {.cell = table::Cell{*flag}, .invalid = false};
            }
            return invalid();
        case ColumnKind::Measure:
            if (auto const parts = measureOf(value)) {
                if (auto const unit = units == nullptr ? std::nullopt : units->find(*parts->unit)) {
                    return {.cell = table::Cell{table::Measure{.amount = parts->amount, .unit = *unit}},
                            .invalid = false};
                }
            }
            return invalid();
        default:
            return invalid();
    }
}

bool sameCell(table::Cell const& left, table::Cell const& right) {
    auto const samePlaces = [](math::Rational const& lhs, math::Rational const& rhs) {
        return lhs.decimalPlaces.value == rhs.decimalPlaces.value;
    };
    if (left != right) {
        return false;
    }
    if (auto const* const exact = std::get_if<math::Rational>(&left)) {
        return samePlaces(*exact, std::get<math::Rational>(right));
    }
    if (auto const* const measure = std::get_if<table::Measure>(&left)) {
        return samePlaces(measure->amount, std::get<table::Measure>(right).amount);
    }
    return true;
}

bool sameObject(Value const& left, Value const& right) {
    if (left.kind() != right.kind()) {
        return false;
    }
    if (auto const* const record = left.asRecord()) {
        return record == right.asRecord();
    }
    if (auto const* const list = left.asList()) {
        return list == right.asList();
    }
    return left == right;
}

table::Cell ValueRowsSnapshot::cell(std::size_t row, std::size_t column) const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): column < _columns.size() by the condition above
    if (column >= _columns.size() || _columns[column] == nullptr) {
        return table::Cell{};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): column < _columns.size() by the condition above
    return _columns[column]->at(row);
}

void ValueRowsSnapshot::readColumn(std::size_t column, table::ColumnSink& sink, std::size_t first,
                                   std::size_t last) const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): column < _columns.size() by the condition above
    if (column >= _columns.size() || _columns[column] == nullptr || first >= last) {
        RowSnapshot::readColumn(column, sink, first, last);
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): column < _columns.size() by the condition above
    sink.cells(first, std::span<table::Cell const>{*_columns[column]}.subspan(first, last - first));
}

}  // namespace morph::ui::detail

namespace morph::ui {

ValueRowsSource::ValueRowsSource(reactive::Runtime& runtime, std::vector<ValueColumn> columns,
                                 std::function<Key(Value const& row)> keyOf,
                                 std::shared_ptr<table::UnitRegistry const> units)
    : _runtime{&runtime},
      _columns{std::move(columns)},
      _keyOf{std::move(keyOf)},
      _units{std::move(units)},
      _held(_columns.size()) {
    _infos.reserve(_columns.size() + 1);
    for (auto const& column : _columns) {
        _infos.push_back(table::ColumnInfo{.id = column.id,
                                           .kind = column.kind.value_or(table::ColumnKind::Text),
                                           .comparator = {},
                                           .format = {},
                                           .level = {},
                                           .dimensions = {}});
    }
}

std::shared_ptr<table::RowSnapshot const> ValueRowsSource::snapshot() const {
    if (_snapshot == nullptr) {
        std::vector<std::shared_ptr<std::vector<table::Cell> const>> cells;
        cells.reserve(_infos.size());
        for (auto const& held : _held) {
            cells.push_back(held.uses > 0 ? held.cells : nullptr);
        }
        if (_group) {
            cells.push_back(_runCells);
        }
        _snapshot = std::make_shared<detail::ValueRowsSnapshot const>(_rows, _keys, std::move(cells));
    }
    return _snapshot;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one diff whose steps share their working vectors.
void ValueRowsSource::assign(ValueList rows) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): every vector here is sized to the row count and indexed below it; `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column
    auto keys = std::make_shared<std::vector<Key>>();
    auto kept = std::make_shared<std::vector<Value>>();
    std::unordered_map<Key, std::size_t> index;
    keys->reserve(rows.size());
    kept->reserve(rows.size());
    for (auto& row : rows) {
        Key key = _keyOf(row);
        if (!index.try_emplace(key, kept->size()).second) {
            _runtime->core()->report(detail::site::kDuplicateKey);
            continue;
        }
        keys->push_back(std::move(key));
        kept->push_back(std::move(row));
    }
    std::size_t const count = kept->size();
    // Held, not referred to: the previous rows go when the new ones are taken, before the removed are counted.
    auto const previousRows = _rows;
    std::vector<Value> const& old = *previousRows;
    constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> was(count, kNone);
    std::vector<bool> same(count, false);
    std::size_t matched = 0;
    bool inOrder = true;
    std::size_t previous = 0;
    for (std::size_t at = 0; at < count; ++at) {
        auto const found = _index.find((*keys)[at]);
        if (found == _index.end()) {
            continue;
        }
        was[at] = found->second;
        same[at] = detail::sameObject(old[found->second], (*kept)[at]);
        inOrder = inOrder && (matched == 0 || found->second > previous);
        previous = found->second;
        ++matched;
    }
    bool const added = matched < count;
    bool const dropped = matched < old.size();
    bool reset = std::exchange(_pending, Pending{}).any();
    std::vector<bool> updated(count, false);
    for (std::size_t column = 0; column < _columns.size(); ++column) {
        if (_held[column].uses == 0) {
            continue;
        }
        reset = reassign(column, *kept, was, same, updated) || reset;
    }
    for (std::size_t at = 0; at < count; ++at) {
        if (!same[at] && was[at] != kNone && !updated[at] && !(old[was[at]] == (*kept)[at])) {
            updated[at] = true;
        }
    }
    bool runsMoved = false;
    if (_group) {
        std::vector<Value> groupKeys(count);
        for (std::size_t at = 0; at < count; ++at) {
            groupKeys[at] = same[at] ? _byValues[was[at]] : readBy((*kept)[at]);
        }
        auto const before = _runs;
        auto const recordsBefore = _runRecords;
        _byValues = std::move(groupKeys);
        buildRuns(*kept);
        runsMoved = _runs != before;
        if (!runsMoved) {
            for (std::size_t run = 0; run < _runRecords.size(); ++run) {
                if (!(_runRecords[run] == recordsBefore[run])) {
                    updated[_runFirst[run]] = true;
                }
            }
        }
    }
    _rows = std::move(kept);
    _keys = std::move(keys);
    _index = std::move(index);
    _snapshot.reset();
    bool const structural = added || dropped;
    if (reset || (structural && (!inOrder || (added && dropped) || runsMoved)) || (!structural && runsMoved)) {
        notify(table::RowChange{.kind = table::ChangeKind::Reset, .rows = {}});
        return;
    }
    if (dropped) {
        std::vector<bool> stays(old.size(), false);
        for (auto const heldIndex : was) {
            if (heldIndex != kNone) {
                stays[heldIndex] = true;
            }
        }
        std::vector<std::size_t> removed;
        for (std::size_t at = 0; at < old.size(); ++at) {
            if (!stays[at]) {
                removed.push_back(at);
            }
        }
        notify(table::RowChange{.kind = table::ChangeKind::Removed, .rows = std::move(removed)});
    }
    if (added) {
        std::vector<std::size_t> inserted;
        for (std::size_t at = 0; at < count; ++at) {
            if (was[at] == kNone) {
                inserted.push_back(at);
            }
        }
        notify(table::RowChange{.kind = table::ChangeKind::Inserted, .rows = std::move(inserted)});
    }
    std::vector<std::size_t> changed;
    for (std::size_t at = 0; at < count; ++at) {
        if (updated[at]) {
            changed.push_back(at);
        }
    }
    if (!changed.empty()) {
        notify(table::RowChange{.kind = table::ChangeKind::Updated, .rows = std::move(changed)});
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

void ValueRowsSource::setGroup(std::function<Value(Value const& row)> groupBy, UnitTable units) {
    _group = std::move(groupBy);
    _groupUnits = std::move(units);
    bool const listed = _infos.size() > _columns.size();
    if (_group && !listed) {
        _infos.push_back(table::ColumnInfo{.id = std::string{kGroupRunColumn},
                                           .kind = table::ColumnKind::Integer,
                                           .comparator = {},
                                           .format = {},
                                           .level = {},
                                           .dimensions = {}});
    } else if (!_group && listed) {
        _infos.pop_back();
    }
    _byValues.clear();
    if (_group) {
        _byValues.reserve(_rows->size());
        for (auto const& row : *_rows) {
            _byValues.push_back(readBy(row));
        }
    }
    buildRuns(*_rows);
    _snapshot.reset();
    notify(table::RowChange{.kind = table::ChangeKind::Reset, .rows = {}});
}

std::size_t ValueRowsSource::runOf(std::size_t sourceRow) const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): sourceRow < _runs.size() by the condition
    return sourceRow < _runs.size() ? _runs[sourceRow] : 0;
}

void ValueRowsSource::use(std::string_view column) {
    auto const columnIndex = columnOf(column);
    if (!columnIndex) {
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column (columnOf returns an index into `_columns`)
    if (_held[*columnIndex].uses++ > 0) {
        return;
    }
    rebuild(*columnIndex);
    _snapshot.reset();
    notify(table::RowChange{.kind = table::ChangeKind::Reset, .rows = {}});
}

void ValueRowsSource::unuse(std::string_view column) {
    auto const columnIndex = columnOf(column);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column (columnOf returns an index into `_columns`)
    if (!columnIndex || _held[*columnIndex].uses == 0) {
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column (columnOf returns an index into `_columns`)
    if (--_held[*columnIndex].uses > 0) {
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column (columnOf returns an index into `_columns`)
    _held[*columnIndex] = Held{};
    _snapshot.reset();
}

RecordOf ValueRowsSource::recordOf() {
    return [](table::RowSnapshot const& snapshot, std::size_t sourceRow) {
        auto const* const rows = dynamic_cast<detail::ValueRowsSnapshot const*>(&snapshot);
        return RowRecord{.key = snapshot.rowId(sourceRow), .row = rows == nullptr ? Value{} : rows->row(sourceRow)};
    };
}

std::size_t ValueRowsSource::cellsHeld() const noexcept {
    std::size_t cells = 0;
    for (auto const& held : _held) {
        cells += held.cells == nullptr ? 0 : held.cells->size();
    }
    return cells + (_runCells == nullptr ? 0 : _runCells->size());
}

std::size_t ValueRowsSource::invalidCells(std::string_view column) const {
    auto const columnIndex = columnOf(column);
    if (!columnIndex) {
        return 0;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column (columnOf returns an index into `_columns`)
    return static_cast<std::size_t>(std::ranges::count(_held[*columnIndex].invalid, std::uint8_t{1}));
}

std::optional<std::size_t> ValueRowsSource::columnOf(std::string_view columnId) const {
    auto const found = std::ranges::find(_columns, columnId, &ValueColumn::id);
    if (found == _columns.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - _columns.begin());
}

Value ValueRowsSource::fieldOf(std::size_t column, Value const& row) const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column
    auto const* const field = row.field(_columns[column].id);
    return field == nullptr ? Value{} : *field;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): column index, then row index, in every call here.
Value ValueRowsSource::valueOf(std::size_t column, std::size_t row, Value const& input) const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column; row < trackers.size() by the condition
    auto const& trackers = _held[column].trackers;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column; row < trackers.size() by the condition
    if (row < trackers.size() && trackers[row] != nullptr) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column; row < trackers.size() by the condition
        return trackers[row]->value;
    }
    return fieldOf(column, input);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): column index, then row index, in every call here.
std::unique_ptr<ValueRowsSource::Tracker> ValueRowsSource::track(std::size_t column, std::size_t row,
                                                                 Value const& input) {
    auto tracker = std::make_unique<Tracker>();
    tracker->column = column;
    tracker->row = row;
    tracker->input = input;
    Tracker* const self = tracker.get();
    tracker->effect = std::make_unique<reactive::Effect>(*_runtime, [this, self] { rerun(*self); });
    self->live = true;
    return tracker;
}

void ValueRowsSource::rerun(Tracker& tracker) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column; `row` is below the row count every per-row vector here is sized to
    Value value;
    try {
        value = _columns[tracker.column].sortBy(tracker.input);
    } catch (...) {
        value = Value{};
    }
    ++_derived;
    tracker.value = std::move(value);
    if (!tracker.live) {
        return;
    }
    auto& held = _held[tracker.column];
    std::size_t const row = tracker.row;
    auto const kind = vote(held, tracker.column, row, detail::voteOf(tracker.value));
    _pending.rerun.insert(row);
    if (kind != _infos[tracker.column].kind) {
        _infos[tracker.column].kind = kind;
        reconvert(tracker.column);
        _pending.reset = true;
    } else {
        auto converted = detail::convertedCell(tracker.value, kind, _units.get());
        held.invalid[row] = converted.invalid ? 1 : 0;
        if (!detail::sameCell(converted.cell, (*held.cells)[row])) {
            _snapshot.reset();
            writable(held)[row] = std::move(converted.cell);
            _pending.rows.push_back(row);
        }
    }
    if (!_posted) {
        _posted = true;
        _runtime->owner().post(_alive.guard([this] { flushTracked(); }));
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

void ValueRowsSource::flushTracked() {
    _posted = false;
    auto pending = std::exchange(_pending, Pending{});
    if (!pending.any()) {
        return;
    }
    if (pending.reset || pending.rerun.size() == _rows->size()) {
        notify(table::RowChange{.kind = table::ChangeKind::Reset, .rows = {}});
        return;
    }
    std::ranges::sort(pending.rows);
    auto const [first, last] = std::ranges::unique(pending.rows);
    pending.rows.erase(first, last);
    notify(table::RowChange{.kind = table::ChangeKind::Updated, .rows = std::move(pending.rows)});
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): column index, then row index, in every call here.
table::ColumnKind ValueRowsSource::vote(Held& held, std::size_t column, std::size_t row, detail::ValueVote next) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `row` is below the row count every per-row vector here is sized to
    auto& current = held.votes[row];
    --held.counts.at(static_cast<std::size_t>(current));
    ++held.counts.at(static_cast<std::size_t>(next));
    current = next;
    return kindOf(column, held);
}

table::ColumnKind ValueRowsSource::kindOf(std::size_t column, Held const& held) const {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column
    if (auto const& declared = _columns[column].kind) {
        return *declared;
    }
    return detail::inferredKind(held.counts, held.votes);
}

std::vector<table::Cell>& ValueRowsSource::writable(Held& held) {
    if (held.cells.use_count() > 1) {
        held.cells = std::make_shared<std::vector<table::Cell>>(*held.cells);
    }
    return *held.cells;
}

void ValueRowsSource::reconvert(std::size_t column) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column; `row` is below the row count every per-row vector here is sized to
    auto& held = _held[column];
    auto const kind = _infos[column].kind;
    auto cells = std::make_shared<std::vector<table::Cell>>(_rows->size());
    for (std::size_t row = 0; row < _rows->size(); ++row) {
        auto converted = detail::convertedCell(valueOf(column, row, (*_rows)[row]), kind, _units.get());
        (*cells)[row] = std::move(converted.cell);
        held.invalid[row] = converted.invalid ? 1 : 0;
    }
    held.cells = std::move(cells);
    _snapshot.reset();
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

void ValueRowsSource::rebuild(std::size_t column) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column; `row` is below the row count every per-row vector here is sized to
    auto& held = _held[column];
    std::size_t const count = _rows->size();
    held.votes.assign(count, detail::ValueVote::None);
    held.invalid.assign(count, 0);
    held.counts = {};
    held.counts[0] = count;
    held.trackers.clear();
    if (_columns[column].sortBy) {
        held.trackers.resize(count);
    }
    for (std::size_t row = 0; row < count; ++row) {
        Value const& input = (*_rows)[row];
        if (_columns[column].sortBy) {
            held.trackers[row] = track(column, row, input);
            static_cast<void>(vote(held, column, row, detail::voteOf(held.trackers[row]->value)));
        } else {
            ++_derived;
            static_cast<void>(vote(held, column, row, detail::voteOf(fieldOf(column, input))));
        }
    }
    _infos[column].kind = kindOf(column, held);
    reconvert(column);
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

bool ValueRowsSource::reassign(std::size_t column, std::vector<Value> const& rows, std::vector<std::size_t> const& was,
                               std::vector<bool> const& same, std::vector<bool>& updated) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `column` indexes `_columns`, and `_held` and `_infos` hold one entry per column; `row` is below the row count every per-row vector here is sized to
    auto& held = _held[column];
    bool const tracked = static_cast<bool>(_columns[column].sortBy);
    std::size_t const count = rows.size();
    Held next;
    next.uses = held.uses;
    next.votes.assign(count, detail::ValueVote::None);
    next.invalid.assign(count, 0);
    if (tracked) {
        next.trackers.resize(count);
    }
    auto cells = std::make_shared<std::vector<table::Cell>>(count);
    std::vector<Value> fresh(count);
    for (std::size_t row = 0; row < count; ++row) {
        if (same[row]) {
            next.votes[row] = held.votes[was[row]];
            next.invalid[row] = held.invalid[was[row]];
            (*cells)[row] = (*held.cells)[was[row]];
            if (tracked) {
                next.trackers[row] = std::move(held.trackers[was[row]]);
                next.trackers[row]->row = row;
            }
        } else if (tracked) {
            next.trackers[row] = track(column, row, rows[row]);
            fresh[row] = next.trackers[row]->value;
            next.votes[row] = detail::voteOf(fresh[row]);
        } else {
            ++_derived;
            fresh[row] = fieldOf(column, rows[row]);
            next.votes[row] = detail::voteOf(fresh[row]);
        }
        ++next.counts.at(static_cast<std::size_t>(next.votes[row]));
    }
    auto const kind = kindOf(column, next);
    bool const changedKind = kind != _infos[column].kind;
    for (std::size_t row = 0; row < count; ++row) {
        if (same[row] && !changedKind) {
            continue;
        }
        Value value;
        if (same[row]) {
            value = tracked ? next.trackers[row]->value : fieldOf(column, rows[row]);
        } else {
            value = std::move(fresh[row]);
        }
        auto converted = detail::convertedCell(value, kind, _units.get());
        next.invalid[row] = converted.invalid ? 1 : 0;
        if (!changedKind && was[row] != std::numeric_limits<std::size_t>::max() &&
            !detail::sameCell(converted.cell, (*held.cells)[was[row]])) {
            updated[row] = true;
        }
        (*cells)[row] = std::move(converted.cell);
    }
    next.cells = std::move(cells);
    _infos[column].kind = kind;
    held = std::move(next);
    return changedKind;
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

void ValueRowsSource::buildRuns(std::vector<Value> const& rows) {
    _runs.clear();
    _runFirst.clear();
    _runRecords.clear();
    if (!_group) {
        _runCells.reset();
        return;
    }
    auto cells = std::make_shared<std::vector<table::Cell>>();
    cells->reserve(rows.size());
    _runs.reserve(rows.size());
    // A heading's key is its run's key and its ordinal among the runs with an equal key, as the interpreter
    // keys it: adjacent grouping can give two runs one key.
    std::map<std::string, std::size_t, std::less<>> ordinals;
    ValueList run;
    std::size_t first = 0;
    auto const close = [&] {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): first and row are below the row count _byValues is sized to
        Value const& key = _byValues[first];
        std::string const json = ui::detail::canonicalJson(key);
        Value const record = runRecord(key, std::exchange(run, {}));
        ValueRecord entry = *record.asRecord();
        entry.insert_or_assign(std::string{detail::kGroupHeadingField},
                               Value{"\x1Fgroup:" + json + ":" + std::to_string(ordinals[json]++)});
        _runRecords.emplace_back(std::move(entry));
        _runFirst.push_back(first);
    };
    for (std::size_t row = 0; row < rows.size(); ++row) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): first and row are below the row count _byValues is sized to
        if (!run.empty() && !detail::sameElement(_byValues[first], _byValues[row], _groupUnits)) {
            close();
        }
        if (run.empty()) {
            first = row;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): first and row are below the row count _byValues is sized to
        run.push_back(rows[row]);
        _runs.push_back(_runRecords.size());
        cells->emplace_back(static_cast<std::int64_t>(_runRecords.size()));
    }
    if (!run.empty()) {
        close();
    }
    _runCells = std::move(cells);
}

}  // namespace morph::ui

namespace morph::ui::detail {

std::optional<std::int64_t> jsonInteger(glz::generic_u64 const& member) {
    if (auto const* const value = member.get_if<std::int64_t>()) {
        return *value;
    }
    if (auto const* const value = member.get_if<std::uint64_t>();
        value != nullptr && *value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(*value);
    }
    return std::nullopt;
}

std::optional<JsonDecimal> jsonDecimal(glz::generic_u64 const& member) {
    auto const* const object = member.get_if<glz::generic_u64::object_t>();
    if (object == nullptr || (object->size() != 3 && object->size() != 4)) {
        return std::nullopt;
    }
    auto const integer = [object](std::string_view name) -> std::optional<std::int64_t> {
        auto const* const found = object->find(name);
        return found == object->end() ? std::nullopt : jsonInteger(found->second);
    };
    auto const num = integer("num");
    auto const den = integer("den");
    auto const places = integer("dp");
    if (!num || !den || !places || *den == 0 || *places < 0 || std::cmp_greater(*places, math::kMaxDecimalPlaces)) {
        return std::nullopt;
    }
    std::string const* unit = nullptr;
    if (object->size() == 4) {
        auto const* const found = object->find("unit");
        unit = found == object->end() ? nullptr : found->second.get_if<std::string>();
        if (unit == nullptr) {
            return std::nullopt;
        }
    }
    auto const amount = math::Rational::from(math::Numerator{*num}, math::Denominator{*den},
                                             math::DecimalPlaces{static_cast<std::uint32_t>(*places)});
    if (!amount) {
        return std::nullopt;
    }
    return JsonDecimal{.amount = *amount, .unit = unit};
}

}  // namespace morph::ui::detail
