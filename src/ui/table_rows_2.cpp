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

namespace morph::ui::detail {

JsonVote jsonVoteOf(glz::generic_u64 const& member) {
    if (member.is_boolean()) {
        return JsonVote::Bool;
    }
    if (member.is_string()) {
        return JsonVote::Text;
    }
    if (member.is_number()) {
        return jsonInteger(member) ? JsonVote::Integer : JsonVote::Number;
    }
    if (auto const decimal = jsonDecimal(member)) {
        return decimal->unit != nullptr ? JsonVote::Measure : JsonVote::Decimal;
    }
    return JsonVote::None;
}

table::ColumnKind inferredJsonKind(std::span<glz::generic_u64 const> rows, std::string_view column) {
    std::array<std::size_t, kJsonVotes> counts{};
    JsonVote first = JsonVote::None;
    for (auto const& row : rows) {
        auto const* const object = row.get_if<glz::generic_u64::object_t>();
        if (object == nullptr) {
            continue;
        }
        auto const* const found = object->find(column);
        if (found == object->end()) {
            continue;
        }
        JsonVote const vote = jsonVoteOf(found->second);
        if (vote == JsonVote::None) {
            continue;
        }
        ++counts.at(static_cast<std::size_t>(vote));
        if (first == JsonVote::None) {
            first = vote;
        }
    }
    auto const count = [&counts](JsonVote vote) { return counts.at(static_cast<std::size_t>(vote)); };
    std::size_t kinds = 0;
    for (std::size_t at = 1; at < kJsonVotes; ++at) {
        kinds += counts.at(at) > 0 ? 1U : 0U;
    }
    if (kinds == 0) {
        return table::ColumnKind::Text;
    }
    if (kinds == 1) {
        return kindOfJsonVote(first);
    }
    bool const integers = count(JsonVote::Integer) > 0;
    bool const numbers = count(JsonVote::Number) > 0;
    bool const decimals = count(JsonVote::Decimal) > 0;
    std::size_t const numeric = (integers ? 1U : 0U) + (numbers ? 1U : 0U) + (decimals ? 1U : 0U);
    if (numeric == kinds && integers && !numbers) {
        return table::ColumnKind::Decimal;
    }
    if (numeric == kinds && integers && numbers) {
        return table::ColumnKind::Number;
    }
    return kindOfJsonVote(first);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per column kind.
table::Cell typedCell(glz::generic_u64 const& member, table::ColumnKind kind, table::UnitRegistry const* units,
                      std::map<table::UnitId, std::string>& named) {
    using table::ColumnKind;
    if (member.is_null()) {
        return table::Cell{};
    }
    if (auto const* const flag = member.get_if<bool>()) {
        return table::Cell{*flag};
    }
    if (auto const integer = jsonInteger(member)) {
        if (kind == ColumnKind::Decimal) {
            return table::Cell{math::Rational{*integer, math::DecimalPlaces{0}}};
        }
        if (kind == ColumnKind::Number) {
            return table::Cell{static_cast<double>(*integer)};
        }
        return table::Cell{*integer};
    }
    if (auto const* const real = member.get_if<double>()) {
        return table::Cell{*real};
    }
    if (auto const* const large = member.get_if<std::uint64_t>()) {
        return table::Cell{static_cast<double>(*large)};
    }
    if (auto const* const text = member.get_if<std::string>()) {
        table::IsoDateParser const parser;
        if (kind == ColumnKind::Date) {
            if (auto const days = parser.parseDate(*text, {})) {
                return table::Cell{*days};
            }
        } else if (kind == ColumnKind::DateTime) {
            if (auto const seconds = parser.parseDateTime(*text, {})) {
                return table::Cell{*seconds};
            }
        }
        return table::Cell{*text};
    }
    if (auto const decimal = jsonDecimal(member)) {
        if (decimal->unit == nullptr) {
            return table::Cell{decimal->amount};
        }
        if (kind == ColumnKind::Measure) {
            auto const unit = units == nullptr ? std::nullopt : units->find(*decimal->unit);
            if (unit) {
                named.try_emplace(*unit, *decimal->unit);
            }
            return table::Cell{table::Measure{.amount = decimal->amount, .unit = unit.value_or(table::kUnknownUnit)}};
        }
    }
    return table::Cell{glz::write_json(member).value_or(std::string{})};
}

std::optional<table::RowId> typedKey(glz::generic_u64 const& member) {
    if (auto const integer = jsonInteger(member)) {
        return table::RowId{*integer};
    }
    if (auto const* const text = member.get_if<std::string>()) {
        return table::RowId{*text};
    }
    return std::nullopt;
}

std::expected<table::GenericPage, table::TableError> typedPageOf(TypedPageDom const& dom,
                                                                 table::TableDescription const& description,
                                                                 table::UnitRegistry const* units) {
    auto const invalid = [](std::string message) {
        return std::unexpected(table::TableError{
            .code = table::TableErrorCode::InvalidValue, .column = {}, .message = std::move(message)});
    };
    std::shared_ptr<table::UnitRegistry const> built;
    bool const measures = std::ranges::any_of(description.columns, [](table::TableColumn const& column) {
        return column.info.kind == table::ColumnKind::Measure;
    });
    if (measures && units == nullptr) {
        auto registry = table::registryOf(description);
        if (!registry) {
            return std::unexpected(std::move(registry.error()));
        }
        built = std::move(*registry);
        units = built.get();
    }
    auto const rowCount = static_cast<std::int64_t>(dom.rows.size());
    std::int64_t previous = -1;
    for (auto const& unloaded : dom.unloadedRows) {
        if (unloaded.index <= previous || unloaded.index >= rowCount) {
            return invalid("an unloaded row at " + std::to_string(unloaded.index) + " in a page of " +
                           std::to_string(rowCount) + " rows, or out of order");
        }
        previous = unloaded.index;
    }
    if (!dom.indices.empty() && dom.indices.size() != dom.rows.size()) {
        return invalid("a page of " + std::to_string(dom.indices.size()) + " indices and " +
                       std::to_string(dom.rows.size()) + " rows");
    }
    if (std::ranges::any_of(dom.indices, [](std::int64_t index) { return index < 0; })) {
        return invalid("a negative view row in indices");
    }
    table::GenericPage page;
    page.columns.reserve(description.columns.size());
    for (auto const& column : description.columns) {
        page.columns.push_back(column.info.id);
    }
    page.keys.reserve(dom.rows.size());
    page.rows.reserve(dom.rows.size());
    glz::generic_u64 const absent{};
    for (std::size_t at = 0; at < dom.rows.size(); ++at) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < dom.rows.size() by the loop
        auto const* const object = dom.rows[at].get_if<glz::generic_u64::object_t>();
        if (object == nullptr) {
            return invalid("row " + std::to_string(at) + " is not an object");
        }
        auto const* const keyMember = object->find(description.key);
        auto key = keyMember == object->end() ? std::nullopt : typedKey(keyMember->second);
        if (!key) {
            return invalid("row " + std::to_string(at) + " has no key `" + description.key + "`");
        }
        page.keys.push_back(std::move(*key));
        table::GenericRow cells;
        cells.reserve(description.columns.size());
        for (auto const& column : description.columns) {
            auto const* const found = object->find(column.info.id);
            cells.push_back(
                typedCell(found == object->end() ? absent : found->second, column.info.kind, units, page.units));
        }
        page.rows.push_back(std::move(cells));
    }
    page.total = dom.total;
    page.totalKind = dom.totalKind;
    page.offset = dom.offset;
    page.unloadedRows = dom.unloadedRows;
    page.undecided = dom.undecided;
    page.awaiting = dom.awaiting;
    page.failed = dom.failed;
    page.indices = dom.indices;
    page.distinct = dom.distinct;
    return page;
}

std::vector<table::UnitInfo> unlistedUnits(std::span<glz::generic_u64 const> rows,
                                           table::TableDescription const& description,
                                           table::UnitRegistry const* units, std::size_t room) {
    std::vector<table::UnitInfo> out;
    for (auto const& column : description.columns) {
        if (column.info.kind != table::ColumnKind::Measure) {
            continue;
        }
        for (auto const& row : rows) {
            auto const* const object = row.get_if<glz::generic_u64::object_t>();
            auto const found = object == nullptr ? std::optional<glz::generic_u64::object_t::const_iterator>{}
                                                 : std::optional{object->find(column.info.id)};
            if (!found || *found == object->end()) {
                continue;
            }
            auto const decimal = jsonDecimal((*found)->second);
            if (out.size() >= room || !decimal || decimal->unit == nullptr || decimal->unit->empty() ||
                (units != nullptr && units->find(*decimal->unit)) ||
                std::ranges::contains(out, *decimal->unit, &table::UnitInfo::id)) {
                continue;
            }
            out.push_back(table::UnitInfo{.id = *decimal->unit,
                                          .symbol = *decimal->unit,
                                          .dimension = *decimal->unit,
                                          .toBase = math::Rational{1, math::DecimalPlaces{0}},
                                          .decimals = decimal->amount.decimalPlaces.value});
        }
    }
    return out;
}

std::expected<TypedPageDom, table::TableError> typedPageDom(std::string_view json) {
    TypedPageDom dom;
    if (auto const failed =
            glz::read<glz::opts{.null_terminated = false, .error_on_unknown_keys = false}>(dom, json)) {
        return std::unexpected(table::TableError{.code = table::TableErrorCode::InvalidValue,
                                                 .column = {},
                                                 .message = "not a page: " + glz::format_error(failed, json)});
    }
    return dom;
}

}  // namespace morph::ui::detail

namespace morph::ui {

std::expected<table::GenericPage, table::TableError> readTypedPage(std::string_view json,
                                                                   table::TableDescription const& description,
                                                                   table::UnitRegistry const* units) {
    auto dom = detail::typedPageDom(json);
    if (!dom) {
        return std::unexpected(std::move(dom.error()));
    }
    return detail::typedPageOf(*dom, description, units);
}

WindowRows::WindowRows(Session& session, WindowRowsOptions options)
    : _session{&session},
      _options{std::move(options)},
      _window{_options.pageSize, _options.maxPages, _options.maxKeys, _options.maxColumns},
      _restoring{_options.restoring},
      _watched{_options.records.empty()},
      _unanswered{_options.records.size()},
      _inferring{!_options.action.empty() && !_options.inferKinds.empty()} {
    // A record watch hears a reconnect as a change of every row; a window that watches none hears it here.
    if (_options.records.empty()) {
        _reconnect = _session->onReconnected(_scope.token().guard([this] {
            forgetDistinct();
            _window.invalidate();
            pump();
        }));
    }
    for (auto const& record : _options.records) {
        auto watching = _session->watchRecord(
            record, _scope.token().guard([this](effects::RecordChange const& change) { heard(change); }));
        watching.then(_scope, [this](WatchHolder const& holder) {
            _watches.push_back(holder);
            answered();
        });
        watching.onError(_scope, [this](std::exception_ptr const&) { answered(); });
    }
}

// NOLINTNEXTLINE(bugprone-exception-escape): only allocation can throw, and that ends the process either way.
WindowRows::~WindowRows() {
    stopAll();
    for (auto& [flightId, stop] : _distinctFlights) {
        static_cast<void>(flightId);
        stop.request_stop();
    }
    for (auto const& held : _keyRequests) {
        if (auto const request = held.lock()) {
            for (auto& stop : request->stops) {
                stop.request_stop();
            }
            // Answered now, while the owner it delivers on is alive: the request may outlive it in a reply's
            // handler.
            if (request->waiting > 0) {
                request->waiting = 0;
                request->promise.resolve(
                    std::expected<std::vector<table::RowId>, table::TableError>{std::unexpected(table::TableError{
                        .code = table::TableErrorCode::Unavailable, .column = {}, .message = "the rows are gone"})});
            }
        }
    }
    for (auto const& held : _rowsRequests) {
        if (auto const request = held.lock()) {
            for (auto& stop : request->stops) {
                stop.request_stop();
            }
            if (request->waiting > 0) {
                request->waiting = 0;
                request->promise.resolve(
                    std::expected<std::vector<FetchedRow>, table::TableError>{std::unexpected(table::TableError{
                        .code = table::TableErrorCode::Unavailable, .column = {}, .message = "the rows are gone"})});
            }
        }
    }
    _locateStop.request_stop();
}

void WindowRows::setDescription(table::TableDescription description) {
    if (_descriptionError) {
        _error.reset();
    }
    _descriptionError = false;
    bool const another = !_description || _description->revision != description.revision;
    if (another) {
        forgetDistinct();
    }
    bool const replacing = _description.has_value() && another;
    table::FitReport report;
    auto fitted = table::fitState(table::StateDocument{.version = table::kStateVersion,
                                                       .layout = {},
                                                       .sort = _sort,
                                                       .filters = _filter,
                                                       .view = {},
                                                       .carried = {}},
                                  description, report);
    _sort = fitted.sort.value_or(SortChain{});
    _filter = fitted.filters.value_or(table::FilterSpec{});
    _description = std::move(description);
    // One registry for the description: a page's measure cells and the records made of them name units by its
    // numbers. A description whose units do not build leaves it null, and its pages are refused as they read.
    auto registry = table::registryOf(*_description);
    _units = registry ? std::move(*registry) : nullptr;
    _infos.clear();
    for (auto const& column : _description->columns) {
        _infos.insert_or_assign(column.info.id, column.info);
    }
    std::erase_if(_columns, [this](std::string const& columnId) { return !_infos.contains(columnId); });
    ++_columnsStamp;
    if (another) {
        _error.reset();
    }
    if (replacing) {
        restart();
    } else {
        pump();
    }
}

void WindowRows::setDescription(Value const& described) {
    table::TableDescription description;
    auto const text = ui::detail::canonicalJson(described);
    if (auto const failed = glz::read<glz::opts{.error_on_unknown_keys = false}>(description, text)) {
        _descriptionError = true;
        _error =
            table::TableError{.code = table::TableErrorCode::InvalidValue,
                              .column = {},
                              .message = "the table's description does not read: " + glz::format_error(failed, text)};
        status();
        return;
    }
    setDescription(std::move(description));
}

void WindowRows::release() {
    if (!_restoring) {
        return;
    }
    _restoring = false;
    pump();
}

void WindowRows::retry() {
    _error.reset();
    _retry = true;
    _window.invalidate();
    pump();
}

TableTotal WindowRows::total() const {
    auto const rows = _window.total();
    return TableTotal{.rows = rows.value_or(0), .kind = _window.totalKind(), .known = rows.has_value()};
}

std::optional<TableRowRef> WindowRows::row(std::int64_t index) const {
    auto const* const key = _window.key(index);
    if (key == nullptr) {
        return std::nullopt;
    }
    auto [found, made] = _memo.try_emplace(*key, Entry{.key = *key, .record = {}, .stamp = 0});
    Entry& entry = found->second;
    if (made || entry.stamp != _columnsStamp) {
        ValueRecord fields;
        auto const convert = [&](std::string const& columnId) {
            auto const* const cell = _window.cell(index, columnId);
            auto const info = _infos.find(columnId);
            if (cell == nullptr || info == _infos.end()) {
                return;
            }
            fields.insert_or_assign(columnId, cellValue(*cell, info->second, _units.get()));
            ++_stats.cellsConverted;
        };
        if (_columns.empty() && _description) {
            for (auto const& column : _description->columns) {
                convert(column.info.id);
            }
        } else {
            for (auto const& columnId : _columns) {
                convert(columnId);
            }
        }
        entry.record = Value{std::move(fields)};
        entry.stamp = _columnsStamp;
    }
    return TableRowRef{.key = entry.key, .row = entry.record};
}

std::vector<std::pair<std::int64_t, std::int64_t>> WindowRows::heldRanges() const {
    auto const size = std::max<std::int64_t>(_options.pageSize, 1);
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    for (std::int64_t const page : _heldPages) {
        if (heldPage(page)) {
            out.emplace_back(page * size, size);
        }
    }
    return out;
}

void WindowRows::visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) {
    _first = std::max<std::int64_t>(first, 0);
    _count = std::max<std::int64_t>(count, 0);
    std::vector<std::string> named(columns.begin(), columns.end());
    if (_description) {
        std::erase_if(named, [this](std::string const& columnId) { return !_infos.contains(columnId); });
    }
    if (named != _columns) {
        _columns = std::move(named);
        ++_columnsStamp;
    }
    _hasVisible = true;
    auto const anchor = _first / std::max<std::int64_t>(_options.pageSize, 1);
    if (anchor != _anchor) {
        _anchor = anchor;
        _anchorSince = now();
    }
    pump();
}

async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> WindowRows::keysOf(std::int64_t first,
                                                                                                  std::int64_t count) {
    using Result = std::expected<std::vector<table::RowId>, table::TableError>;
    auto [completion, promise] = async::Completion<Result>::makeSettleable(&_session->runtime().owner());
    if (first < 0 || count < 0 || count > std::numeric_limits<std::int64_t>::max() - first) {
        promise.resolve(Result{std::unexpected(
            table::TableError{.code = table::TableErrorCode::InvalidSpec,
                              .column = {},
                              .message = std::format("{} rows from view row {} are not a range", count, first)})});
        return std::move(completion);
    }
    auto const size = std::max<std::int64_t>(_options.pageSize, 1);
    auto state = std::make_shared<KeysRequest>(KeysRequest{.promise = std::move(promise),
                                                           .first = first,
                                                           .count = count,
                                                           .pages = {},
                                                           .waiting = 0,
                                                           .failed = std::nullopt});
    std::vector<std::int64_t> missing;
    for (std::int64_t page = first / size; count > 0 && page <= (first + count - 1) / size; ++page) {
        std::int64_t const from = std::max(first, page * size);
        std::int64_t const end = std::min(first + count, (page + 1) * size);
        for (std::int64_t at = from; at < end; ++at) {
            if (_window.key(at) == nullptr) {
                missing.push_back(page);
                break;
            }
        }
    }
    if (missing.empty() || !_description) {
        finishKeys(*state);
        return std::move(completion);
    }
    state->waiting = missing.size();
    std::erase_if(_keyRequests, [](std::weak_ptr<KeysRequest> const& held) { return held.expired(); });
    _keyRequests.push_back(state);
    for (std::int64_t const page : missing) {
        table::TableQuery query = queryOf(table::PageRequest{.offset = page * size, .limit = size}, {});
        query.columns = std::vector<std::string>{};
        auto body = bodyOf(query);
        if (!body) {
            keysAnswered(*state, page, std::unexpected(body.error()));
            continue;
        }
        ::core::async::StopSource stop;
        auto reply = _session->read(_options.channel, "Table." + _options.table, *body, stop.get_token());
        state->stops.push_back(std::move(stop));
        reply.then(_scope, [this, state, page](std::string const& text) {
            auto read = table::readGenericPage(text, *_description, _units.get());
            if (!read) {
                keysAnswered(*state, page, std::unexpected(read.error()));
                return;
            }
            keysAnswered(*state, page, std::move(read->keys));
        });
        reply.onError(_scope, [this, state, page](std::exception_ptr const& error) {
            // A stopped fetch is the window going: the range is never answered.
            if (!wasCancelled(error)) {
                keysAnswered(*state, page, std::unexpected(failureOf(error)));
            }
        });
    }
    return std::move(completion);
}

void WindowRows::locateRow(table::RowId key, ::core::async::StopToken stop,
                           std::function<void(RowLocation const&)> done) {
    table::TableQuery query = queryOf(table::PageRequest{}, {});
    query.columns = std::vector<std::string>{};
    query.keys = std::vector<table::RowId>{std::move(key)};
    auto body = _description ? bodyOf(query) : std::expected<Value, table::TableError>{std::unexpected(unavailable())};
    if (!body) {
        _session->runtime().owner().post(
            _scope.guard([stop = std::move(stop), done = std::move(done), error = std::move(body.error())] {
                if (!stop.stop_requested()) {
                    done(std::unexpected(error));
                }
            }));
        return;
    }
    auto reply = _session->read(_options.channel, actionName(), *body, stop);
    reply.then(_scope, [this, done, stop](std::string const& text) {
        if (stop.stop_requested()) {
            return;
        }
        if (!_description) {
            done(std::unexpected(unavailable()));
            return;
        }
        // Only the indices are read: a typed reply here fixes no inferred kind, which a page's first rows do.
        auto read = _options.action.empty() ? table::readGenericPage(text, *_description, _units.get())
                                            : readTypedPage(text, *_description, _units.get());
        if (!read) {
            done(std::unexpected(read.error()));
            return;
        }
        // No row: the view lacks the key. A row without its index leaves where it is unknown, which is not
        // its absence.
        if (read->rows.empty()) {
            done(std::nullopt);
        } else if (read->indices.empty()) {
            done(std::unexpected(table::TableError{.code = table::TableErrorCode::InvalidValue,
                                                   .column = {},
                                                   .message = "the keyed reply gives its row no index"}));
        } else {
            done(std::optional<std::int64_t>{read->indices.front()});
        }
    });
    reply.onError(_scope, [done, stop](std::exception_ptr const& error) {
        if (!stop.stop_requested() && !wasCancelled(error)) {
            done(std::unexpected(failureOf(error)));
        }
    });
}

async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> WindowRows::fetchRows(
    std::vector<table::RowId> keys, std::vector<std::string> columns) {
    using Result = std::expected<std::vector<FetchedRow>, table::TableError>;
    auto [completion, promise] = async::Completion<Result>::makeSettleable(&_session->runtime().owner());
    auto state = std::make_shared<RowsRequest>(
        RowsRequest{.promise = std::move(promise), .rows = {}, .waiting = 0, .failed = std::nullopt, .stops = {}});
    if (keys.empty() || !_description) {
        if (!_description) {
            state->failed = unavailable();
        }
        finishRows(*state);
        return std::move(completion);
    }
    std::size_t const chunk = std::max<std::size_t>(_options.maxKeys, 1);
    state->waiting = (keys.size() + chunk - 1) / chunk;
    std::erase_if(_rowsRequests, [](std::weak_ptr<RowsRequest> const& held) { return held.expired(); });
    _rowsRequests.push_back(state);
    auto const named = std::make_shared<std::vector<std::string> const>(std::move(columns));
    for (std::size_t from = 0; from < keys.size(); from += chunk) {
        table::TableQuery query = queryOf(table::PageRequest{}, {});
        query.columns = *named;
        query.keys =
            std::vector<table::RowId>(keys.begin() + static_cast<std::ptrdiff_t>(from),
                                      keys.begin() + static_cast<std::ptrdiff_t>(std::min(from + chunk, keys.size())));
        auto body = bodyOf(query);
        if (!body) {
            rowsAnswered(*state, std::unexpected(std::move(body.error())));
            continue;
        }
        ::core::async::StopSource stop;
        auto reply = _session->read(_options.channel, actionName(), *body, stop.get_token());
        state->stops.push_back(std::move(stop));
        reply.then(_scope, [this, state, named](std::string const& text) {
            if (!_description) {
                rowsAnswered(*state, std::unexpected(unavailable()));
                return;
            }
            auto read = _options.action.empty() ? table::readGenericPage(text, *_description, _units.get())
                                                : readTypedPage(text, *_description, _units.get());
            if (!read) {
                rowsAnswered(*state, std::unexpected(std::move(read.error())));
                return;
            }
            rowsAnswered(*state, fetchedOf(*read, *named));
        });
        reply.onError(_scope, [state](std::exception_ptr const& error) {
            // A stopped fetch is the window going: the rows are never answered.
            if (!wasCancelled(error)) {
                rowsAnswered(*state, std::unexpected(failureOf(error)));
            }
        });
    }
    return std::move(completion);
}

void WindowRows::locate(table::RowId key, std::function<void(std::optional<std::int64_t>)> done) {
    stopLocating();
    locateRow(std::move(key), _locateStop.get_token(),
              [done = std::move(done)](RowLocation const& found) { done(found ? *found : std::nullopt); });
}

void WindowRows::stopLocating() {
    _locateStop.request_stop();
    _locateStop = ::core::async::StopSource{};
}

std::expected<void, table::TableError> WindowRows::setSort(SortChain sort) {
    if (_description) {
        if (auto error = sortError(sort)) {
            return std::unexpected(std::move(*error));
        }
    }
    if (sort == _sort) {
        return {};
    }
    _sort = std::move(sort);
    restart();
    return {};
}

std::expected<void, table::TableError> WindowRows::setFilter(table::FilterSpec filter) {
    if (_description) {
        auto const infos = table::columnInfos(*_description);
        // A measure's operand is read with the description's units.
        table::Services services;
        services.units = _units;
        auto compiled = table::compileFilterGuarded(filter, infos, services, _description->groups);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
    }
    if (filter == _filter) {
        return {};
    }
    _filter = std::move(filter);
    // The revision held is of the old filter's view: a selection taken before the new one answers would name it.
    _viewRevision.clear();
    _outOfView.clear();
    // The retained rows are of the filter they no longer matched.
    _retained.clear();
    _retainedSet.clear();
    ++_retainStamp;
    restart();
    return {};
}

async::Completion<std::expected<table::DistinctValues, table::TableError>> WindowRows::distinctValues(
    table::DistinctRequest request) {
    using Result = std::expected<table::DistinctValues, table::TableError>;
    auto [completion, promise] = async::Completion<Result>::makeSettleable(&_session->runtime().owner());
    if (!_description || !_description->capabilities.distinct || !_options.action.empty()) {
        promise.resolve(Result{std::unexpected(table::TableError{
            .code = table::TableErrorCode::Unavailable, .column = {}, .message = "the table lists no values"})});
        return std::move(completion);
    }
    table::TableQuery query;
    query.filters = _filter;
    query.distinct = std::move(request);
    auto body = bodyOf(query);
    if (!body) {
        promise.resolve(Result{std::unexpected(std::move(body.error()))});
        return std::move(completion);
    }
    std::string key = ui::detail::canonicalJson(*body);
    if (auto const held = _distinct.find(key); held != _distinct.end()) {
        promise.resolve(Result{held->second});
        return std::move(completion);
    }
    ::core::async::StopSource stop;
    ++_stats.distinctFetches;
    auto reply = _session->read(_options.channel, actionName(), *body, stop.get_token());
    // The oldest values list still in flight is stopped past the bound: its menu has asked for others since.
    std::uint64_t const flight = ++_distinctFlight;
    _distinctFlights.emplace(flight, std::move(stop));
    if (_distinctFlights.size() > kMaxDistinctFlights) {
        _distinctFlights.begin()->second.request_stop();
        _distinctFlights.erase(_distinctFlights.begin());
    }
    auto const settle = std::make_shared<async::Completion<Result>::Promise>(std::move(promise));
    reply.then(_scope, [this, settle, flight, epoch = _distinctEpoch, key = std::move(key)](std::string const& text) {
        _distinctFlights.erase(flight);
        auto read = table::readGenericPage(text, *_description, _units.get());
        if (!read) {
            settle->resolve(Result{std::unexpected(std::move(read.error()))});
            return;
        }
        if (!read->distinct) {
            settle->resolve(Result{std::unexpected(table::TableError{
                .code = table::TableErrorCode::InvalidValue, .column = {}, .message = "the reply holds no values"})});
            return;
        }
        // A reply asked for before the record changed, the session reconnected or the revision moved answers
        // its menu, and is not kept: it may count rows as they were.
        if (epoch == _distinctEpoch) {
            keepDistinct(key, *read->distinct);
        }
        settle->resolve(Result{std::move(*read->distinct)});
    });
    reply.onError(_scope, [this, settle, flight](std::exception_ptr const& error) {
        _distinctFlights.erase(flight);
        settle->resolve(Result{std::unexpected(failureOf(error))});
    });
    return std::move(completion);
}

TableCountsView WindowRows::counts() const {
    auto const awaiting = _window.awaiting();
    auto const failed = _window.failed();
    return TableCountsView{.view = total(),
                           .source = std::nullopt,
                           .undecided = _window.undecided(),
                           .pending = _window.pending() || !_rowsFlights.empty(),
                           .awaiting = {awaiting.begin(), awaiting.end()},
                           .failed = {failed.begin(), failed.end()},
                           .retained = static_cast<std::int64_t>(_unmatchedHeld.size()),
                           .busy = _states.busyCount()};
}

void WindowRows::moveKey(table::RowId const& from, table::RowId const& target) {
    _states.move(from, target);
    if (from == target || !_retainedSet.contains(from)) {
        return;
    }
    _retainedSet.erase(from);
    if (_retainedSet.insert(target).second) {
        std::ranges::replace(_retained, from, target);
    } else {
        std::erase(_retained, from);
    }
    _unmatched.erase(from);
    _unmatchedHeld.erase(from);
    // The revision held is of the view with the old key retained.
    ++_retainStamp;
    _viewRevision.clear();
}

RetainResult WindowRows::retain(std::span<table::RowId const> keys) {
    std::vector<table::RowId> added;
    std::unordered_set<table::RowId> seen;
    for (table::RowId const& key : keys) {
        if (!_retainedSet.contains(key) && seen.insert(key).second) {
            added.push_back(key);
        }
    }
    if (_retained.size() + added.size() > table::QueryLimits{}.maxRetained) {
        return RetainResult::OverBound;
    }
    if (added.empty()) {
        return RetainResult::Retained;
    }
    _retainedSet.insert(added.begin(), added.end());
    _retained.insert(_retained.end(), added.begin(), added.end());
    // The revision held is of the view without them: a selection taken before the next reply would name it.
    ++_retainStamp;
    _viewRevision.clear();
    refetch(added);
    return RetainResult::Retained;
}

void WindowRows::clearRetained() {
    if (_retained.empty()) {
        return;
    }
    _retained.clear();
    _retainedSet.clear();
    _unmatched.clear();
    _unmatchedHeld.clear();
    ++_retainStamp;
    _viewRevision.clear();
    _window.invalidate();
    pump();
    status();
}

void WindowRows::holdRefetches(std::uint64_t run) {
    auto* const scheduler = _session->options().scheduler;
    if (scheduler == nullptr) {
        return;
    }
    if (_holding.empty()) {
        _holdTimer = scheduler->after(_session->options().retainHold, _scope.token().guard([this] {
            _holdTimer = {};
            _holding.clear();
            flushMarked();
        }));
    }
    _holding.insert(run);
}

void WindowRows::releaseRefetches(std::uint64_t run) {
    if (_holding.erase(run) == 0 || !_holding.empty()) {
        return;
    }
    _holdTimer = {};
    flushMarked();
}

std::expected<ViewSelection, SelectAllRefusal> WindowRows::selectView() const {
    auto const shown = counts();
    std::vector<std::string> levels;
    if (_description) {
        std::vector<std::string> filtered;
        for (auto const& [column, entry] : _filter.columns) {
            static_cast<void>(entry);
            filtered.push_back(column);
        }
        for (table::GroupFilter const& group : _filter.groups) {
            filtered.insert(filtered.end(), group.columns.begin(), group.columns.end());
            for (table::ColumnGroupInfo const& declared : _description->groups) {
                if (!group.group.empty() && declared.id == group.group) {
                    filtered.insert(filtered.end(), declared.columns.begin(), declared.columns.end());
                }
            }
        }
        for (table::TableColumn const& column : _description->columns) {
            if (!column.info.level.empty() && std::ranges::contains(filtered, column.info.id) &&
                std::ranges::contains(shown.awaiting, column.info.level) &&
                !std::ranges::contains(levels, column.info.level)) {
                levels.push_back(column.info.level);
            }
        }
    }
    if (shown.undecided > 0 || !levels.empty() || _viewRevision.empty()) {
        return std::unexpected(SelectAllRefusal{.undecided = shown.undecided, .levels = std::move(levels)});
    }
    return ViewSelection{.filters = _filter, .retained = _retained, .revision = _viewRevision};
}

SelectedCount WindowRows::selectedCount(TableSelection const& selection) const {
    if (!selection.view) {
        return SelectedCount{.rows = static_cast<std::int64_t>(selection.keys.size()), .kind = CountKind::Exact};
    }
    auto const view = total();
    std::unordered_set<table::RowId> held;
    for (auto const& [first, count] : heldRanges()) {
        for (std::int64_t at = first; at < first + count; ++at) {
            if (auto const* const key = _window.key(at)) {
                held.insert(*key);
            }
        }
    }
    std::unordered_set<table::RowId> owed;
    for (RowsFlight const& flight : _rowsFlights) {
        owed.insert(flight.fetch.keys.begin(), flight.fetch.keys.end());
    }
    bool exact = view.known && view.kind == table::TotalKind::Exact;
    std::int64_t rows = view.rows;
    for (table::RowId const& key : selection.except) {
        if (held.contains(key)) {
            --rows;
            exact = exact && !owed.contains(key);
        } else {
            exact = exact && _outOfView.contains(key);
        }
    }
    return SelectedCount{.rows = std::max<std::int64_t>(rows, 0),
                         .kind = exact ? CountKind::Exact : CountKind::Approximate};
}

async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> WindowRows::rowsOf(
    std::int64_t first, std::int64_t count, std::span<std::string const> columns) {
    using Result = std::expected<std::vector<FetchedRow>, table::TableError>;
    auto [completion, promise] = async::Completion<Result>::makeSettleable(&_session->runtime().owner());
    auto state = std::make_shared<RowsRequest>(
        RowsRequest{.promise = std::move(promise), .rows = {}, .waiting = 0, .failed = std::nullopt, .stops = {}});
    if (!_description || first < 0 || count <= 0) {
        if (!_description) {
            state->failed = unavailable();
        }
        finishRows(*state);
        return std::move(completion);
    }
    auto const size = std::max<std::int64_t>(_options.pageSize, 1);
    auto const named = std::make_shared<std::vector<std::string> const>(columns.begin(), columns.end());
    std::vector<std::int64_t> offsets;
    for (std::int64_t offset = first; offset < first + count; offset += size) {
        offsets.push_back(offset);
    }
    state->waiting = offsets.size();
    std::erase_if(_rowsRequests, [](std::weak_ptr<RowsRequest> const& held) { return held.expired(); });
    _rowsRequests.push_back(state);
    for (std::int64_t const offset : offsets) {
        table::TableQuery query =
            queryOf(table::PageRequest{.offset = offset, .limit = std::min(size, first + count - offset)}, {});
        query.columns = *named;
        auto body = bodyOf(query);
        if (!body) {
            rowsAnswered(*state, std::unexpected(std::move(body.error())));
            continue;
        }
        ::core::async::StopSource stop;
        auto reply = _session->read(_options.channel, actionName(), *body, stop.get_token());
        state->stops.push_back(std::move(stop));
        reply.then(_scope, [this, state, named](std::string const& text) {
            if (!_description) {
                rowsAnswered(*state, std::unexpected(unavailable()));
                return;
            }
            auto read = _options.action.empty() ? table::readGenericPage(text, *_description, _units.get())
                                                : readTypedPage(text, *_description, _units.get());
            if (!read) {
                rowsAnswered(*state, std::unexpected(std::move(read.error())));
                return;
            }
            // A page's rows are at its offset onward.
            read->indices.clear();
            for (std::size_t at = 0; at < read->rows.size(); ++at) {
                read->indices.push_back(read->offset + static_cast<std::int64_t>(at));
            }
            rowsAnswered(*state, fetchedOf(*read, *named));
        });
        reply.onError(_scope, [state](std::exception_ptr const& error) {
            if (!wasCancelled(error)) {
                rowsAnswered(*state, std::unexpected(failureOf(error)));
            }
        });
    }
    return std::move(completion);
}

void WindowRows::keysAnswered(KeysRequest& request, std::int64_t page,
                              std::expected<std::vector<table::RowId>, table::TableError> keys) {
    if (keys) {
        request.pages.insert_or_assign(page, std::move(*keys));
    } else if (!request.failed) {
        request.failed = std::move(keys.error());
    }
    if (request.waiting > 0) {
        --request.waiting;
        if (request.waiting == 0) {
            finishKeys(request);
        }
    }
}

void WindowRows::finishKeys(KeysRequest& request) {
    using Result = std::expected<std::vector<table::RowId>, table::TableError>;
    if (request.failed) {
        request.promise.resolve(Result{std::unexpected(*request.failed)});
        return;
    }
    auto const size = std::max<std::int64_t>(_options.pageSize, 1);
    std::vector<table::RowId> keys;
    keys.reserve(static_cast<std::size_t>(request.count));
    for (std::int64_t at = request.first; at < request.first + request.count; ++at) {
        if (auto const page = request.pages.find(at / size); page != request.pages.end()) {
            auto const place = static_cast<std::size_t>(at - ((at / size) * size));
            if (place < page->second.size()) {
                keys.push_back(page->second.at(place));
                continue;
            }
        } else if (auto const* const key = _window.key(at)) {
            keys.push_back(*key);
            continue;
        }
        request.promise.resolve(
            Result{std::unexpected(table::TableError{.code = table::TableErrorCode::Unavailable,
                                                     .column = {},
                                                     .message = std::format("view row {} is in no page", at)})});
        return;
    }
    request.promise.resolve(Result{std::move(keys)});
}

void WindowRows::rowsAnswered(RowsRequest& request, std::expected<std::vector<FetchedRow>, table::TableError> rows) {
    if (rows) {
        request.rows.insert(request.rows.end(), std::make_move_iterator(rows->begin()),
                            std::make_move_iterator(rows->end()));
    } else if (!request.failed) {
        request.failed = std::move(rows.error());
    }
    if (request.waiting > 0) {
        --request.waiting;
        if (request.waiting == 0) {
            finishRows(request);
        }
    }
}

void WindowRows::finishRows(RowsRequest& request) {
    using Result = std::expected<std::vector<FetchedRow>, table::TableError>;
    if (request.failed) {
        request.promise.resolve(Result{std::unexpected(*request.failed)});
        return;
    }
    request.promise.resolve(Result{std::move(request.rows)});
}

std::expected<std::vector<FetchedRow>, table::TableError> WindowRows::fetchedOf(
    table::GenericPage& page, std::vector<std::string> const& columns) const {
    if (!page.rows.empty() && page.indices.size() != page.rows.size()) {
        return std::unexpected(table::TableError{.code = table::TableErrorCode::InvalidValue,
                                                 .column = {},
                                                 .message = "the keyed reply gives its rows no index"});
    }
    std::vector<std::optional<std::size_t>> places;
    places.reserve(columns.size());
    for (std::string const& columnId : columns) {
        auto const found = std::ranges::find(page.columns, columnId);
        places.push_back(found == page.columns.end()
                             ? std::nullopt
                             : std::optional{static_cast<std::size_t>(found - page.columns.begin())});
    }
    std::vector<FetchedRow> out;
    out.reserve(page.rows.size());
    for (std::size_t at = 0; at < page.rows.size() && at < page.keys.size(); ++at) {
        auto const unloaded =
            std::ranges::find(page.unloadedRows, static_cast<std::int64_t>(at), &table::UnloadedRow::index);
        FetchedRow row{.key = std::move(page.keys.at(at)), .viewRow = page.indices.at(at), .cells = {}};
        row.cells.reserve(columns.size());
        for (std::size_t column = 0; column < columns.size(); ++column) {
            std::optional<std::size_t> const place = places.at(column);
            table::GenericRow& cells = page.rows.at(at);
            if (!place || *place >= cells.size()) {
                row.cells.emplace_back();
                continue;
            }
            auto const info = _infos.find(columns.at(column));
            bool const missing = unloaded != page.unloadedRows.end() && info != _infos.end() &&
                                 !info->second.level.empty() &&
                                 std::ranges::contains(unloaded->levels, info->second.level);
            row.cells.push_back(missing ? table::Cell{table::Unloaded{}} : std::move(cells.at(*place)));
        }
        out.push_back(std::move(row));
    }
    return out;
}

std::chrono::milliseconds WindowRows::now() const {
    auto const& clock = _session->options().clock;
    if (clock) {
        return clock();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

std::optional<table::TableError> WindowRows::sortError(SortChain const& sort) const {
    if (!_description) {
        return unavailable();
    }
    auto const error = [](table::TableErrorCode code, std::string column, std::string message) {
        return table::TableError{.code = code, .column = std::move(column), .message = std::move(message)};
    };
    if (sort.size() > _description->capabilities.maxSortKeys) {
        return error(
            table::TableErrorCode::LimitExceeded, {},
            std::format("{} sort keys; the table takes {}", sort.size(), _description->capabilities.maxSortKeys));
    }
    for (std::size_t at = 0; at < sort.size(); ++at) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < sort.size() by the loop
        auto const& columnId = sort[at].column;
        auto const column =
            std::ranges::find(_description->columns, columnId,
                              [](table::TableColumn const& candidate) -> auto const& { return candidate.info.id; });
        if (column == _description->columns.end()) {
            return error(table::TableErrorCode::UnknownColumn, columnId, "no column " + columnId);
        }
        if (!column->sortable) {
            return error(table::TableErrorCode::InvalidSpec, columnId, "the column " + columnId + " is not sortable");
        }
        if (std::ranges::contains(sort.begin(), sort.begin() + static_cast<std::ptrdiff_t>(at), columnId,
                                  &table::SortKey::column)) {
            return error(table::TableErrorCode::InvalidSpec, columnId, "the column " + columnId + " is sorted twice");
        }
    }
    return std::nullopt;
}

void WindowRows::restart() {
    stopAll();
    _window.reset();
    _memo.clear();
    _heldPages.clear();
    _unmatched.clear();
    _unmatchedHeld.clear();
    _retry = false;
    _awaitedLast.reset();
    _awaitedTimer = {};
    announce(table::ViewChange{
        .ops = {table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0}}});
    pump();
}

void WindowRows::stopAll() {
    for (auto& flight : _pageFlights) {
        flight.stop.request_stop();
    }
    for (auto& flight : _rowsFlights) {
        flight.stop.request_stop();
    }
    _pageFlights.clear();
    _rowsFlights.clear();
}

bool WindowRows::heldPage(std::int64_t page) const {
    auto const size = std::max<std::int64_t>(_options.pageSize, 1);
    return page >= 0 && _window.key(page * size) != nullptr;
}

bool WindowRows::due(table::PageFetch const& fetch, std::chrono::milliseconds instant) const {
    if (_session->options().scheduler == nullptr) {
        return true;
    }
    auto const page = fetch.request.offset / std::max<std::int64_t>(_options.pageSize, 1);
    if (heldPage(page) || heldPage(page - 1) || heldPage(page + 1)) {
        return true;
    }
    if (_window.pagesHeld() == 0 && page == _anchor) {
        return true;
    }
    return instant - _anchorSince >= _session->options().tableSettle;
}

bool WindowRows::spaced(std::chrono::milliseconds instant) const {
    return !_window.awaiting().empty() && _session->options().scheduler != nullptr && _awaitedLast &&
           instant - *_awaitedLast < _session->options().tableAwaitedRefetch;
}

void WindowRows::pump() {
    if (!ready()) {
        return;
    }
    auto fetches = _window.visible(_first, _count, _columns);
    for (auto const& superseded : _window.superseded()) {
        auto const flight = std::ranges::find(_pageFlights, superseded, &PageFlight::fetch);
        if (flight != _pageFlights.end()) {
            flight->stop.request_stop();
            _pageFlights.erase(flight);
        }
        _window.cancelled(superseded);
        ++_stats.cancelled;
    }
    auto const instant = now();
    auto const size = std::max<std::int64_t>(_options.pageSize, 1);
    bool settling = false;
    bool spacing = false;
    bool refetched = false;
    for (auto const& fetch : fetches) {
        bool const refetch = heldPage(fetch.request.offset / size);
        if (refetch && spaced(instant)) {
            _window.cancelled(fetch);
            spacing = true;
        } else if (due(fetch, instant)) {
            refetched = refetched || refetch;
            send(fetch);
        } else {
            _window.cancelled(fetch);
            settling = true;
        }
    }
    if (refetched && !_window.awaiting().empty()) {
        _awaitedLast = instant;
    }
    // A retry's fetches carry the flag until the last of them, held back or not, has gone.
    if (!settling && !spacing) {
        _retry = false;
    }
    auto* const scheduler = _session->options().scheduler;
    if (settling) {
        auto const wait = _anchorSince + _session->options().tableSettle - instant;
        _settleTimer = scheduler->after(std::max(wait, std::chrono::milliseconds{1}), _scope.token().guard([this] {
            _settleTimer = {};
            pump();
        }));
    }
    if (spacing && _awaitedLast && !_awaitedTimer.active()) {
        auto const wait = *_awaitedLast + _session->options().tableAwaitedRefetch - instant;
        _awaitedTimer = scheduler->after(std::max(wait, std::chrono::milliseconds{1}), _scope.token().guard([this] {
            _awaitedTimer = {};
            pump();
        }));
    }
}

std::expected<Value, table::TableError> WindowRows::bodyOf(table::TableQuery const& query) const {
    auto const invalid = [](std::string message) {
        return std::unexpected(table::TableError{
            .code = table::TableErrorCode::InvalidValue, .column = {}, .message = std::move(message)});
    };
    auto const text = glz::write_json(query);
    if (!text) {
        return invalid("the query does not write");
    }
    auto const json = parseJson(*text);
    if (!json) {
        return invalid(json.error());
    }
    auto const value = valueFromJson(*json);
    if (!value || value->asRecord() == nullptr) {
        return invalid(value ? std::string{"the query is not a record"} : value.error());
    }
    ValueRecord body;
    if (auto const* const fixed = _options.body.asRecord()) {
        body = *fixed;
    }
    std::string const member = _options.action.empty() ? std::string{"query"} : _options.queryMember;
    ValueRecord members;
    if (auto const found = body.find(member); found != body.end() && found->second.asRecord() != nullptr) {
        members = *found->second.asRecord();
    }
    for (auto const& [name, field] : *value->asRecord()) {
        members.insert_or_assign(name, field);
    }
    body.insert_or_assign(member, Value{std::move(members)});
    return Value{std::move(body)};
}

table::TableQuery WindowRows::queryOf(table::PageRequest page, std::vector<std::string> const& columns) const {
    table::TableQuery query;
    query.sort = _sort;
    query.filters = _filter;
    query.page = page;
    if (!columns.empty()) {
        query.columns = columns;
    }
    query.retry = _retry;
    query.retain = _retained;
    return query;
}

void WindowRows::send(table::PageFetch const& fetch) {
    auto body = bodyOf(queryOf(fetch.request, fetch.columns));
    if (!body) {
        _window.fail(fetch, body.error());
        _error = std::move(body.error());
        return;
    }
    ::core::async::StopSource stop;
    ++_stats.pageFetches;
    auto reply = _session->read(_options.channel, actionName(), *body, stop.get_token());
    _pageFlights.push_back(PageFlight{.fetch = fetch, .stop = std::move(stop), .retained = _retainStamp});
    reply.then(_scope, [this, fetch](std::string const& text) { pageArrived(fetch, text); });
    reply.onError(_scope, [this, fetch](std::exception_ptr const& error) { pageFailed(fetch, error); });
    status();
}

void WindowRows::send(table::RowsFetch const& fetch) {
    auto query = queryOf(table::PageRequest{}, fetch.columns);
    query.keys = fetch.keys;
    auto body = bodyOf(query);
    if (!body) {
        keyedFailed(std::move(body.error()));
        return;
    }
    ::core::async::StopSource stop;
    ++_stats.rowsFetches;
    auto reply = _session->read(_options.channel, actionName(), *body, stop.get_token());
    _rowsFlights.push_back(RowsFlight{.fetch = fetch, .stop = std::move(stop), .retained = _retainStamp});
    reply.then(_scope, [this, fetch](std::string const& text) { rowsArrived(fetch, text); });
    reply.onError(_scope, [this, fetch](std::exception_ptr const& error) { rowsFailed(fetch, error); });
    status();
}

void WindowRows::answered() {
    if (_unanswered > 0) {
        --_unanswered;
        if (_unanswered == 0) {
            _watched = true;
            pump();
        }
    }
}

std::expected<table::GenericPage, table::TableError> WindowRows::decode(std::string const& text) {
    if (!_description) {
        return std::unexpected(unavailable());
    }
    // Held by reference: the helpers below change the description in place and never replace it.
    table::TableDescription& description = *_description;
    if (_options.action.empty()) {
        return table::readGenericPage(text, description, _units.get());
    }
    auto dom = detail::typedPageDom(text);
    if (!dom) {
        return std::unexpected(std::move(dom.error()));
    }
    if (!_inferring || dom->rows.empty()) {
        listUnits(dom->rows);
        return detail::typedPageOf(*dom, description, _units.get());
    }
    // The kinds are fixed only by a reply that reads: one that does not leaves them to the next.
    auto const before = description.columns;
    inferKinds(dom->rows);
    listUnits(dom->rows);
    auto page = detail::typedPageOf(*dom, description, _units.get());
    if (!page) {
        description.columns = before;
        for (auto const& column : before) {
            _infos.insert_or_assign(column.info.id, column.info);
        }
        ++_columnsStamp;
        return page;
    }
    _inferring = false;
    if (_onInferred) {
        _onInferred();
    }
    return page;
}

void WindowRows::listUnits(std::span<glz::generic_u64 const> rows) {
    if (!_description) {
        return;
    }
    auto const added = detail::unlistedUnits(
        rows, *_description, _units.get(), detail::kMaxUnlistedUnits - std::min(_unlisted, detail::kMaxUnlistedUnits));
    if (added.empty()) {
        return;
    }
    _unlisted += added.size();
    _description->units.insert(_description->units.end(), added.begin(), added.end());
    auto registry = table::registryOf(*_description);
    _units = registry ? std::move(*registry) : nullptr;
    _memo.clear();
    ++_columnsStamp;
}

void WindowRows::inferKinds(std::span<glz::generic_u64 const> rows) {
    if (!_description) {
        return;
    }
    for (auto& column : _description->columns) {
        if (std::ranges::contains(_options.inferKinds, column.info.id)) {
            column.info.kind = detail::inferredJsonKind(rows, column.info.id);
            _infos.insert_or_assign(column.info.id, column.info);
        }
    }
    auto registry = table::registryOf(*_description);
    _units = registry ? std::move(*registry) : nullptr;
    _memo.clear();
    ++_columnsStamp;
}

bool WindowRows::wasCancelled(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (::core::async::OperationCancelled const&) {
        return true;
    } catch (...) {
        return false;
    }
}

table::TableError WindowRows::failureOf(std::exception_ptr const& error) {
    return table::TableError{
        .code = table::TableErrorCode::Unavailable, .column = {}, .message = ui::detail::failureText(error)};
}

table::TableError WindowRows::unavailable() {
    return table::TableError{.code = table::TableErrorCode::Unavailable, .column = {}, .message = "no description"};
}

void WindowRows::heardRevision(std::string const& revision) {
    if (_description && revision != _description->revision && _staleRevisions.insert(revision).second && _onStale) {
        _onStale();
    }
}

std::vector<std::optional<table::RowId>> WindowRows::slotsOfPage(std::int64_t page) const {
    auto const size = std::max<std::int64_t>(_options.pageSize, 1);
    std::vector<std::optional<table::RowId>> out;
    out.reserve(static_cast<std::size_t>(size));
    for (std::int64_t at = page * size; at < (page + 1) * size; ++at) {
        auto const* const key = _window.key(at);
        out.push_back(key == nullptr ? std::nullopt : std::optional{*key});
    }
    return out;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-easily-swappable-parameters): page, count
table::ViewChange WindowRows::pageChange(std::int64_t page, std::int64_t rowsBefore,
                                         std::vector<std::optional<table::RowId>> const& before,
                                         std::set<std::int64_t> const& heldBefore) const {
    using table::ViewOp;
    auto const reset = [] {
        return table::ViewChange{.ops = {ViewOp{.kind = ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0}}};
    };
    auto const size = std::max<std::int64_t>(_options.pageSize, 1);
    auto const rowsAfter = shownRows();
    auto const start = page * size;
    auto const after = slotsOfPage(page);
    table::ViewChange change;
    if (rowsAfter < rowsBefore) {
        change.ops.push_back(ViewOp{.kind = ViewOp::Kind::Removed,
                                    .first = static_cast<std::size_t>(rowsAfter),
                                    .count = static_cast<std::size_t>(rowsBefore - rowsAfter),
                                    .to = 0});
    }
    auto const places =
        static_cast<std::size_t>(std::clamp<std::int64_t>(std::min(rowsBefore, rowsAfter) - start, 0, size));
    std::unordered_map<table::RowId, std::size_t> wasAt;
    std::unordered_map<table::RowId, std::size_t> nowAt;
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < places <= slots' size
    for (std::size_t at = 0; at < places; ++at) {
        auto const& previous = before[at];
        auto const& current = after[at];
        bool const unique =
            (!previous || wasAt.emplace(*previous, at).second) && (!current || nowAt.emplace(*current, at).second);
        if (!unique) {
            return reset();
        }
    }
    // Identities: each key one number; each place's own empty slot one number after every key's.
    std::unordered_map<table::RowId, std::uint32_t> ids;
    auto const idOf = [&ids](table::RowId const& key) {
        return ids.try_emplace(key, static_cast<std::uint32_t>(ids.size())).first->second;
    };
    std::vector<std::uint32_t> changed;
    for (std::size_t at = 0; at < places; ++at) {
        if (auto const& previous = before[at]) {
            idOf(*previous);
        }
        if (auto const& current = after[at]) {
            changed.push_back(idOf(*current));
        }
    }
    auto const emptyBase = static_cast<std::uint32_t>(ids.size());
    auto const emptyAt = [emptyBase](std::size_t index) { return emptyBase + static_cast<std::uint32_t>(index); };
    std::vector<std::uint32_t> was(places);
    std::vector<std::uint32_t> now(places);
    for (std::size_t at = 0; at < places; ++at) {
        auto const& previous = before[at];
        auto const& current = after[at];
        if (previous) {
            was[at] = ids.at(*previous);
        } else if (current && !wasAt.contains(*current)) {
            was[at] = ids.at(*current);
        } else {
            was[at] = emptyAt(at);
        }
        if (current) {
            now[at] = ids.at(*current);
        } else if (previous && !nowAt.contains(*previous)) {
            now[at] = ids.at(*previous);
        } else {
            now[at] = emptyAt(at);
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    auto inPage = table::diffViews(was, now, changed, static_cast<std::size_t>(emptyBase) + places, kResetThreshold);
    if (inPage.isReset()) {
        return inPage;
    }
    for (auto op : inPage.ops) {
        op.first += static_cast<std::size_t>(start);
        if (op.kind == ViewOp::Kind::Moved) {
            op.to += static_cast<std::size_t>(start);
        }
        change.ops.push_back(op);
    }
    if (rowsAfter > rowsBefore) {
        change.ops.push_back(ViewOp{.kind = ViewOp::Kind::Inserted,
                                    .first = static_cast<std::size_t>(rowsBefore),
                                    .count = static_cast<std::size_t>(rowsAfter - rowsBefore),
                                    .to = 0});
    }
    for (auto const held : heldBefore) {
        if (held == page || heldPage(held)) {
            continue;
        }
        auto const first = held * size;
        auto const last = std::min(first + size, rowsAfter);
        if (first < last) {
            change.ops.push_back(ViewOp{.kind = ViewOp::Kind::Changed,
                                        .first = static_cast<std::size_t>(first),
                                        .count = static_cast<std::size_t>(last - first),
                                        .to = 0});
        }
    }
    return change;
}

void WindowRows::pageArrived(table::PageFetch const& fetch, std::string const& text) {
    auto const flight = std::ranges::find(_pageFlights, fetch, &PageFlight::fetch);
    std::uint64_t const stamp = flight == _pageFlights.end() ? 0 : flight->retained;
    std::erase_if(_pageFlights, [&fetch](PageFlight const& other) { return other.fetch == fetch; });
    if (!_description) {
        return;
    }
    auto page = decode(text);
    if (!page) {
        _window.fail(fetch, page.error());
        _error = std::move(page.error());
        status();
        return;
    }
    heardRevision(page->revision);
    if (fetch.query == _window.query()) {
        // A reply to a query of another retained set names that set's view, and its rows' marks are of that set.
        if (stamp == _retainStamp) {
            if (!page->viewRevision.empty()) {
                _viewRevision = page->viewRevision;
            }
            heardUnmatched(*page);
        }
        for (table::RowId const& key : page->keys) {
            _outOfView.erase(key);
        }
    }
    auto const size = std::max<std::int64_t>(_options.pageSize, 1);
    auto const index = fetch.request.offset / size;
    auto const before = slotsOfPage(index);
    auto const rowsBefore = shownRows();
    if (_window.accept(fetch, std::move(*page))) {
        _error.reset();
        auto const change = pageChange(index, rowsBefore, before, _heldPages);
        for (auto const& slot : before) {
            if (slot) {
                _memo.erase(*slot);
            }
        }
        for (auto const& slot : slotsOfPage(index)) {
            if (slot) {
                _memo.erase(*slot);
            }
        }
        std::erase_if(_heldPages, [this](std::int64_t held) { return !heldPage(held); });
        if (heldPage(index)) {
            _heldPages.insert(index);
        }
        heldUnmatched();
        announce(change);
    }
    planned(_window.owed());
    pump();
    status();
}

void WindowRows::pageFailed(table::PageFetch const& fetch, std::exception_ptr const& error) {
    std::erase_if(_pageFlights, [&fetch](PageFlight const& other) { return other.fetch == fetch; });
    if (wasCancelled(error)) {
        return;
    }
    auto failure = failureOf(error);
    _window.fail(fetch, failure);
    _error = std::move(failure);
    status();
}

void WindowRows::rowsArrived(table::RowsFetch const& fetch, std::string const& text) {
    auto const flight = std::ranges::find(_rowsFlights, fetch, &RowsFlight::fetch);
    std::uint64_t const stamp = flight == _rowsFlights.end() ? 0 : flight->retained;
    std::erase_if(_rowsFlights, [&fetch](RowsFlight const& other) { return other.fetch == fetch; });
    if (!_description) {
        return;
    }
    auto page = decode(text);
    if (!page) {
        keyedFailed(std::move(page.error()));
        return;
    }
    heardRevision(page->revision);
    // Only a reply under the query the window holds says a row left: under another, the key may be in the view.
    std::vector<table::RowId> absent;
    if (fetch.query == _window.query()) {
        if (stamp == _retainStamp) {
            if (!page->viewRevision.empty()) {
                _viewRevision = page->viewRevision;
            }
            heardUnmatched(*page);
        }
        std::unordered_set<table::RowId> const answered(page->keys.begin(), page->keys.end());
        for (table::RowId const& key : fetch.keys) {
            if (answered.contains(key)) {
                _outOfView.erase(key);
            } else {
                _outOfView.insert(key);
                absent.push_back(key);
            }
        }
    }
    auto const revision = _window.revision();
    auto const replaced = _window.patch(fetch, std::move(*page));
    heldUnmatched();
    table::ViewChange change;
    for (auto const index : replaced) {
        if (auto const* const key = _window.key(index)) {
            _memo.erase(*key);
        }
        auto const row = static_cast<std::size_t>(index);
        if (!change.ops.empty() && change.ops.back().first + change.ops.back().count == row) {
            ++change.ops.back().count;
        } else {
            change.ops.push_back(
                table::ViewOp{.kind = table::ViewOp::Kind::Changed, .first = row, .count = 1, .to = 0});
        }
    }
    announce(change);
    if (!absent.empty() && _onRowsAbsent) {
        std::function<void(std::span<table::RowId const>)> const heard = _onRowsAbsent;
        heard(absent);
    }
    if (_window.revision() != revision) {
        pump();
    }
    status();
}

void WindowRows::rowsFailed(table::RowsFetch const& fetch, std::exception_ptr const& error) {
    std::erase_if(_rowsFlights, [&fetch](RowsFlight const& other) { return other.fetch == fetch; });
    if (wasCancelled(error)) {
        return;
    }
    keyedFailed(failureOf(error));
}

void WindowRows::keyedFailed(table::TableError error) {
    _error = std::move(error);
    _window.invalidate();
    pump();
    status();
}

void WindowRows::planned(table::RefreshPlan const& plan) {
    if (plan.kind == table::Refresh::Rows) {
        send(plan.rows);
    } else if (plan.kind == table::Refresh::Pages) {
        pump();
    }
}

void WindowRows::keepDistinct(std::string const& key, table::DistinctValues const& values) {
    if (!_distinct.contains(key)) {
        _distinctOrder.push_back(key);
    }
    _distinct.insert_or_assign(key, values);
    while (_distinctOrder.size() > kMaxDistinctReplies) {
        _distinct.erase(_distinctOrder.front());
        _distinctOrder.pop_front();
    }
}

void WindowRows::forgetDistinct() {
    _distinct.clear();
    _distinctOrder.clear();
    ++_distinctEpoch;
}

void WindowRows::heard(effects::RecordChange const& change) {
    // Any row may change a count: the values lists are asked again.
    forgetDistinct();
    std::optional<std::vector<table::RowId>> keys;
    if (change.keys) {
        keys.emplace();
        keys->reserve(change.keys->size());
        for (auto const& raw : *change.keys) {
            auto key = table::rowIdOf(raw);
            if (!key) {
                keys.reset();
                break;
            }
            keys->push_back(std::move(*key));
        }
    }
    // A change of a row ends its failed mark. An action's own event names the rows it changed and not those it
    // could not, so it leaves their marks; an event without keys names no row and ends none.
    if (keys && !_states.failedKeys().empty()) {
        auto const marked = _states.failedKeys().size();
        _states.clearFailed(*keys);
        if (_states.failedKeys().size() != marked) {
            status();
        }
    }
    // While a run holds the refetches, the event only marks what it would fetch.
    if (!_holding.empty()) {
        if (keys) {
            _marked.insert(_marked.end(), keys->begin(), keys->end());
        } else {
            _markedAll = true;
        }
        return;
    }
    if (!keys) {
        _window.invalidate();
        pump();
        return;
    }
    planned(_window.refresh(*keys));
}

void WindowRows::refetch(std::vector<table::RowId> const& keys) {
    if (!_holding.empty()) {
        _marked.insert(_marked.end(), keys.begin(), keys.end());
        return;
    }
    auto plan = _window.refresh(keys);
    // A window that holds no row has no keyed fetch to make: its pages are fetched again.
    if (plan.kind == table::Refresh::Nothing) {
        _window.invalidate();
        pump();
        return;
    }
    planned(plan);
}

void WindowRows::flushMarked() {
    bool const all = std::exchange(_markedAll, false);
    auto keys = std::move(_marked);
    _marked.clear();
    if (all) {
        _window.invalidate();
        pump();
    } else if (!keys.empty()) {
        std::ranges::sort(keys);
        auto const [first, last] = std::ranges::unique(keys);
        keys.erase(first, last);
        refetch(keys);
    }
}

void WindowRows::heardUnmatched(table::GenericPage const& page) {
    for (std::size_t at = 0; at < page.keys.size(); ++at) {
        if (std::ranges::binary_search(page.unmatched, static_cast<std::int64_t>(at))) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < page.keys.size() by the loop
            _unmatched.insert(page.keys[at]);
        } else {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < page.keys.size() by the loop
            _unmatched.erase(page.keys[at]);
        }
    }
}

void WindowRows::heldUnmatched() {
    _unmatchedHeld.clear();
    if (_unmatched.empty()) {
        return;
    }
    for (auto const& [first, count] : heldRanges()) {
        for (std::int64_t at = first; at < first + count; ++at) {
            if (auto const* const key = _window.key(at); key != nullptr && _unmatched.contains(*key)) {
                _unmatchedHeld.insert(*key);
            }
        }
    }
}

void WindowRows::status() const {
    if (_onStatus) {
        _onStatus();
    }
}

void WindowRows::announce(table::ViewChange const& change) {
    if (change.empty()) {
        return;
    }
    // Records of rows no longer held are not looked up again; past the window's own bound they are dropped.
    if (_memo.size() > _options.maxPages * static_cast<std::size_t>(std::max<std::int64_t>(_options.pageSize, 1))) {
        _memo.clear();
    }
    if (_onChange) {
        _onChange(change);
    }
}

}  // namespace morph::ui
