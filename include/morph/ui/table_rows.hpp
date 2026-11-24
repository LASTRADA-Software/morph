// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ui/table_rows.hpp
/// @brief What a table's mount reads, whoever serves its rows: `TableRows`, over the engine (`EngineRows`) or over
///        a server's pages (`WindowRows`); and `ValueRowsSource`, a document's rows as the engine's source.
///
/// `EngineRows` holds every row: `visible` does nothing and `keysOf` completes at once. `WindowRows` holds the
/// pages around the visible rows in a `table::PageWindow`, sends the fetches the window asks for through the
/// session's read, which makes no cache entry, and keeps the window current from the table record's change events.
/// It owns the two timing rules the window has no clock for: when a fetch goes while the user scrolls, and how
/// often an order that waits for a level is fetched again.
///
/// A row is a record keyed by column id. `EngineRows` builds it with the caller's `RecordOf`; `WindowRows` converts
/// the cells the latest `visible` call named, one at a time. Both keep the record until the row changes.
///
/// See `docs/spec/table/table.md`, "The client table model".

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
#include <morph/ui/value.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <set>
#include <unordered_set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

namespace testing {
/// @brief What a test reads of a `WindowRows` that its interface does not show; the tests define it.
struct WindowRowsProbe;
}  // namespace testing

/// @brief The rows of a table's view, as far as the model knows them.
struct TableTotal {
    /// @brief Rows in the view, or a lower bound (see `kind`).
    std::int64_t rows = 0;
    /// @brief Whether `rows` is the count of the view or a lower bound.
    table::TotalKind kind = table::TotalKind::Exact;
    /// @brief `false` until a server-mode table's first reply; always `true` over the engine.
    bool known = false;

    /// @brief Member-wise equality.
    /// @param other The total to compare with.
    /// @return `true` when rows, kind and knowledge match.
    [[nodiscard]] bool operator==(TableTotal const& other) const = default;
};

/// @brief One row of the view: its key and its record, both owned by the model.
///
/// The references hold until the model's next `onChange`.
struct TableRowRef {
    /// @brief The row's key.
    table::RowId const& key;
    /// @brief The row as a record keyed by column id. A field the model does not hold for the row is absent.
    Value const& row;
};

/// @brief What the status line counts.
struct TableCountsView {
    /// @brief The view's rows.
    TableTotal view;
    /// @brief Rows before the filter, where the model knows it.
    std::optional<std::int64_t> source;
    /// @brief Rows the filter can neither admit nor reject until a level loads.
    std::int64_t undecided = 0;
    /// @brief A sort, filter or fetch is in flight.
    bool pending = false;
    /// @brief Levels the order still waits for.
    std::vector<std::string> awaiting;
    /// @brief Levels whose load failed for a row the table shows or orders by.
    std::vector<std::string> failed;
    /// @brief Held view rows that are retained and no longer match the filter.
    std::int64_t retained = 0;
    /// @brief Rows an action of this session is working on: `RowStates::busyCount`.
    std::int64_t busy = 0;
};

/// @brief What `TableRows::retain` did with the keys it was given.
enum class RetainResult : std::uint8_t {
    Retained,   ///< The keys are retained, those retained already included.
    OverBound,  ///< None is: they would take the set past `table::QueryLimits::maxRetained`.
};

/// @brief Where a key's row is in the view: its view index, nothing when the view has no row with the key, or why
///        that cannot be told.
using RowLocation = std::expected<std::optional<std::int64_t>, table::TableError>;

/// @brief A row `TableRows::fetchRows` fetched by its key: where it is in the view, and its cells.
struct FetchedRow {
    /// @brief The row's key.
    table::RowId key;
    /// @brief The row's view index under the current sort and filters.
    std::int64_t viewRow = 0;
    /// @brief One cell per column asked for, in that order; `table::Unloaded` for a cell of a level the row has not
    ///        loaded, empty for a column the rows do not have.
    std::vector<table::Cell> cells;
};

/// @brief The view a selection of the view is of: what the server resolves it against.
struct ViewSelection {
    /// @brief The view's filter, as the table queried it.
    table::FilterSpec filters;
    /// @brief The rows the view kept although they no longer match.
    std::vector<table::RowId> retained;
    /// @brief The view's `viewRevision` when the user selected it.
    std::string revision;

    /// @brief Member-wise equality.
    /// @param other The view to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(ViewSelection const& other) const = default;
};

/// @brief What a user selected in a table: exactly some keys, or every row of a view less some keys. Its JSON is
///        `table::RowSelection`'s (`selectionJsonOf`).
struct TableSelection {
    /// @brief Without `view`: exactly these rows.
    std::vector<table::RowId> keys;
    /// @brief With it: every row of that view, less `except`.
    std::optional<ViewSelection> view;
    /// @brief The keys a selection of the view leaves out.
    std::vector<table::RowId> except;

    /// @brief Member-wise equality.
    /// @param other The selection to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(TableSelection const& other) const = default;
};

/// @brief Whether a selected count is the count or an estimate; element value `exact` or `approximate`.
enum class CountKind : std::uint8_t {
    Exact,        ///< The count of the rows selected.
    Approximate,  ///< An estimate: the total is a lower bound, or a key left out is not accounted for.
};

/// @brief How many rows a selection holds.
struct SelectedCount {
    /// @brief The rows.
    std::int64_t rows = 0;
    /// @brief Whether `rows` is exact.
    CountKind kind = CountKind::Exact;

    /// @brief Member-wise equality.
    /// @param other The count to compare with.
    /// @return `true` when rows and kind match.
    [[nodiscard]] bool operator==(SelectedCount const& other) const = default;
};

/// @brief Why the view cannot be selected yet: its membership is still open.
struct SelectAllRefusal {
    /// @brief Rows the filter cannot decide until a level loads.
    std::int64_t undecided = 0;
    /// @brief The awaited levels a filtered column needs.
    std::vector<std::string> levels;
};

/// @brief The JSON of a selection, as an action's body carries it: `{"keys": […]}`, or `{"view": {…}, "except":
///        […]}` with the filter's default members left out.
/// @param selection The selection.
/// @return The JSON text.
[[nodiscard]] inline std::string selectionJsonOf(TableSelection const& selection) {
    return table::detail::selectionJson(
        selection.keys, selection.view ? &selection.view->filters : nullptr,
        selection.view ? std::span<table::RowId const>{selection.view->retained} : std::span<table::RowId const>{},
        selection.view ? std::string_view{selection.view->revision} : std::string_view{}, selection.except);
}

/// @brief What the status line says when select-all is refused.
/// @param refusal Why.
/// @param locale  The formatting facts: the count's group separator.
/// @return `"Select all is available once <n> rows are decided"`, or with no undecided row `"Select all is available
///         once <levels joined by " and "> data has loaded"`, or with neither `"Select all is available once the
///         rows have loaded"`: the view has not answered its current query yet.
[[nodiscard]] inline std::string selectAllRefusalText(SelectAllRefusal const& refusal, FormatContext const& locale) {
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

/// @brief The rows of one table an action's reply says it changed, and those it could not.
struct ChangedRowsValue {
    /// @brief The rows changed.
    std::vector<table::ChangedRow> rows;
    /// @brief The rows the action could not change, each with its message.
    std::vector<table::FailedRow> failed;

    /// @brief Member-wise equality.
    /// @param other The value to compare with.
    /// @return `true` when rows and failures match.
    [[nodiscard]] bool operator==(ChangedRowsValue const& other) const = default;
};

/// @brief Reads the `ChangedRows` of one table from an action's reply.
///
/// Each path the catalog gives for @p record names a `ChangedRows`' `rows` member: `"<member>.rows"`, or `"rows"`
/// for the result itself. The value at the path's parent is read as `{"rows": [{"key", "from"}], "failed": [{"key",
/// "message"}]}`; a number is an integer key and a string a text key, and an entry whose `key` is neither is
/// skipped, as is a `from` of neither.
/// @param reply  The action's reply.
/// @param action The action's catalog entry.
/// @param record The table's record, `<model type>.<id of the table>`.
/// @return The rows of every such path, in the reply's order; none when the catalog names no path for @p record.
[[nodiscard]] inline ChangedRowsValue changedRowsOf(Value const& reply, ActionInfo const& action,
                                                    std::string_view record) {
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
            Value const* at = &reply;
            while (at != nullptr && !parent.empty()) {
                auto const dot = parent.find('.');
                at = at->field(parent.substr(0, dot));
                parent = dot == std::string_view::npos ? std::string_view{} : parent.substr(dot + 1);
            }
            if (at == nullptr) {
                continue;
            }
            if (Value const* const rows = at->field("rows"); rows != nullptr && rows->asList() != nullptr) {
                for (Value const& row : *rows->asList()) {
                    if (auto key = keyOf(row.field("key"))) {
                        out.rows.push_back(
                            table::ChangedRow{.key = std::move(*key), .from = keyOf(row.field("from"))});
                    }
                }
            }
            if (Value const* const failed = at->field("failed"); failed != nullptr && failed->asList() != nullptr) {
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

/// @brief The rows of a table an action of this session is working on, and those an action could not change.
///
/// A run's rows are busy until it settles, whatever its outcome: the keys of a key selection or of a row, or every
/// row of a selection of the view but its `except`. Runs are kept apart, so two runs over one row keep it busy until
/// both settle. A failed row holds its message until its mark is cleared.
class RowStates {
public:
    /// @brief Makes rows busy for a run; a run named again adds to its keys.
    /// @param run  The run's id, unique in the session.
    /// @param keys The rows' keys.
    void busyKeys(std::uint64_t run, std::vector<table::RowId> keys) {
        std::vector<table::RowId>& held = _keyRuns[run];
        for (table::RowId& key : keys) {
            if (!std::ranges::contains(held, key)) {
                held.push_back(std::move(key));
            }
        }
    }

    /// @brief Makes every row of a selection of the view busy for a run, but its `except`.
    /// @param run    The run's id, unique in the session.
    /// @param except The keys the selection leaves out.
    /// @param count  The rows the selection held when the run started: what `busyCount` counts for it.
    void busyView(std::uint64_t run, std::vector<table::RowId> except, std::int64_t count) {
        _viewRuns.insert_or_assign(run,
                                   ViewRun{.except = std::move(except), .count = std::max<std::int64_t>(count, 0)});
    }

    /// @brief Ends a run: its rows are no longer busy for it, whatever its outcome.
    /// @param run The run's id; one that holds nothing is ignored.
    void settle(std::uint64_t run) {
        _keyRuns.erase(run);
        _viewRuns.erase(run);
    }

    /// @brief Marks rows an action could not change, each with its message; a row marked already takes the new one.
    /// @param rows The rows.
    void fail(std::span<table::FailedRow const> rows) {
        for (table::FailedRow const& row : rows) {
            auto const found = std::ranges::find(_failed, row.key, &table::FailedRow::key);
            if (found != _failed.end()) {
                found->message = row.message;
            } else {
                _failed.push_back(row);
            }
        }
    }

    /// @brief Drops the failed marks of some rows.
    /// @param keys The rows' keys.
    void clearFailed(std::span<table::RowId const> keys) {
        std::erase_if(_failed, [&keys](table::FailedRow const& row) { return std::ranges::contains(keys, row.key); });
    }

    /// @brief Drops every failed mark.
    void clearFailed() { _failed.clear(); }

    /// @brief Moves a row's busy membership and failed mark from its old key to its new one; where the new key has
    ///        its own, the new key's is kept and the old key's dropped.
    /// @param from The row's old key.
    /// @param to   The row's new key.
    void move(table::RowId const& from, table::RowId const& to) {
        if (from == to) {
            return;
        }
        for (auto& [run, keys] : _keyRuns) {
            auto const old = std::ranges::find(keys, from);
            if (old == keys.end()) {
                continue;
            }
            if (std::ranges::contains(keys, to)) {
                keys.erase(old);
            } else {
                *old = to;
            }
        }
        // A view run holds a row unless its `except` names it: a busy old key makes the new key busy, and is then
        // left out itself.
        for (auto& [run, view] : _viewRuns) {
            if (!std::ranges::contains(view.except, from)) {
                std::erase(view.except, to);
                view.except.push_back(from);
            }
        }
        auto const old = std::ranges::find(_failed, from, &table::FailedRow::key);
        if (old == _failed.end()) {
            return;
        }
        if (std::ranges::contains(_failed, to, &table::FailedRow::key)) {
            _failed.erase(old);
        } else {
            old->key = to;
        }
    }

    /// @brief Whether a row is busy: a key run holds it, or a view run does not leave it out.
    /// @param key The row's key.
    /// @return `true` while any run in flight works on it.
    [[nodiscard]] bool isBusy(table::RowId const& key) const {
        auto const holds = [&key](auto const& run) { return std::ranges::contains(run.second, key); };
        auto const leavesIn = [&key](auto const& run) { return !std::ranges::contains(run.second.except, key); };
        return std::ranges::any_of(_keyRuns, holds) || std::ranges::any_of(_viewRuns, leavesIn);
    }

    /// @brief A row's failed mark.
    /// @param key The row's key.
    /// @return Its message, or nothing for a row not marked.
    [[nodiscard]] std::optional<std::string> failure(table::RowId const& key) const {
        auto const found = std::ranges::find(_failed, key, &table::FailedRow::key);
        return found == _failed.end() ? std::nullopt : std::optional{found->message};
    }

    /// @brief How many rows are busy.
    /// @return With no view run in flight, the distinct keys of the key runs; with one, the largest `count` among
    ///         the view runs plus the distinct keys of key runs that every view run leaves out, which are outside its
    ///         rows.
    [[nodiscard]] std::int64_t busyCount() const {
        std::unordered_set<table::RowId> keys;
        for (auto const& [run, held] : _keyRuns) {
            for (table::RowId const& key : held) {
                auto const leavesOut = [&key](auto const& view) {
                    return std::ranges::contains(view.second.except, key);
                };
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

    /// @brief The rows marked failed.
    /// @return Their keys, in the order they failed.
    [[nodiscard]] std::vector<table::RowId> failedKeys() const {
        std::vector<table::RowId> out;
        out.reserve(_failed.size());
        for (table::FailedRow const& row : _failed) {
            out.push_back(row.key);
        }
        return out;
    }

private:
    // A selection of the view a run works on: the keys it leaves out, and the rows it held when the run started.
    struct ViewRun {
        std::vector<table::RowId> except;
        std::int64_t count = 0;
    };

    std::map<std::uint64_t, std::vector<table::RowId>> _keyRuns;
    std::map<std::uint64_t, ViewRun> _viewRuns;
    std::vector<table::FailedRow> _failed;
};

/// @brief What a table's mount reads, whatever serves the rows.
///
/// Every member is called on the runtime's owner.
class TableRows {
public:
    /// @brief A model with no rows of its own yet.
    TableRows() = default;
    TableRows(TableRows const&) = delete;
    TableRows& operator=(TableRows const&) = delete;
    TableRows(TableRows&&) = delete;
    TableRows& operator=(TableRows&&) = delete;
    virtual ~TableRows() = default;

    /// @brief The view's rows.
    /// @return The total.
    [[nodiscard]] virtual TableTotal total() const = 0;

    /// @brief The rows a mount realizes: the view's, and the rows a decorator adds outside it, such as summary rows,
    ///        which no count and no selection takes in. A decorator over such rows forwards this too, or the rows they
    ///        add are dropped.
    /// @return The total the mount positions rows by; `total()` for rows that add none.
    [[nodiscard]] virtual TableTotal mountedTotal() const { return total(); }

    /// @brief The row at a view index, or nothing while it is not fetched.
    /// @param index The view index.
    /// @return The row; its references hold until the next `onChange`.
    [[nodiscard]] virtual std::optional<TableRowRef> row(std::int64_t index) const = 0;

    /// @brief The levels not loaded for the row at a view index, and which of them failed.
    /// @param index The view index.
    /// @return Views into the model, valid until its next `onChange`; empty when the row is not held.
    [[nodiscard]] virtual table::UnloadedLevels unloadedLevels(std::int64_t index) const = 0;

    /// @brief Tells the model which rows and which columns are in or near view; a server-mode model fetches them.
    /// @param first   The first view row.
    /// @param count   The rows.
    /// @param columns The column ids the rows' records hold; empty means every column.
    virtual void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) = 0;

    /// @brief The keys of a range of view rows, for a range selection; completes when every key is known.
    /// @param first The first view row.
    /// @param count The rows.
    /// @return The keys in view order, or why they cannot be had.
    virtual async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t first,
                                                                                               std::int64_t count) = 0;

    /// @brief Finds the view index of the row with a key, under the current sort and filters, whether or not the row
    ///        is held: what focus follows its key by when a new order moves it away from the rows in view.
    /// @param key  The row's key.
    /// @param stop Once a stop is requested, @p done is not called.
    /// @param done Called once on the owner, never before this returns, with the row's view index, nothing when the
    ///        view has no row with @p key, or the error that kept the model from telling.
    virtual void locateRow(table::RowId key, std::stop_token stop, std::function<void(RowLocation const&)> done) = 0;

    /// @brief Fetches rows by key, whether or not they are held, with the cells of the columns named: what a copy
    ///        of rows the model does not hold reads. Nothing is held or cached for them.
    /// @param keys    The rows' keys.
    /// @param columns The column ids whose cells each row carries, in that order.
    /// @return The rows the view has, each with its view index, in no particular order; a key the view lacks is
    ///         left out. Delivered on the owner.
    virtual async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) = 0;

    /// @brief Sets the sort chain.
    /// @param sort The keys, most significant first; empty is the table's own order.
    /// @return The error that left the sort unchanged, or nothing.
    virtual std::expected<void, table::TableError> setSort(SortChain sort) = 0;

    /// @brief Sets the filter.
    /// @param filter The filter; an empty one shows every row.
    /// @return The error that left the filter unchanged, or nothing.
    virtual std::expected<void, table::TableError> setFilter(table::FilterSpec filter) = 0;

    /// @brief The distinct values of a column, or of a declared group's columns pooled, over the rows the other
    ///        filters leave: what a header's filter menu lists.
    /// @param request The column or group, the search, the date bucket and the page of values.
    /// @return The values, delivered on the owner; `Unavailable` when the rows cannot list values.
    virtual async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest request) = 0;

    /// @brief What the status line counts.
    /// @return The counts.
    [[nodiscard]] virtual TableCountsView counts() const = 0;

    /// @brief The view as a selection of it takes it: its filter, its retained rows and its revision.
    /// @return The view, or why it cannot be selected yet; rows that select by keys alone refuse with nothing
    ///         undecided and no level.
    [[nodiscard]] virtual std::expected<ViewSelection, SelectAllRefusal> selectView() const {
        return std::unexpected(SelectAllRefusal{});
    }

    /// @brief How many rows a selection holds, as far as the rows know.
    /// @param selection The selection.
    /// @return The count; for keys, their number, exact.
    [[nodiscard]] virtual SelectedCount selectedCount(TableSelection const& selection) const {
        return SelectedCount{.rows = static_cast<std::int64_t>(selection.keys.size()), .kind = CountKind::Exact};
    }

    /// @brief Whether a reply has shown that the view has no row with a key: a key of a selection's `except` that
    ///        left the view is dropped from it.
    /// @param key The key.
    /// @return `true` once a reply that asked for the key left it out, until one holds it again.
    [[nodiscard]] virtual bool leftView(table::RowId const& key) const {
        static_cast<void>(key);
        return false;
    }

    /// @brief Adds rows the view keeps although the filter rejects them: the rows an action of this session changed.
    ///        All of them are added, or, when they would take the set past `table::QueryLimits::maxRetained`, none.
    ///        A change of the filter clears them.
    /// @param keys The rows' keys; a key retained already counts once.
    /// @return `Retained`, or `OverBound` with the set unchanged. Rows that cannot retain keep nothing and answer
    ///         `Retained`.
    [[nodiscard]] virtual RetainResult retain(std::span<table::RowId const> keys) {
        static_cast<void>(keys);
        return RetainResult::Retained;
    }

    /// @brief Drops every retained row: the view holds the rows the filter admits again.
    virtual void clearRetained() {}

    /// @brief Whether a held row is retained and the filter rejects it.
    /// @param key The row's key; a row's look has its key, not its view index.
    /// @return `true` for a held row the latest reply that supplied it listed as unmatched; `false` for a key the
    ///         rows do not hold.
    [[nodiscard]] virtual bool unmatched(table::RowId const& key) const {
        static_cast<void>(key);
        return false;
    }

    /// @brief Holds back the refetches the table's change events cause while an action of this session that changes
    ///        the table runs, so a row it changed is fetched with the retained rows its reply adds, never before.
    ///        The hold lasts while any run holds, for at most `SessionOptions::retainHold` from the first.
    /// @param run The run's id.
    virtual void holdRefetches(std::uint64_t run) { static_cast<void>(run); }

    /// @brief Ends a run's hold: when no run holds any more, what the events marked is fetched, as one refetch.
    /// @param run The run's id, as `holdRefetches` took it; one that holds nothing is ignored.
    virtual void releaseRefetches(std::uint64_t run) { static_cast<void>(run); }

    /// @brief Fetches a range of view rows with the cells of the columns named, whether or not they are held: what
    ///        a copy of a selection of the view reads. Nothing is held or cached for them.
    /// @param first   The first view row.
    /// @param count   The rows.
    /// @param columns The column ids whose cells each row carries, in that order.
    /// @return One row per view row of the range the view has, in view order, delivered on the owner; none from
    ///         rows that cannot read a range.
    virtual async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> rowsOf(
        std::int64_t first, std::int64_t count, std::span<std::string const> columns) {
        static_cast<void>(first);
        static_cast<void>(count);
        static_cast<void>(columns);
        return fetchRows({}, {});
    }

    /// @brief The view rows the model holds now, as `{first, count}` ranges in view order: what a lookup by key reads,
    ///        so it is bounded by what is held and not by the view.
    /// @return The ranges; every view row for rows that hold them all.
    [[nodiscard]] virtual std::vector<std::pair<std::int64_t, std::int64_t>> heldRanges() const {
        return {{0, total().rows}};
    }

    /// @brief The rows an action of this session is working on, and those an action could not change.
    /// @return The rows' states.
    [[nodiscard]] virtual RowStates& states() = 0;

    /// @brief The rows an action of this session is working on, and those an action could not change.
    /// @return The rows' states.
    [[nodiscard]] virtual RowStates const& states() const = 0;

    /// @brief Moves what the rows hold by a row's key to its new key: its place among the retained rows and its
    ///        states, as `RowStates::move` moves them. An action that gave a row a new key names the old one in a
    ///        `table::ChangedRow::from`.
    /// @param from The row's old key.
    /// @param to   The row's new key.
    virtual void moveKey(table::RowId const& from, table::RowId const& to) = 0;

    /// @brief Sets the one receiver of the view's changes, called after the rows changed with the operations that
    ///        turn the old view into the new one.
    ///
    /// A single slot: a call replaces the handler, and the mount is what sets it. Anything else that needs the
    /// changes wraps this `TableRows` in a forwarding decorator and hands the wrapper to the mount.
    /// @param handler The receiver; empty to stop receiving.
    virtual void onChange(std::function<void(table::ViewChange const&)> handler) = 0;
};

/// @brief Why a row is locked: its `rowLock` companion's text.
/// @param row         The row.
/// @param description The table's description; a `rowLock` that is empty locks no row.
/// @return The companion's text when it is non-empty text, else nothing.
[[nodiscard]] inline std::optional<std::string> lockText(TableRowRef const& row,
                                                         table::TableDescription const& description) {
    if (description.rowLock.empty()) {
        return std::nullopt;
    }
    Value const* const cell = row.row.field(description.rowLock);
    std::string const* const text = cell == nullptr ? nullptr : cell->asString();
    return text == nullptr || text->empty() ? std::nullopt : std::optional{*text};
}

/// @brief Whether a row takes no cell edit: an action of this session is working on it, or it is locked.
/// @param rows        The rows.
/// @param index       The row's view index.
/// @param description The table's description.
/// @return `true` for a busy or locked row; `false` for a row the rows do not hold.
[[nodiscard]] inline bool refusesEdit(TableRows const& rows, std::int64_t index,
                                      table::TableDescription const& description) {
    std::optional<TableRowRef> const row = rows.row(index);
    return row && (rows.states().isBusy(row->key) || lockText(*row, description).has_value());
}

/// @brief A row's key and its record, owned.
struct RowRecord {
    /// @brief The row's key.
    table::RowId key;
    /// @brief The row as a record keyed by column id.
    Value row;
};

/// @brief What `EngineRows` reads a row's key and record through: its source's snapshot and a source row.
using RecordOf = std::function<RowRecord(table::RowSnapshot const& snapshot, std::size_t sourceRow)>;

namespace detail {

/// @brief The site names a misuse of a table's rows is reported under, through the runtime's owner probe.
namespace site {
/// @brief A ForEach, Table or rows-source snapshot repeated a key; the later row is refused and the first one kept.
///        Declared here, and not beside the mount's other sites, because `mount.hpp` includes this header.
inline constexpr char const* kDuplicateKey = "morph::ui: duplicate ForEach key";
}  // namespace site

/// @brief A `double` as the record holds it: the decimal its shortest fixed-notation text spells, or that text's
///        shortest form when it spells no decimal `parseDecimal` reads, so the value is never lost to `null`.
/// @param value The number.
/// @return A `Decimal`, or the `String` of `std::to_chars`'s shortest text (`"1e+308"`, `"inf"`).
[[nodiscard]] inline Value doubleValue(double value) {
    if (std::isfinite(value)) {
        // The longest fixed text of a finite double: 309 integer digits, a sign, a point and 767 fraction digits.
        std::array<char, 1100> fixed{};
        auto const written =
            std::to_chars(fixed.data(), fixed.data() + fixed.size(), value, std::chars_format::fixed);
        if (written.ec == std::errc{}) {
            if (auto const parsed = table::parseDecimal(std::string_view{fixed.data(), written.ptr})) {
                return Value{Decimal{*parsed}};
            }
        }
    }
    std::array<char, 64> shortest{};
    auto const written = std::to_chars(shortest.data(), shortest.data() + shortest.size(), value);
    return Value{std::string{shortest.data(), written.ptr}};
}

/// @brief A `Date` cell's days since 1970-01-01 as `YYYY-MM-DD`.
/// @param days The days.
/// @return The date's text; the number's own text past the years 0 to 9999.
[[nodiscard]] inline Value dateValue(std::int64_t days) {
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

/// @brief A `DateTime` cell's seconds since the epoch as a timestamp.
/// @param seconds The seconds.
/// @return The timestamp; the number's own text when its milliseconds do not fit an `int64`.
[[nodiscard]] inline Value dateTimeValue(std::int64_t seconds) {
    constexpr std::int64_t kMillis = 1000;
    if (seconds > std::numeric_limits<std::int64_t>::max() / kMillis ||
        seconds < std::numeric_limits<std::int64_t>::min() / kMillis) {
        return Value{std::to_string(seconds)};
    }
    return Value{time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{seconds * kMillis}}}};
}

}  // namespace detail

/// @brief A cell as the document reads it.
///
/// Empty and unloaded cells are `null`. An integer is an `Int`, a `Date` column's the `String` `YYYY-MM-DD` and a
/// `DateTime` column's a `Timestamp`. A `Rational` is a `Decimal` with its places. A `double` is the `Decimal` its
/// shortest fixed-notation text spells, or that text's shortest form as a `String` when it spells none
/// `table::parseDecimal` reads (non-finite, more than 18 fraction digits, beyond an `int64` numerator). Text is a
/// `String`, a flag a `Bool`, and a quantity the `Decimal` of its amount in the column's canonical unit. A measure is a
/// `Quantity` of its amount in its own unit, named by the unit's id in @p units; its amount alone, a `Decimal`, when
/// @p units is null or does not hold the unit.
/// @param cell   The cell.
/// @param column The cell's column, whose kind decides how an integer reads.
/// @param units  The registry a measure's unit number is named through; null for none.
/// @return The value.
[[nodiscard]] inline Value cellValue(table::Cell const& cell, table::ColumnInfo const& column,
                                     table::UnitRegistry const* units = nullptr) {
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
            } else if constexpr (std::is_same_v<T, std::string>) {
                return Value{value};
            } else if constexpr (std::is_same_v<T, bool>) {
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

/// @brief The status line's counts part, its numbers grouped with @p format's separator.
///
/// Nothing while no total is known; `"<view> so far, <undecided> loading"` while rows are undecided;
/// `"<selected> of <view> selected"` with a selection; `"<view> or more"` under a lower bound; otherwise `"1 row"` or
/// `"<view> rows"`. A lower bound's `<view>` is followed by `" or more"` in the selection's text too.
/// @param counts   The model's counts.
/// @param selected Rows selected.
/// @param format   The formatting facts: their numbers' group separator.
/// @return The text.
[[nodiscard]] inline std::string countsText(TableCountsView const& counts, std::int64_t selected,
                                            FormatContext const& format) {
    if (!counts.view.known) {
        return {};
    }
    auto const grouped = [&format](std::int64_t number) {
        return render::formatCanonicalNumber(std::to_string(number), format.numeric);
    };
    bool const lowerBound = counts.view.kind == table::TotalKind::AtLeast;
    std::string const view = grouped(counts.view.rows) + (lowerBound ? " or more" : "");
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

// ── Over the engine ─────────────────────────────────────────────────────────

/// @brief A table's rows over a `table::Engine`: every row is present.
///
/// `visible` does nothing and `keysOf` completes at once; a change is the engine's `ViewChange`. A row's record is
/// built by `RecordOf` once per source row of the engine's `viewSnapshot()` and kept: dropped for the rows an
/// `Updated` change names, and whole after any other change of the source, when the engine's view first shows it.
/// The view's snapshot is matched to the change that made it by pointer, so a source's `snapshot()` must return the
/// same pointer until its rows change, as `VectorSource` does; with a source that returns a new one on every call,
/// every logged change is applied whenever the view's snapshot moves.
class EngineRows final : public TableRows, private table::ChangeListener {
public:
    /// @brief Rows over a new engine.
    /// @param source   The rows. Must not be null.
    /// @param recordOf What builds a row's key and record.
    /// @param options  How the engine runs. `owner` must not be null: `keysOf` delivers there.
    /// @throws std::invalid_argument when @p source or `options.owner` is null.
    EngineRows(std::shared_ptr<table::DataSource> source, RecordOf recordOf, table::EngineOptions options)
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

    EngineRows(EngineRows const&) = delete;
    EngineRows& operator=(EngineRows const&) = delete;
    EngineRows(EngineRows&&) = delete;
    EngineRows& operator=(EngineRows&&) = delete;

    /// @brief Stops hearing the engine and the source.
    ~EngineRows() override {
        _engine->onViewChange({});
        _source->unsubscribe(*this);
    }

    /// @brief The engine.
    /// @return It.
    [[nodiscard]] table::Engine& engine() noexcept { return *_engine; }

    /// @brief Calls of `RecordOf` so far: the memo's measure.
    /// @return The count.
    [[nodiscard]] std::size_t recordsBuilt() const noexcept { return _built; }

    /// @brief Holds the rows back while a table's stored arrangement is read, or lets them through.
    ///
    /// While restoring, `total()` is the view's rows, not known, and `row` answers nothing, so a mount draws
    /// placeholder rows in place of an order the stored state is about to change; the engine still runs. A change of
    /// it tells the receiver of `onChange` that the whole view changed.
    /// @param restoring Whether the rows are held back.
    void setRestoring(bool restoring) {
        if (std::exchange(_restoring, restoring) == restoring) {
            return;
        }
        if (_onChange) {
            table::ViewChange change;
            change.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
            _onChange(change);
        }
    }

    /// @brief Whether the rows are held back (`setRestoring`).
    /// @return Whether they are.
    [[nodiscard]] bool restoring() const noexcept { return _restoring; }

    /// @brief Every row of the view, counted exactly; not known while restoring.
    /// @return The total.
    [[nodiscard]] TableTotal total() const override {
        if (_restoring) {
            return TableTotal{.rows = static_cast<std::int64_t>(_engine->viewRowCount()),
                              .kind = table::TotalKind::Exact,
                              .known = false};
        }
        return TableTotal{.rows = static_cast<std::int64_t>(_engine->viewRowCount()),
                          .kind = table::TotalKind::Exact,
                          .known = true};
    }

    /// @brief The row at a view index, built once and kept until it changes.
    /// @param index The view index.
    /// @return The row; nothing past the view, and nothing while restoring.
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override {
        if (_restoring) {
            return std::nullopt;
        }
        auto const* const entry = entryAt(index);
        if (entry == nullptr) {
            return std::nullopt;
        }
        return TableRowRef{.key = entry->record.key, .row = entry->record.row};
    }

    /// @brief The levels of the row's columns that hold an unloaded cell.
    /// @param index The view index.
    /// @return The levels, sorted; none failed, since a source's level has no failure of its own.
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override {
        auto const* const entry = entryAt(index);
        if (entry == nullptr) {
            return {};
        }
        return table::UnloadedLevels{.levels = entry->levels, .failed = {}};
    }

    /// @brief Does nothing: every row is present.
    /// @param first   Unused.
    /// @param count   Unused.
    /// @param columns Unused.
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        static_cast<void>(first);
        static_cast<void>(count);
        static_cast<void>(columns);
    }

    /// @brief The keys of a range of view rows, at once.
    /// @param first The first view row.
    /// @param count The rows.
    /// @return The keys, delivered on the engine's owner; `InvalidSpec` for a range outside the view.
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(
        std::int64_t first, std::int64_t count) override {
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

    /// @brief The rows of keys in the engine's view, read from its snapshot at once.
    /// @param keys    The rows' keys.
    /// @param columns The column ids whose cells each row carries; a column the engine lacks gives an empty cell.
    /// @return The rows the view has, in the order of @p keys, delivered on the engine's owner.
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) override {
        using Result = std::expected<std::vector<FetchedRow>, table::TableError>;
        auto [completion, promise] = async::Completion<Result>::makeSettleable(_owner);
        auto const infos = _engine->columns();
        std::vector<std::optional<std::size_t>> places;
        places.reserve(columns.size());
        for (std::string const& id : columns) {
            auto const found = std::ranges::find(infos, id, &table::ColumnInfo::id);
            places.push_back(found == infos.end() ? std::nullopt
                                                  : std::optional{static_cast<std::size_t>(found - infos.begin())});
        }
        auto const& snapshot = *_engine->viewSnapshot();
        std::vector<FetchedRow> rows;
        for (table::RowId& key : keys) {
            std::optional<std::size_t> const at = _engine->viewRowOfKey(key);
            std::optional<std::size_t> const sourceRow = at ? _engine->sourceRowOf(*at) : std::nullopt;
            if (!sourceRow) {
                continue;
            }
            FetchedRow row{.key = std::move(key), .viewRow = static_cast<std::int64_t>(*at), .cells = {}};
            row.cells.reserve(places.size());
            for (std::optional<std::size_t> const place : places) {
                row.cells.push_back(place ? snapshot.cell(*sourceRow, *place) : table::Cell{});
            }
            rows.push_back(std::move(row));
        }
        promise.resolve(Result{std::move(rows)});
        return std::move(completion);
    }

    /// @brief The engine's view as a selection takes it. A table held whole selects all by keys, so the lowering
    ///        does not ask; the answer is the engine's filter and the digest of its view's keys.
    /// @return The view; never refused, since every row is decided.
    [[nodiscard]] std::expected<ViewSelection, SelectAllRefusal> selectView() const override {
        table::MembershipDigest digest;
        for (std::size_t at = 0; at < _engine->viewRowCount(); ++at) {
            digest.add(_engine->rowIdAt(at));
        }
        auto const retained = _engine->retained();
        return ViewSelection{.filters = _engine->filter(),
                             .retained = {retained.begin(), retained.end()},
                             .revision = digest.text()};
    }

    /// @brief How many rows a selection holds: every row is here, so the count is exact.
    /// @param selection The selection.
    /// @return The keys' number, or the view's rows less the keys of `except` in it.
    [[nodiscard]] SelectedCount selectedCount(TableSelection const& selection) const override {
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

    /// @brief A range of view rows, read from the engine at once.
    /// @param first   The first view row.
    /// @param count   The rows.
    /// @param columns The column ids whose cells each row carries.
    /// @return The rows in view order, delivered on the engine's owner; the part of the range outside the view has
    ///         none.
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> rowsOf(
        std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        auto const rows = static_cast<std::int64_t>(_engine->viewRowCount());
        std::vector<table::RowId> keys;
        for (std::int64_t at = std::max<std::int64_t>(first, 0); at < std::min(rows, first + count); ++at) {
            keys.push_back(_engine->rowIdAt(static_cast<std::size_t>(at)));
        }
        return fetchRows(std::move(keys), std::vector<std::string>(columns.begin(), columns.end()));
    }

    /// @brief Finds the view index of the row with a key in the engine's view, as it is now.
    /// @param key  The row's key.
    /// @param stop Once a stop is requested, @p done is not called.
    /// @param done Called on the engine's owner with the row's view index, or nothing when the view lacks it.
    void locateRow(table::RowId key, std::stop_token stop, std::function<void(RowLocation const&)> done) override {
        std::optional<std::size_t> const at = _engine->viewRowOfKey(key);
        RowLocation const found{at ? std::optional{static_cast<std::int64_t>(*at)} : std::nullopt};
        _owner->post([stop = std::move(stop), done = std::move(done), found] {
            if (!stop.stop_requested()) {
                done(found);
            }
        });
    }

    /// @brief Sets the engine's sort.
    /// @param sort The keys.
    /// @return The engine's error, or nothing.
    std::expected<void, table::TableError> setSort(SortChain sort) override { return _engine->setSort(std::move(sort)); }

    /// @brief Sets the engine's filter.
    /// @param filter The filter.
    /// @return The engine's error, or nothing.
    std::expected<void, table::TableError> setFilter(table::FilterSpec filter) override {
        return _engine->setFilter(std::move(filter));
    }

    /// @brief The distinct values of a column, from the engine's rows, in a task on the owner: a change of the
    ///        source made before the call (a column put in use to be listed) is taken in first.
    /// @param request The request.
    /// @return The engine's answer, delivered on its owner.
    async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest request) override {
        using Result = std::expected<table::DistinctValues, table::TableError>;
        auto [completion, promise] = async::Completion<Result>::makeSettleable(_owner);
        auto settle = std::make_shared<async::Completion<Result>::Promise>(std::move(promise));
        _owner->post(_scope.token().guard([this, settle, request = std::move(request)] {
            settle->resolve(_engine->distinctValues(request));
        }));
        return std::move(completion);
    }

    /// @brief The engine's counts: its view, its source's rows, its undecided rows and whether it computes.
    /// @return The counts.
    [[nodiscard]] TableCountsView counts() const override {
        std::int64_t retained = 0;
        for (table::RowId const& key : _engine->retained()) {
            if (auto const at = _engine->viewRowOfKey(key); at && _engine->isUnmatched(*at)) {
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

    /// @brief Adds keys to the engine's retained rows, all or none.
    /// @param keys The rows' keys.
    /// @return `OverBound` when they would take the set past `table::QueryLimits::maxRetained`.
    [[nodiscard]] RetainResult retain(std::span<table::RowId const> keys) override {
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
            static_cast<void>(_engine->retain(std::move(next)));
        }
        return RetainResult::Retained;
    }

    /// @brief Drops the engine's retained rows.
    void clearRetained() override {
        if (!_engine->retained().empty()) {
            static_cast<void>(_engine->retain({}));
        }
    }

    /// @brief Whether the engine's view row of a key is retained and rejected by the filter.
    /// @param key The row's key.
    /// @return `false` for a key not in the view.
    [[nodiscard]] bool unmatched(table::RowId const& key) const override {
        auto const at = _engine->viewRowOfKey(key);
        return at && _engine->isUnmatched(*at);
    }

    /// @brief The rows an action of this session is working on, and those an action could not change.
    /// @return The rows' states.
    [[nodiscard]] RowStates& states() override { return _states; }

    /// @brief The rows an action of this session is working on, and those an action could not change.
    /// @return The rows' states.
    [[nodiscard]] RowStates const& states() const override { return _states; }

    /// @brief Moves a row's place among the engine's retained rows and its states to its new key.
    /// @param from The row's old key.
    /// @param to   The row's new key.
    void moveKey(table::RowId const& from, table::RowId const& to) override {
        _states.move(from, to);
        auto const held = _engine->retained();
        if (!std::ranges::contains(held, from)) {
            return;
        }
        std::vector<table::RowId> next;
        next.reserve(held.size());
        bool const toHeld = std::ranges::contains(held, to);
        for (table::RowId const& key : held) {
            if (key == from) {
                if (!toHeld) {
                    next.push_back(to);
                }
            } else {
                next.push_back(key);
            }
        }
        static_cast<void>(_engine->retain(std::move(next)));
    }

    /// @brief Sets the one receiver of the engine's view changes.
    /// @param handler The receiver; empty to stop receiving.
    void onChange(std::function<void(table::ViewChange const&)> handler) override { _onChange = std::move(handler); }

private:
    // One row's record and its unloaded levels, for one source row.
    struct Entry {
        RowRecord record;
        std::vector<std::string> levels;
    };
    RowStates _states;

    // One change of the source: the snapshot it made, and the rows it touched or the whole source.
    struct Logged {
        std::shared_ptr<table::RowSnapshot const> snapshot;
        bool whole = false;
        std::vector<std::size_t> rows;
    };

    [[nodiscard]] static std::shared_ptr<table::DataSource> checked(std::shared_ptr<table::DataSource> source,
                                                                    table::EngineOptions const& options) {
        if (source == nullptr || options.owner == nullptr) {
            throw std::invalid_argument{"morph::ui::EngineRows: the source and the engine's owner must not be null"};
        }
        return source;
    }

    void rowsChanged(table::RowChange const& change) override {
        _log.push_back(Logged{.snapshot = _source->snapshot(),
                              .whole = change.kind != table::ChangeKind::Updated,
                              .rows = change.rows});
    }

    // Brings the memo to the engine's view snapshot: every change up to the one that made it is applied. The
    // changes after it are kept, since a record built now reads the view's snapshot, which they are not in yet.
    void sync() const {
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

    [[nodiscard]] Entry const* entryAt(std::int64_t index) const {
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
            if (!columns[column].level.empty() &&
                std::holds_alternative<table::Unloaded>(snapshot.cell(*sourceRow, column))) {
                entry.levels.push_back(columns[column].level);
            }
        }
        std::ranges::sort(entry.levels);
        auto const [first, last] = std::ranges::unique(entry.levels);
        entry.levels.erase(first, last);
        return &_memo.insert_or_assign(*sourceRow, std::move(entry)).first->second;
    }

    std::shared_ptr<table::DataSource> _source;
    RecordOf _recordOf;
    exec::IExecutor* _owner;
    std::unique_ptr<table::Engine> _engine;
    std::function<void(table::ViewChange const&)> _onChange;
    mutable std::unordered_map<std::size_t, Entry> _memo;
    mutable std::shared_ptr<table::RowSnapshot const> _memoSnapshot;
    mutable std::deque<Logged> _log;
    mutable std::size_t _built = 0;
    bool _restoring = false;
    /// Last, so it is the first to go: no values list reaches a destroyed engine.
    async::CallbackScope _scope;
};

// ── A document's rows as a source ───────────────────────────────────────────

/// @brief The expression language's text order: the default collator's fold, then the text itself.
///
/// Texts that fold alike order by their own bytes, so the order is total and is the one the expression language's
/// `sort` gives text. A text is folded once for both its sort key and its `fold`: the engine asks for one and then
/// the other of the same text, and each thread keeps the last text it folded. Safe to call concurrently:
/// `cloneForTask` returns null.
class ValueRowsCollator final : public table::TextCollator {
public:
    /// @brief The fold of @p text, a NUL, then @p text: case-folded order first, then the text's own bytes.
    /// @param text UTF-8 text.
    /// @return The collation key.
    [[nodiscard]] std::string sortKey(std::string_view text) const override {
        std::string key = folded(text);
        key.push_back('\0');
        key.append(text);
        return key;
    }

    /// @brief @p text folded as the default collator folds it, which filters match against.
    /// @param text UTF-8 text.
    /// @return The folded text.
    [[nodiscard]] std::string fold(std::string_view text) const override { return folded(text); }

    /// @brief Texts folded so far, by `sortKey` and `fold` together.
    /// @return The count.
    [[nodiscard]] std::size_t folds() const noexcept { return _folds.load(std::memory_order_relaxed); }

private:
    // The fold of a text: the one this thread made last when it was of the same text, else a new one, counted.
    [[nodiscard]] std::string folded(std::string_view text) const {
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

    mutable std::atomic<std::size_t> _folds{0};
};

/// @brief One column of a `rows` table, as its source reads it.
struct ValueColumn {
    /// @brief The column's id: the row field it reads when it has no `sortBy`.
    std::string id;
    /// @brief The column part's literal `kind`; empty to infer the kind from the cells.
    std::optional<table::ColumnKind> kind;
    /// @brief The column's `sortBy` evaluated in the row's scope, read tracked; empty reads the field `id`.
    std::function<Value(Value const& row)> sortBy;
};

/// @brief The hidden leading sort key of a grouped rows table: an `Integer` column of each row's run index.
///
/// It begins with U+001F, a character no document name holds, so no column a document declares can share its id.
inline constexpr std::string_view kGroupRunColumn = "\x1F"
                                                    "group";

namespace detail {

/// @brief What one non-null value says a column's kind is.
enum class ValueVote : std::uint8_t {
    None,      ///< Nothing: a `null`, a list or a record that is not a measure.
    Bool,      ///< A `Bool` column.
    Integer,   ///< An `Integer` column.
    Decimal,   ///< A `Decimal` column.
    Text,      ///< A `Text` column.
    DateTime,  ///< A `DateTime` column.
    Measure,   ///< A `Measure` column.
};

/// @brief How many `ValueVote`s there are.
inline constexpr std::size_t kValueVotes = 7;

/// @brief An amount and its unit's id, as a quantity or a `{num, den, unit}` record holds them.
struct MeasureParts {
    /// @brief The amount.
    math::Rational amount;
    /// @brief The unit's id; points into the value read.
    std::string const* unit = nullptr;
};

/// @brief The places a fraction with denominator @p den is written to exactly, or the most a decimal holds.
/// @param den The denominator, positive.
/// @return The places.
[[nodiscard]] inline std::uint32_t exactPlacesOf(std::int64_t den) {
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

/// @brief A quantity, or a record of exactly `num`, `den` and `unit` (integers and a text), read as a measure.
/// @param value The value.
/// @return Its amount and unit, or nothing for any other value.
[[nodiscard]] inline std::optional<MeasureParts> measureOf(Value const& value) {
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

/// @brief The kind a value votes its column.
/// @param value The value.
/// @return The vote; `None` for `null`, a list and a record that is not a measure.
[[nodiscard]] inline ValueVote voteOf(Value const& value) {
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

/// @brief The column kind a vote names.
/// @param vote A vote other than `None`.
/// @return The kind.
[[nodiscard]] constexpr table::ColumnKind kindOfVote(ValueVote vote) noexcept {
    switch (vote) {
        case ValueVote::Bool:
            return table::ColumnKind::Bool;
        case ValueVote::Integer:
            return table::ColumnKind::Integer;
        case ValueVote::Decimal:
            return table::ColumnKind::Decimal;
        case ValueVote::DateTime:
            return table::ColumnKind::DateTime;
        case ValueVote::Measure:
            return table::ColumnKind::Measure;
        case ValueVote::Text:
        case ValueVote::None:
        default:
            return table::ColumnKind::Text;
    }
}

/// @brief A column's kind from its cells' votes.
///
/// No vote is `Text`; votes all alike are their kind; integers and decimals together are `Decimal`, an integer being
/// an exact decimal. Any other mix takes the first non-null cell's kind, `Decimal` when that is an integer and a
/// decimal voted too.
/// @param counts The votes of each kind.
/// @param votes  Each row's vote, in source order.
/// @return The kind.
[[nodiscard]] inline table::ColumnKind inferredKind(std::array<std::size_t, kValueVotes> const& counts,
                                                    std::span<ValueVote const> votes) {
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

/// @brief A cell of a column and whether it is not of the column's kind.
struct ConvertedCell {
    /// @brief The cell.
    table::Cell cell;
    /// @brief The value is not of the column's kind: the cell sorts last.
    bool invalid = false;
};

/// @brief @p value as a cell of a column of @p kind.
///
/// `null` is an empty cell. A value not of the kind is invalid: an empty cell in a `Text` column, which reads any
/// other cell as text, and elsewhere a measure in no unit, which every other kind reads as invalid. A timestamp is
/// UTC seconds rounded down (a `Date`'s days, rounded down); an integer in a `Decimal` column is a decimal of 0
/// places; under `Number` an integer or a decimal is the nearest `double`; a measure is its amount in its unit's
/// number in @p units, invalid when @p units is null or lacks the unit.
/// @param value The value.
/// @param kind  The column's kind.
/// @param units The registry a measure's unit is found in; null for none.
/// @return The cell.
[[nodiscard]] inline ConvertedCell convertedCell(Value const& value, table::ColumnKind kind,
                                                 table::UnitRegistry const* units) {
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

/// @brief Whether two cells are the same, a decimal's places included.
/// @param left  A cell.
/// @param right Another.
/// @return `true` when they hold the same value with the same places.
[[nodiscard]] inline bool sameCell(table::Cell const& left, table::Cell const& right) {
    auto const samePlaces = [](math::Rational const& a, math::Rational const& b) {
        return a.decimalPlaces.value == b.decimalPlaces.value;
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

/// @brief Whether two row values are one object: a record or a list by its storage, any other value by equality.
/// @param left  A value.
/// @param right Another.
/// @return `true` when nothing in @p right can differ from @p left.
[[nodiscard]] inline bool sameObject(Value const& left, Value const& right) {
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

/// @brief The rows of a `ValueRowsSource` at one moment: each row's value and key, and the cells of the columns in
///        use, shared with the source until it changes them.
class ValueRowsSnapshot final : public table::RowSnapshot {
public:
    /// @brief A snapshot over shared storage.
    /// @param rows    Each row's value.
    /// @param keys    Each row's key.
    /// @param columns Each column's cells; null for a column not in use, whose cells are empty.
    ValueRowsSnapshot(std::shared_ptr<std::vector<Value> const> rows, std::shared_ptr<std::vector<Key> const> keys,
                      std::vector<std::shared_ptr<std::vector<table::Cell> const>> columns)
        : _rows{std::move(rows)}, _keys{std::move(keys)}, _columns{std::move(columns)} {}

    /// @brief The rows.
    /// @return Their count.
    [[nodiscard]] std::size_t rowCount() const override { return _keys->size(); }

    /// @brief A row's key.
    /// @param row Row index.
    /// @return The key.
    [[nodiscard]] table::RowId rowId(std::size_t row) const override { return _keys->at(row); }

    /// @brief One cell.
    /// @param row    Row index.
    /// @param column Column index.
    /// @return The cell; empty for a column not in use.
    [[nodiscard]] table::Cell cell(std::size_t row, std::size_t column) const override {
        if (column >= _columns.size() || _columns[column] == nullptr) {
            return table::Cell{};
        }
        return _columns[column]->at(row);
    }

    /// @brief Hands rows `[first, last)` of @p column to @p sink in one run.
    /// @param column Column index.
    /// @param sink   Receives the cells.
    /// @param first  First row.
    /// @param last   One past the last row.
    void readColumn(std::size_t column, table::ColumnSink& sink, std::size_t first, std::size_t last) const override {
        if (column >= _columns.size() || _columns[column] == nullptr || first >= last) {
            RowSnapshot::readColumn(column, sink, first, last);
            return;
        }
        sink.cells(first, std::span<table::Cell const>{*_columns[column]}.subspan(first, last - first));
    }

    /// @brief A row's value.
    /// @param row Row index.
    /// @return The value the row was given.
    [[nodiscard]] Value const& row(std::size_t row) const { return _rows->at(row); }

private:
    std::shared_ptr<std::vector<Value> const> _rows;
    std::shared_ptr<std::vector<Key> const> _keys;
    std::vector<std::shared_ptr<std::vector<table::Cell> const>> _columns;
};

}  // namespace detail

/// @brief A `table::DataSource` over a document's list of values: what a `rows` table sorts and filters through the
///        engine.
///
/// A column holds cells only while it is in use (`use`, `unuse`); one no sort, filter or values list reads holds
/// none. A cell is the column's `sortBy` of the row, or the row's field of the column's id, converted to the
/// column's kind: a literal `kind`, or the kind its cells vote for (see `detail::inferredKind`). A `sortBy` is read in
/// a tracked computation per row: a change to what one row's reads notifies `Updated` for that row, and a change that
/// every row's reads is a `Reset`.
///
/// `assign` matches the new list to the held one by key: a row whose value is the held object is kept unread, any
/// other has its in-use cells derived and compared with the held ones. It is `Updated` when a cell differs, and also
/// when only a field no column in use reads differs, so what the table shows of it is read again; its keys are the
/// same, so the engine repairs it in place. New keys are `Inserted`, missing keys `Removed`, and a list that both
/// adds and drops rows, or reorders them, is a `Reset`. So is a change of a column's kind, and of a grouped table's
/// runs. A later row with a key already seen is dropped and reported (`detail::site::kDuplicateKey`), once per
/// duplicate.
///
/// Grouped, adjacent rows are one run while their `by` is the same element as the run's first row's
/// (`detail::sameElement`, the rule of the interpreter's `group` and of `runs`). A run whose heading record changed
/// while the runs kept their places is an `Updated` of its first source row.
///
/// `snapshot()` returns one pointer until the rows or cells change. Every member is called on the runtime's owner.
class ValueRowsSource final : public table::DataSource {
public:
    /// @brief A source of no rows over @p columns.
    /// @param runtime The runtime each `sortBy` is tracked on and duplicates are reported to; must outlive the source.
    /// @param columns The document's columns, each id once.
    /// @param keyOf   Reads a row's key.
    /// @param units   The registry `Quantity` cells' units are found in; null: none, and every quantity is invalid.
    ValueRowsSource(reactive::Runtime& runtime, std::vector<ValueColumn> columns, std::function<Key(Value const& row)> keyOf,
                    std::shared_ptr<table::UnitRegistry const> units = {})
        : _runtime{&runtime},
          _columns{std::move(columns)},
          _keyOf{std::move(keyOf)},
          _units{std::move(units)},
          _held(_columns.size()) {
        _infos.reserve(_columns.size() + 1);
        for (auto const& column : _columns) {
            _infos.push_back(table::ColumnInfo{
                .id = column.id, .kind = column.kind.value_or(table::ColumnKind::Text), .comparator = {},
                .format = {}, .level = {}, .dimensions = {}});
        }
    }

    ValueRowsSource(ValueRowsSource const&) = delete;
    ValueRowsSource& operator=(ValueRowsSource const&) = delete;
    ValueRowsSource(ValueRowsSource&&) = delete;
    ValueRowsSource& operator=(ValueRowsSource&&) = delete;
    /// @brief Ends every tracked `sortBy`.
    ~ValueRowsSource() override = default;

    /// @brief The document's columns, then `kGroupRunColumn` while the table is grouped.
    /// @return The columns.
    [[nodiscard]] std::span<table::ColumnInfo const> columns() const override { return _infos; }

    /// @brief The column groups the document declares (a `rows` table's `groups`).
    /// @return They, as `setGroups` gave them.
    [[nodiscard]] std::span<table::ColumnGroupInfo const> groups() const override { return _groups; }

    /// @brief Declares the column groups: a group filter or a values list names them by id. Set before an engine
    ///        reads the source.
    /// @param groups The groups, each of 2 to 16 columns of the source.
    void setGroups(std::vector<table::ColumnGroupInfo> groups) { _groups = std::move(groups); }

    /// @brief The rows and the cells of the columns in use.
    /// @return The same pointer until they change.
    [[nodiscard]] std::shared_ptr<table::RowSnapshot const> snapshot() const override {
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

    /// @brief Registers @p listener.
    /// @param listener The listener.
    void subscribe(table::ChangeListener& listener) override { _listeners.add(listener); }

    /// @brief Removes @p listener; safe from inside a notification.
    /// @param listener The listener.
    void unsubscribe(table::ChangeListener& listener) override { _listeners.remove(listener); }

    /// @brief Takes a new list: matched to the held rows by key, then by the cells of the columns in use.
    /// @param rows The rows, in source order.
    void assign(ValueList rows) {
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
            std::vector<Value> by(count);
            for (std::size_t at = 0; at < count; ++at) {
                by[at] = same[at] ? _byValues[was[at]] : readBy((*kept)[at]);
            }
            auto const before = _runs;
            auto const recordsBefore = _runRecords;
            _byValues = std::move(by);
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
            for (auto const at : was) {
                if (at != kNone) {
                    stays[at] = true;
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
    }

    /// @brief Groups the rows by the `group` part's `by`, read per row, and adds the hidden column
    ///        `kGroupRunColumn`; notifies `Reset`.
    ///
    /// `by` is read untracked: what it reads besides the row regroups the rows when `setGroup` or `assign` is called
    /// again.
    /// @param by    The `by`; empty: no groups, and the hidden column goes.
    /// @param units The unit table two quantities of a run's `by` are compared through: the screen's, so 1 kg and
    ///              1000 g are one run.
    void setGroup(std::function<Value(Value const& row)> by, UnitTable units) {
        _group = std::move(by);
        _groupUnits = std::move(units);
        bool const listed = _infos.size() > _columns.size();
        if (_group && !listed) {
            _infos.push_back(table::ColumnInfo{.id = std::string{kGroupRunColumn}, .kind = table::ColumnKind::Integer,
                                               .comparator = {}, .format = {}, .level = {}, .dimensions = {}});
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

    /// @brief The run a source row belongs to: its index among the runs of adjacent rows whose `by` is the same
    ///        element as their run's first row's, in source order.
    /// @param sourceRow The row.
    /// @return The run's index; 0 when the table is not grouped or the row is past the rows.
    [[nodiscard]] std::size_t runOf(std::size_t sourceRow) const {
        return sourceRow < _runs.size() ? _runs[sourceRow] : 0;
    }

    /// @brief Each run's heading record `{key, count, first, last, items}`, with its heading key in
    ///        `detail::kGroupHeadingField`, by run index, as `TableView::runs` holds them.
    /// @return The records; none when the table is not grouped.
    [[nodiscard]] std::span<Value const> runRecords() const noexcept { return _runRecords; }

    /// @brief A sort, a filter or a values list began to read @p column: its cells are derived when this is its
    ///        first use, and a `Reset` is notified so the engine reads them.
    /// @param column The column's id; one the source does not have is ignored.
    void use(std::string_view column) {
        auto const at = columnOf(column);
        if (!at) {
            return;
        }
        if (_held[*at].uses++ > 0) {
            return;
        }
        rebuild(*at);
        _snapshot.reset();
        notify(table::RowChange{.kind = table::ChangeKind::Reset, .rows = {}});
    }

    /// @brief A use of @p column ended; at its last, its cells and tracked `sortBy`s go. Nothing is notified.
    /// @param column The column's id; one not in use is ignored.
    void unuse(std::string_view column) {
        auto const at = columnOf(column);
        if (!at || _held[*at].uses == 0 || --_held[*at].uses > 0) {
            return;
        }
        _held[*at] = Held{};
        _snapshot.reset();
    }

    /// @brief What `EngineRows` reads a row's key and record through: the snapshot's key and the row's value.
    /// @return The reader; it holds nothing of the source.
    [[nodiscard]] RecordOf recordOf() const {
        return [](table::RowSnapshot const& snapshot, std::size_t sourceRow) {
            auto const* const rows = dynamic_cast<detail::ValueRowsSnapshot const*>(&snapshot);
            return RowRecord{.key = snapshot.rowId(sourceRow), .row = rows == nullptr ? Value{} : rows->row(sourceRow)};
        };
    }

    /// @brief Cells held: one per row of each column in use, and of the run column while grouped.
    /// @return The count.
    [[nodiscard]] std::size_t cellsHeld() const noexcept {
        std::size_t cells = 0;
        for (auto const& held : _held) {
            cells += held.cells == nullptr ? 0 : held.cells->size();
        }
        return cells + (_runCells == nullptr ? 0 : _runCells->size());
    }

    /// @brief Cells derived so far: a row's field read, or its `sortBy` run, for a column in use.
    /// @return The count.
    [[nodiscard]] std::size_t cellsDerived() const noexcept { return _derived; }

    /// @brief Cells of @p column, a column in use, that are not of its kind.
    /// @param column The column's id.
    /// @return The count; 0 for a column not in use.
    [[nodiscard]] std::size_t invalidCells(std::string_view column) const {
        auto const at = columnOf(column);
        if (!at) {
            return 0;
        }
        return static_cast<std::size_t>(std::ranges::count(_held[*at].invalid, std::uint8_t{1}));
    }

private:
    // A `sortBy` cell: the row it reads, the value it read last and the tracked computation that reads it.
    struct Tracker {
        std::size_t column = 0;
        std::size_t row = 0;
        Value input;
        Value value;
        bool live = false;
        std::unique_ptr<reactive::Effect> effect;
    };

    // A column's cells while it is in use, each row's vote and whether its cell is invalid.
    struct Held {
        std::size_t uses = 0;
        std::shared_ptr<std::vector<table::Cell>> cells;
        std::vector<detail::ValueVote> votes;
        std::vector<std::uint8_t> invalid;
        std::array<std::size_t, detail::kValueVotes> counts{};
        std::vector<std::unique_ptr<Tracker>> trackers;
    };

    // What tracked `sortBy`s changed since the last notification.
    struct Pending {
        std::vector<std::size_t> rows;
        std::set<std::size_t> rerun;
        bool reset = false;
        [[nodiscard]] bool any() const noexcept { return reset || !rows.empty(); }
    };

    [[nodiscard]] std::optional<std::size_t> columnOf(std::string_view id) const {
        auto const found = std::ranges::find(_columns, id, &ValueColumn::id);
        if (found == _columns.end()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(found - _columns.begin());
    }

    void notify(table::RowChange const& change) { _listeners.notify(change); }

    // The `group`'s `by` of a row, read untracked: a change of the list is what regroups it.
    [[nodiscard]] Value readBy(Value const& row) const {
        return _runtime->untracked([&] { return _group(row); });
    }

    // A field column's value for a row.
    [[nodiscard]] Value fieldOf(std::size_t column, Value const& row) const {
        auto const* const field = row.field(_columns[column].id);
        return field == nullptr ? Value{} : *field;
    }

    // The value a column's cell of a row converts: the tracked `sortBy`'s last, or the row's field.
    [[nodiscard]] Value valueOf(std::size_t column, std::size_t row, Value const& input) const {
        auto const& trackers = _held[column].trackers;
        if (row < trackers.size() && trackers[row] != nullptr) {
            return trackers[row]->value;
        }
        return fieldOf(column, input);
    }

    // A new tracked `sortBy` for a row: its first run derives the value.
    std::unique_ptr<Tracker> track(std::size_t column, std::size_t row, Value const& input) {
        auto tracker = std::make_unique<Tracker>();
        tracker->column = column;
        tracker->row = row;
        tracker->input = input;
        Tracker* const self = tracker.get();
        tracker->effect = std::make_unique<reactive::Effect>(*_runtime, [this, self] { rerun(*self); });
        self->live = true;
        return tracker;
    }

    // A tracked `sortBy`'s run: the first stores the value, a later one updates the row's cell.
    void rerun(Tracker& tracker) {
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
    }

    // One notification for the tracked `sortBy`s that ran: `Reset` when every row's ran or a kind changed.
    void flushTracked() {
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

    // Changes one row's vote and returns the column's kind after it.
    table::ColumnKind vote(Held& held, std::size_t column, std::size_t row, detail::ValueVote next) {
        auto& current = held.votes[row];
        --held.counts.at(static_cast<std::size_t>(current));
        ++held.counts.at(static_cast<std::size_t>(next));
        current = next;
        return kindOf(column, held);
    }

    [[nodiscard]] table::ColumnKind kindOf(std::size_t column, Held const& held) const {
        if (_columns[column].kind) {
            return *_columns[column].kind;
        }
        return detail::inferredKind(held.counts, held.votes);
    }

    // The column's cells, copied first when a snapshot still shares them.
    std::vector<table::Cell>& writable(Held& held) {
        if (held.cells.use_count() > 1) {
            held.cells = std::make_shared<std::vector<table::Cell>>(*held.cells);
        }
        return *held.cells;
    }

    // Every cell of a column converted again for its kind, from the values held.
    void reconvert(std::size_t column) {
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
    }

    // A column's first use: every row's cell derived.
    void rebuild(std::size_t column) {
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
    }

    // A held column over a new list: kept rows keep their cells, others are derived and compared. Marks the rows
    // whose cell changed in @p updated; returns whether the column's kind changed.
    bool reassign(std::size_t column, std::vector<Value> const& rows, std::vector<std::size_t> const& was,
                  std::vector<bool> const& same, std::vector<bool>& updated) {
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
            Value const value = same[row] ? (tracked ? next.trackers[row]->value : fieldOf(column, rows[row]))
                                          : std::move(fresh[row]);
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
    }

    // Each row's run, each run's first row and each run's heading record, from the `by` values held. A run's key is
    // its first row's `by`, as the interpreter keys it.
    void buildRuns(std::vector<Value> const& rows) {
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
            if (!run.empty() && !detail::sameElement(_byValues[first], _byValues[row], _groupUnits)) {
                close();
            }
            if (run.empty()) {
                first = row;
            }
            run.push_back(rows[row]);
            _runs.push_back(_runRecords.size());
            cells->emplace_back(static_cast<std::int64_t>(_runRecords.size()));
        }
        if (!run.empty()) {
            close();
        }
        _runCells = std::move(cells);
    }

    reactive::Runtime* _runtime;
    std::vector<ValueColumn> _columns;
    std::function<Key(Value const& row)> _keyOf;
    std::shared_ptr<table::UnitRegistry const> _units;
    std::vector<table::ColumnInfo> _infos;
    std::vector<table::ColumnGroupInfo> _groups;
    std::vector<Held> _held;
    std::shared_ptr<std::vector<Value> const> _rows = std::make_shared<std::vector<Value> const>();
    std::shared_ptr<std::vector<Key> const> _keys = std::make_shared<std::vector<Key> const>();
    std::unordered_map<Key, std::size_t> _index;
    std::function<Value(Value const& row)> _group;
    std::vector<Value> _byValues;
    UnitTable _groupUnits;
    std::vector<std::size_t> _runs;
    std::vector<std::size_t> _runFirst;
    std::vector<Value> _runRecords;
    std::shared_ptr<std::vector<table::Cell>> _runCells;
    Pending _pending;
    bool _posted = false;
    std::size_t _derived = 0;
    mutable std::shared_ptr<table::RowSnapshot const> _snapshot;
    table::detail::Listeners _listeners;
    // Last, so it is the first to go: no posted notification reaches a half-destroyed source.
    async::CallbackScope _alive;
};

// ── A typed list action's pages ─────────────────────────────────────────────

namespace detail {

/// @brief A typed list action's reply as read before its rows are cells: `Page<Row>`'s members, each row a JSON DOM.
using TypedPageDom = table::Page<glz::generic_u64>;

/// @brief What one JSON member says its column's kind is.
enum class JsonVote : std::uint8_t {
    None,     ///< Nothing: `null`, an array, or an object that is neither a decimal nor a measure.
    Bool,     ///< `true` or `false`: a `Bool` column.
    Integer,  ///< An integer: an `Integer` column.
    Number,   ///< A number with a fraction or an exponent: a `Number` column.
    Decimal,  ///< `{num, den, dp}`: a `Decimal` column.
    Measure,  ///< `{num, den, dp, unit}`: a `Measure` column.
    Text,     ///< A string: a `Text` column.
};

/// @brief How many `JsonVote`s there are.
inline constexpr std::size_t kJsonVotes = 7;

/// @brief A JSON integer as an `int64`.
/// @param member The member.
/// @return Its value, or nothing when it is not an integer or does not fit.
[[nodiscard]] inline std::optional<std::int64_t> jsonInteger(glz::generic_u64 const& member) {
    if (auto const* const value = member.get_if<std::int64_t>()) {
        return *value;
    }
    if (auto const* const value = member.get_if<std::uint64_t>();
        value != nullptr && *value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(*value);
    }
    return std::nullopt;
}

/// @brief An exact amount written `{num, den, dp}`, with a `unit` or without.
struct JsonDecimal {
    /// @brief The amount, its places kept.
    math::Rational amount;
    /// @brief The unit's id; null when the object names none.
    std::string const* unit = nullptr;
};

/// @brief An object of exactly `num`, `den` and `dp` (integers, `den` not 0, `dp` 0 to 18), and optionally a `unit`
///        string, read as an exact amount.
/// @param member The member.
/// @return The amount and its unit, or nothing for any other member.
[[nodiscard]] inline std::optional<JsonDecimal> jsonDecimal(glz::generic_u64 const& member) {
    auto const* const object = member.get_if<glz::generic_u64::object_t>();
    if (object == nullptr || (object->size() != 3 && object->size() != 4)) {
        return std::nullopt;
    }
    auto const integer = [object](std::string_view name) -> std::optional<std::int64_t> {
        auto const found = object->find(name);
        return found == object->end() ? std::nullopt : jsonInteger(found->second);
    };
    auto const num = integer("num");
    auto const den = integer("den");
    auto const dp = integer("dp");
    if (!num || !den || !dp || *den == 0 || *dp < 0 || *dp > static_cast<std::int64_t>(math::kMaxDecimalPlaces)) {
        return std::nullopt;
    }
    std::string const* unit = nullptr;
    if (object->size() == 4) {
        auto const found = object->find("unit");
        unit = found == object->end() ? nullptr : found->second.get_if<std::string>();
        if (unit == nullptr) {
            return std::nullopt;
        }
    }
    auto const amount = math::Rational::from(math::Numerator{*num}, math::Denominator{*den},
                                             math::DecimalPlaces{static_cast<std::uint32_t>(*dp)});
    if (!amount) {
        return std::nullopt;
    }
    return JsonDecimal{.amount = *amount, .unit = unit};
}

/// @brief The kind a member votes its column.
/// @param member The member.
/// @return The vote; `None` for `null`, an array and any object that is not a decimal or a measure.
[[nodiscard]] inline JsonVote jsonVoteOf(glz::generic_u64 const& member) {
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

/// @brief The column kind a vote names.
/// @param vote A vote other than `None`.
/// @return The kind.
[[nodiscard]] constexpr table::ColumnKind kindOfJsonVote(JsonVote vote) noexcept {
    switch (vote) {
        case JsonVote::Bool:
            return table::ColumnKind::Bool;
        case JsonVote::Integer:
            return table::ColumnKind::Integer;
        case JsonVote::Number:
            return table::ColumnKind::Number;
        case JsonVote::Decimal:
            return table::ColumnKind::Decimal;
        case JsonVote::Measure:
            return table::ColumnKind::Measure;
        case JsonVote::Text:
        case JsonVote::None:
        default:
            return table::ColumnKind::Text;
    }
}

/// @brief A column's kind from the members its rows hold under its id.
///
/// No vote is `Text`; votes all alike are their kind; integers and decimals alone are `Decimal`; integers with
/// fractional numbers, decimals or not, are `Number`; any other mix is the first non-null member's kind.
/// @param rows   The rows.
/// @param column The column's id.
/// @return The kind.
[[nodiscard]] inline table::ColumnKind inferredJsonKind(std::span<glz::generic_u64 const> rows,
                                                        std::string_view column) {
    std::array<std::size_t, kJsonVotes> counts{};
    JsonVote first = JsonVote::None;
    for (auto const& row : rows) {
        auto const* const object = row.get_if<glz::generic_u64::object_t>();
        if (object == nullptr) {
            continue;
        }
        auto const found = object->find(column);
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

/// @brief A member as a cell of a column of @p kind.
///
/// `null` is an empty cell. An integer is an `int64` (beyond its range a `double`), a `Rational` of 0 places under
/// `Decimal` and a `double` under `Number`; a fractional number a `double`; a string its text, under `Date` its days when it is `YYYY-MM-DD` and
/// under `DateTime` its seconds rounded down when it is ISO-8601; `{num, den, dp}` its `Rational`; with a `unit` under
/// `Measure` a measure in @p units' number for the unit (`kUnknownUnit`, invalid, when @p units lacks it). Anything
/// else is the member's JSON text, which only a `Text` column reads as valid.
/// @param member The member.
/// @param kind   The column's kind.
/// @param units  The registry a measure's unit is found in; null finds none.
/// @param named  The unit ids of the page's measures, by number: what a measure found adds to.
/// @return The cell.
[[nodiscard]] inline table::Cell typedCell(glz::generic_u64 const& member, table::ColumnKind kind,
                                           table::UnitRegistry const* units,
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

/// @brief A key member read as a row's id: an integer, or a string.
/// @param member The member.
/// @return The id, or nothing for any other member.
[[nodiscard]] inline std::optional<table::RowId> typedKey(glz::generic_u64 const& member) {
    if (auto const integer = jsonInteger(member)) {
        return table::RowId{*integer};
    }
    if (auto const* const text = member.get_if<std::string>()) {
        return table::RowId{*text};
    }
    return std::nullopt;
}

/// @brief A typed list action's reply, read as a DOM, as a generic page over @p description's columns.
/// @param dom         The reply.
/// @param description The description: its columns, their kinds and its key.
/// @param units       The registry a measure's unit is found in; null resolves through `registryOf(description)`,
///                    built only when a column is `Measure`.
/// @return The page, or `InvalidValue` when a row is not an object or has no key, or `unloadedRows` or `indices` do
///         not fit the rows, or what `registryOf` refuses.
[[nodiscard]] inline std::expected<table::GenericPage, table::TableError> typedPageOf(
    TypedPageDom const& dom, table::TableDescription const& description, table::UnitRegistry const* units) {
    auto const invalid = [](std::string message) {
        return std::unexpected(
            table::TableError{.code = table::TableErrorCode::InvalidValue, .column = {}, .message = std::move(message)});
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
        auto const* const object = dom.rows[at].get_if<glz::generic_u64::object_t>();
        if (object == nullptr) {
            return invalid("row " + std::to_string(at) + " is not an object");
        }
        auto const keyMember = object->find(description.key);
        auto key = keyMember == object->end() ? std::nullopt : typedKey(keyMember->second);
        if (!key) {
            return invalid("row " + std::to_string(at) + " has no key `" + description.key + "`");
        }
        page.keys.push_back(std::move(*key));
        table::GenericRow cells;
        cells.reserve(description.columns.size());
        for (auto const& column : description.columns) {
            auto const found = object->find(column.info.id);
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

/// @brief The most units a window lists for the members of its replies that no description lists. Each is a
///        dimension of its own that the description and its registry keep for the window's life, and one more
///        is added per unit a reply names; a reply naming more is not describing quantities, and its further
///        units stay unknown, their cells invalid.
inline constexpr std::size_t kMaxUnlistedUnits = 64;

/// @brief The units the `Measure` columns' members of @p rows name that @p units does not have, each its own
///        canonical unit in a dimension named by its id, in the order first met.
/// @param rows        The rows.
/// @param description The description: its `Measure` columns.
/// @param units       The registry the description's units are in; null has none.
/// @param room        How many more units may be listed: the rest stay unknown.
/// @return The units to list, at most @p room.
[[nodiscard]] inline std::vector<table::UnitInfo> unlistedUnits(std::span<glz::generic_u64 const> rows,
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

/// @brief Reads a typed list action's reply text as a DOM.
/// @param json The reply.
/// @return The DOM, or `InvalidValue` when the text is not a page.
[[nodiscard]] inline std::expected<TypedPageDom, table::TableError> typedPageDom(std::string_view json) {
    TypedPageDom dom;
    if (auto const failed =
            glz::read<glz::opts{.null_terminated = false, .error_on_unknown_keys = false}>(dom, json)) {
        return std::unexpected(table::TableError{.code = table::TableErrorCode::InvalidValue,
                                                 .column = {},
                                                 .message = "not a page: " + glz::format_error(failed, json)});
    }
    return dom;
}

}  // namespace detail

/// @brief A typed list action's reply (`Page<Row>`'s JSON: `rows`, `total`, `totalKind`, `offset`, `indices`) as a
///        generic page over @p description's columns: each row object's member of a column's id read by its kind.
///
/// The text is read as a `glz::generic_u64` DOM, so an `int64` keeps every digit. A member absent or `null` is an
/// empty cell; an integer an `int64` (a `Rational` of 0 places under `Decimal`, a `double` under `Number`); a number
/// with a fraction a `double`; a string its text, under `Date` `YYYY-MM-DD` its days and under `DateTime` ISO-8601
/// its seconds rounded down; `true` and `false` a `bool`; `{num, den, dp}` its `Rational`, and with a `unit` under
/// `Measure` a measure through @p units, invalid when the unit is not there. Anything else is the member's JSON
/// text, invalid outside `Text`. A row's key is its member named by the description's `key`. The page's `columns`
/// are every description column, whatever the query named, and its `revision` is empty.
/// @param json        The reply's text.
/// @param description The description: the columns, their kinds and the key.
/// @param units       The registry a measure's unit is found in; null resolves through `registryOf(description)`,
///                    built only when a column is `Measure`.
/// @return The page, or `InvalidValue` when the text is not a page, a row is not an object or has no key, or
///         `unloadedRows` or `indices` do not fit the rows, or what `registryOf` refuses.
[[nodiscard]] inline std::expected<table::GenericPage, table::TableError> readTypedPage(
    std::string_view json, table::TableDescription const& description, table::UnitRegistry const* units = nullptr) {
    auto dom = detail::typedPageDom(json);
    if (!dom) {
        return std::unexpected(std::move(dom.error()));
    }
    return detail::typedPageOf(*dom, description, units);
}

// ── Over a server's pages ───────────────────────────────────────────────────

/// @brief How a `WindowRows` reaches its table.
struct WindowRowsOptions {
    /// @brief The model's channel.
    std::shared_ptr<ModelChannel> channel;
    /// @brief The table's id: its actions are `Table.<id>` and `Table.<id>.describe`. Empty with `action`.
    std::string table;
    /// @brief A typed list action's id, sent in place of `Table.<id>`: its reply is a `Page<Row>`, read with
    ///        `readTypedPage`. Empty for a model's table.
    std::string action;
    /// @brief With `action`: the request member the query is written into, the one its schema marks `x-table`.
    std::string queryMember;
    /// @brief With `action`: the columns whose kind the first reply with rows decides, by the members of their id
    ///        (`detail::inferredJsonKind`). Their kind in the description is a placeholder until then, and stays
    ///        what that reply made it for the rows' life.
    std::vector<std::string> inferKinds;
    /// @brief The records watched for the window's life: a model's table its `<model type>.<id>`, a typed list
    ///        action each record its catalog entry names. None: a reconnect or `resync` of the session drops the
    ///        pages, as a record watch's change of every row does.
    std::vector<std::string> records;
    /// @brief The node's `body`: the query's fixed part. A `query` record in it gives the query's other members.
    Value body;
    /// @brief Rows per page.
    std::int64_t pageSize = 200;
    /// @brief Pages held at most.
    std::size_t maxPages = 8;
    /// @brief Keys one keyed fetch asks for at most.
    std::size_t maxKeys = 1000;
    /// @brief Columns a page holds at most.
    std::size_t maxColumns = 96;
    /// @brief Sends no query until `release`, so a stored sort and filter can be applied first.
    bool restoring = false;
};

/// @brief What a `WindowRows` has done, for tests and the inspector.
struct WindowRowsStats {
    /// @brief Page and column fetches sent.
    std::size_t pageFetches = 0;
    /// @brief Keyed fetches sent.
    std::size_t rowsFetches = 0;
    /// @brief Fetches cancelled because the visible rows left their pages.
    std::size_t cancelled = 0;
    /// @brief Cells turned into a row record's fields.
    std::size_t cellsConverted = 0;
    /// @brief Distinct-values queries sent.
    std::size_t distinctFetches = 0;
};

/// @brief A table's rows over a server's pages: a `table::PageWindow` of generic rows, the table's description
///        and the session.
///
/// It watches the table's records first and sends no page fetch until every watch is answered, a description is
/// set and, when restoring, `release` is called. Every fetch goes through `Session::read` as `Table.<id>`, makes
/// no cache entry and is cancelled through its own stop token; its reply's text is read with
/// `table::readGenericPage` against the held description.
///
/// Over a typed list action (`WindowRowsOptions::action`) every fetch is that action, its body the node's body with
/// the query in `queryMember`, and its reply is read with `readTypedPage`. The reply holds whole rows, so `visible`'s
/// columns name what a row's record converts and not what is fetched. The columns of `inferKinds` take their kinds
/// from the first reply that has rows and reads, read once as a DOM for both the kinds and the page. A unit a
/// `Measure` member names that the description does not list is added to it as its own canonical unit, so its cell
/// is a measure in that unit and not an invalid one. A window that watches no record drops its pages on the
/// session's reconnect and `resync`.
///
/// Timing. A fetch for a page held, or next to a page held, goes at once, and so does the first page while none is
/// held; any other waits until the first visible row has stayed in one page for `SessionOptions::tableSettle`.
/// Every fetch the window no longer covers is cancelled. While the order waits for a level every change event
/// invalidates, and a refetch of a held page goes no sooner than `SessionOptions::tableAwaitedRefetch` after the last
/// one: the first after a quiet spell goes at once, and the events between two refetches are one.
///
/// A row's record holds one field per column of the latest `visible` call that the window holds for the row,
/// converted by `cellValue` when the row is first read and kept by key until the row's page is fetched again.
///
/// A change is the diff, by key, of the rows held before a reply against the rows held after it: a new total moves
/// the view's end (`Inserted` or `Removed` there), a row that moved within its page is `Moved`, a row that came or
/// went is `Inserted` or `Removed`, and a place whose cells arrived, were replaced or were evicted is `Changed`.
/// Each operation is valid on the list the one before it leaves, and no key is held twice between two of them. A new
/// query (a sort, a filter or a description revision) is a `Reset`, and so is a reply whose page holds a key twice,
/// which no diff by key can place, or whose page diff would take more than 5,000 operations, the bound of the
/// `ViewChange` contract.
///
/// A values list (`distinctValues`) is a query carrying `distinct` under the held filter. The window keeps each reply
/// by the query's body until the table's record changes, the session reconnects or a new revision is described, so
/// a filter menu opened again sends nothing; it keeps at most 64, the oldest going first, and a reply asked for
/// before such a change answers its menu without being kept. The replies are this table's rows' own, not shared with
/// another table.
///
/// Neither copied nor moved: its watch handler and its replies hold it.
class WindowRows final : public TableRows {
public:
    /// @brief Rows of a table, watching its record from now.
    /// @param session The session; must outlive the rows.
    /// @param options The channel, the table and the window's bounds.
    WindowRows(Session& session, WindowRowsOptions options)
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

    WindowRows(WindowRows const&) = delete;
    WindowRows& operator=(WindowRows const&) = delete;
    WindowRows(WindowRows&&) = delete;
    WindowRows& operator=(WindowRows&&) = delete;

    /// @brief Cancels every fetch in flight.
    ~WindowRows() override {
        stopAll();
        for (auto& [id, stop] : _distinctFlights) {
            static_cast<void>(id);
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
                    request->promise.resolve(std::expected<std::vector<table::RowId>, table::TableError>{
                        std::unexpected(table::TableError{.code = table::TableErrorCode::Unavailable,
                                                          .column = {},
                                                          .message = "the rows are gone"})});
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
                    request->promise.resolve(std::expected<std::vector<FetchedRow>, table::TableError>{
                        std::unexpected(table::TableError{.code = table::TableErrorCode::Unavailable,
                                                          .column = {},
                                                          .message = "the rows are gone"})});
                }
            }
        }
        _locateStop.request_stop();
    }

    /// @brief Takes the table's description: fits the sort, the filters and the visible columns to it, and starts
    ///        a new query when it is another revision than the one held.
    /// @param description The description, its revision set.
    void setDescription(table::TableDescription description) {
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
        auto fitted = table::fitState(
            table::StateDocument{
                .version = table::kStateVersion, .layout = {}, .sort = _sort, .filters = _filter, .view = {},
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
        std::erase_if(_columns, [this](std::string const& id) { return !_infos.contains(id); });
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

    /// @brief Takes the description as the `Table.<id>.describe` query holds it.
    ///
    /// A description's numbers are integers or exact decimals, which a `Value` holds without loss, so it is read
    /// back from the value's canonical JSON. One that does not read sets `error()` to `InvalidValue` and stops
    /// every page fetch until a description that reads is set.
    /// @param described The query's value.
    void setDescription(Value const& described) {
        table::TableDescription description;
        auto const text = ui::detail::canonicalJson(described);
        if (auto const failed = glz::read<glz::opts{.error_on_unknown_keys = false}>(description, text)) {
            _descriptionError = true;
            _error = table::TableError{.code = table::TableErrorCode::InvalidValue,
                                       .column = {},
                                       .message = "the table's description does not read: " +
                                                  glz::format_error(failed, text)};
            status();
            return;
        }
        setDescription(std::move(description));
    }

    /// @brief The description held.
    /// @return It, or null before the first.
    [[nodiscard]] table::TableDescription const* description() const noexcept {
        return _description ? &*_description : nullptr;
    }

    /// @brief Sets what hears a reply that names another revision than the description held, once per revision.
    ///        The reply is still taken.
    /// @param handler The receiver; empty to stop receiving.
    void onStaleDescription(std::function<void()> handler) { _onStale = std::move(handler); }

    /// @brief Sets what hears the rows a keyed fetch named and its reply holds none of: a change event's refetch of a
    ///        row that was deleted or no longer matches the filter. Called once per such reply, after the rows took
    ///        it, with those keys; a reply that holds every key it named is not reported.
    /// @param handler The receiver; empty to stop receiving.
    void onRowsAbsent(std::function<void(std::span<table::RowId const>)> handler) { _onRowsAbsent = std::move(handler); }

    /// @brief Sets what hears that `error()` or `counts()` may have changed: a fetch was sent, answered or failed,
    ///        or a reply or a description did not read. A failed fetch changes no row, so the view's `onChange` says
    ///        nothing of it; this is how a status line learns of it. Called after the rows took the event.
    /// @param handler The receiver; empty to stop receiving.
    void onStatus(std::function<void()> handler) { _onStatus = std::move(handler); }

    /// @brief The query's filter.
    /// @return It.
    [[nodiscard]] table::FilterSpec const& filter() const noexcept { return _filter; }

    /// @brief Whether the kinds of `WindowRowsOptions::inferKinds` still wait for the first reply with rows: until
    ///        then those columns hold a `Text` placeholder, which a filter must not be compiled against.
    /// @return `true` while they wait.
    [[nodiscard]] bool inferring() const noexcept { return _inferring; }

    /// @brief Sets what hears that the first reply with rows fixed the kinds of `inferKinds`, once. Called while
    ///        that reply is being read, so the receiver posts what it does to the rows.
    /// @param handler The receiver; empty to stop receiving.
    void onInferred(std::function<void()> handler) { _onInferred = std::move(handler); }

    /// @brief Leaves the restoring state: the first query goes, with the sort and filter set meanwhile.
    void release() {
        if (!_restoring) {
            return;
        }
        _restoring = false;
        pump();
    }

    /// @brief Fetches the visible pages again with `TableQuery::retry`, so failed level loads are tried at once.
    ///        Every fetch of that refetch carries the flag, one the timing rules hold back included.
    void retry() {
        _error.reset();
        _retry = true;
        _window.invalidate();
        pump();
    }

    /// @brief The last error: a fetch that failed, a reply or a description that did not read. Cleared by the next
    ///        page taken.
    /// @return The error, or nothing.
    [[nodiscard]] std::optional<table::TableError> const& error() const noexcept { return _error; }

    /// @brief What the rows have done.
    /// @return The counters.
    [[nodiscard]] WindowRowsStats const& stats() const noexcept { return _stats; }

    /// @brief The view's rows, as the latest page reply said.
    /// @return The total; not known before a reply.
    [[nodiscard]] TableTotal total() const override {
        auto const rows = _window.total();
        return TableTotal{.rows = rows.value_or(0), .kind = _window.totalKind(), .known = rows.has_value()};
    }

    /// @brief The held row at a view index, as a record of the columns the latest `visible` named.
    /// @param index The view index.
    /// @return The row, or nothing while it is not held.
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override {
        auto const* const key = _window.key(index);
        if (key == nullptr) {
            return std::nullopt;
        }
        auto [found, made] = _memo.try_emplace(*key, Entry{.key = *key, .record = {}, .stamp = 0});
        Entry& entry = found->second;
        if (made || entry.stamp != _columnsStamp) {
            ValueRecord fields;
            auto const convert = [&](std::string const& id) {
                auto const* const cell = _window.cell(index, id);
                auto const info = _infos.find(id);
                if (cell == nullptr || info == _infos.end()) {
                    return;
                }
                fields.insert_or_assign(id, cellValue(*cell, info->second, _units.get()));
                ++_stats.cellsConverted;
            };
            if (_columns.empty() && _description) {
                for (auto const& column : _description->columns) {
                    convert(column.info.id);
                }
            } else {
                for (auto const& id : _columns) {
                    convert(id);
                }
            }
            entry.record = Value{std::move(fields)};
            entry.stamp = _columnsStamp;
        }
        return TableRowRef{.key = entry.key, .row = entry.record};
    }

    /// @brief The view rows the window holds, as whole pages: what a reader of every held row walks instead of the
    ///        view. A page's last rows may lie past the view's end, where `row` answers nothing.
    /// @return Each held page's first view row and its rows, ascending.
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> heldRanges() const override {
        auto const size = std::max<std::int64_t>(_options.pageSize, 1);
        std::vector<std::pair<std::int64_t, std::int64_t>> out;
        for (std::int64_t const page : _heldPages) {
            if (heldPage(page)) {
                out.emplace_back(page * size, size);
            }
        }
        return out;
    }

    /// @brief The held row's unloaded levels.
    /// @param index The view index.
    /// @return The window's view of them.
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override {
        return _window.unloadedLevels(index);
    }

    /// @brief Declares the visible rows and the columns their records hold, and sends what is due.
    /// @param first   The first visible view row.
    /// @param count   The rows.
    /// @param columns The column ids; empty means every column. An id the description lacks is dropped.
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        _first = std::max<std::int64_t>(first, 0);
        _count = std::max<std::int64_t>(count, 0);
        std::vector<std::string> named(columns.begin(), columns.end());
        if (_description) {
            std::erase_if(named, [this](std::string const& id) { return !_infos.contains(id); });
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

    /// @brief The keys of a range of view rows, for a range selection: the rows held give theirs, and every page of
    ///        the range with a row not held is fetched naming no columns, so its reply carries keys alone. The pages
    ///        so fetched are not held.
    /// @param first The first view row.
    /// @param count The rows.
    /// @return The keys once every page has answered; `InvalidSpec` for a negative range, `Unavailable` when a page
    ///         failed or a row of the range is in none.
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(
        std::int64_t first, std::int64_t count) override {
        using Result = std::expected<std::vector<table::RowId>, table::TableError>;
        auto [completion, promise] = async::Completion<Result>::makeSettleable(&_session->runtime().owner());
        if (first < 0 || count < 0 || count > std::numeric_limits<std::int64_t>::max() - first) {
            promise.resolve(Result{std::unexpected(table::TableError{
                .code = table::TableErrorCode::InvalidSpec,
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
            std::int64_t const to = std::min(first + count, (page + 1) * size);
            for (std::int64_t at = from; at < to; ++at) {
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
            std::stop_source stop;
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

    /// @brief Finds the view index of the row with a key: a keyed fetch naming no columns, under the current sort and
    ///        filters.
    /// @param key  The row's key.
    /// @param stop Stops the fetch; once a stop is requested, @p done is not called.
    /// @param done Called on the owner with the row's view index, nothing when the reply has no row, or the error the
    ///        fetch failed with; `Unavailable` while no description is held, and `InvalidValue` when the reply holds
    ///        the row without its index.
    void locateRow(table::RowId key, std::stop_token stop, std::function<void(RowLocation const&)> done) override {
        table::TableQuery query = queryOf(table::PageRequest{}, {});
        query.columns = std::vector<std::string>{};
        query.keys = std::vector<table::RowId>{std::move(key)};
        auto body = _description ? bodyOf(query)
                                 : std::expected<Value, table::TableError>{std::unexpected(unavailable())};
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

    /// @brief Fetches rows by key: keyed queries under the current sort and filters naming @p columns, at most
    ///        `WindowRowsOptions::maxKeys` keys each, through the session's read, which makes no cache entry. The
    ///        replies are not held.
    /// @param keys    The rows' keys.
    /// @param columns The column ids whose cells each row carries.
    /// @return The rows the replies hold, with their view indices; a key no reply holds is left out. A cell of a
    ///         level the reply names the row as lacking is `table::Unloaded`. `Unavailable` while no description is
    ///         held or when a fetch failed, and `InvalidValue` for a reply that gives a row no index.
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) override {
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
            query.keys = std::vector<table::RowId>(keys.begin() + static_cast<std::ptrdiff_t>(from),
                                                   keys.begin() + static_cast<std::ptrdiff_t>(
                                                                      std::min(from + chunk, keys.size())));
            auto body = bodyOf(query);
            if (!body) {
                rowsAnswered(*state, std::unexpected(std::move(body.error())));
                continue;
            }
            std::stop_source stop;
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
            reply.onError(_scope, [this, state](std::exception_ptr const& error) {
                // A stopped fetch is the window going: the rows are never answered.
                if (!wasCancelled(error)) {
                    rowsAnswered(*state, std::unexpected(failureOf(error)));
                }
            });
        }
        return std::move(completion);
    }

    /// @brief Finds a row's view index, as `locateRow` does, stopping the fetch an earlier call still has in flight,
    ///        whose handler is then never called.
    /// @param key  The row's key.
    /// @param done Called with the row's view index, or nothing when the view has no row with @p key or the fetch
    ///        failed.
    void locate(table::RowId key, std::function<void(std::optional<std::int64_t>)> done) {
        stopLocating();
        locateRow(std::move(key), _locateStop.get_token(),
                  [done = std::move(done)](RowLocation const& found) { done(found ? *found : std::nullopt); });
    }

    /// @brief Stops a `locate` fetch in flight; its handler is never called.
    void stopLocating() {
        _locateStop.request_stop();
        _locateStop = std::stop_source{};
    }

    /// @brief Sets the query's sort: a new query.
    /// @param sort The keys.
    /// @return `UnknownColumn`, `InvalidSpec` for a column that is not sortable or named twice, or `LimitExceeded`
    ///         past the table's sort bound, checked against the description held; the sort is then unchanged.
    std::expected<void, table::TableError> setSort(SortChain sort) override {
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

    /// @brief Sets the query's filter: a new query.
    /// @param filter The filter.
    /// @return The error compiling it against the description held; the filter is then unchanged.
    std::expected<void, table::TableError> setFilter(table::FilterSpec filter) override {
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

    /// @brief The distinct values of a column: a query carrying `distinct` under the held filter. Its reply is kept
    ///        by the query's body until the table's record changes, so a menu opened again sends nothing.
    /// @param request The request.
    /// @return The values, delivered on the owner; `Unavailable` without a description, for a table whose
    ///         description says it lists no values, and over a typed list action, whose reply holds rows alone.
    async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest request) override {
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
        std::stop_source stop;
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

    /// @brief The window's counts: its total, undecided rows, awaited and failed levels, and whether a fetch is in
    ///        flight. The rows before the filter are not known.
    /// @return The counts.
    [[nodiscard]] TableCountsView counts() const override {
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

    /// @brief The rows an action of this session is working on, and those an action could not change.
    /// @return The rows' states.
    [[nodiscard]] RowStates& states() override { return _states; }

    /// @brief The rows an action of this session is working on, and those an action could not change.
    /// @return The rows' states.
    [[nodiscard]] RowStates const& states() const override { return _states; }

    /// @brief Moves a row's place among the retained rows and its states to its new key. Nothing is fetched: the
    ///        change event of the action names both keys.
    /// @param from The row's old key.
    /// @param to   The row's new key.
    void moveKey(table::RowId const& from, table::RowId const& to) override {
        _states.move(from, to);
        if (from == to || !_retainedSet.contains(from)) {
            return;
        }
        _retainedSet.erase(from);
        if (_retainedSet.insert(to).second) {
            std::ranges::replace(_retained, from, to);
        } else {
            std::erase(_retained, from);
        }
        _unmatched.erase(from);
        _unmatchedHeld.erase(from);
        // The revision held is of the view with the old key retained.
        ++_retainStamp;
        _viewRevision.clear();
    }

    /// @brief Adds keys to the retained rows, all or none; each query from then on carries the set in `retain`.
    ///
    /// The keys added are refetched as a change event's keys are, so the rows that left the filter come back into
    /// the view; while a run holds the refetches, they are marked with the events'.
    /// @param keys The rows' keys.
    /// @return `OverBound` when they would take the set past `table::QueryLimits::maxRetained`; the set is then
    ///         unchanged.
    [[nodiscard]] RetainResult retain(std::span<table::RowId const> keys) override {
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

    /// @brief Drops every retained row, and fetches the held pages again without them.
    void clearRetained() override {
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

    /// @brief The retained rows, in the order they were added.
    /// @return Their keys.
    [[nodiscard]] std::span<table::RowId const> retained() const noexcept { return _retained; }

    /// @brief Whether a held row is retained and the filter rejects it.
    /// @param key The row's key.
    /// @return `true` for a held row the latest reply that supplied it listed in `unmatched`.
    [[nodiscard]] bool unmatched(table::RowId const& key) const override { return _unmatchedHeld.contains(key); }

    /// @brief Holds back the refetches change events cause, while @p run runs, for at most
    ///        `SessionOptions::retainHold` from the first run held; an event meanwhile only marks what it would fetch.
    ///        Without a scheduler nothing is held.
    /// @param run The run's id.
    void holdRefetches(std::uint64_t run) override {
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

    /// @brief Ends @p run's hold; when no run holds any more, what the events marked goes as one refetch.
    /// @param run The run's id.
    void releaseRefetches(std::uint64_t run) override {
        if (_holding.erase(run) == 0 || !_holding.empty()) {
            return;
        }
        _holdTimer = {};
        flushMarked();
    }

    /// @brief The view as a selection takes it: the held filter and the latest reply's revision.
    ///
    /// Refused while the view's membership is open: rows are undecided, or an awaited level is one a filtered
    /// column needs. A level only the sort needs refuses nothing, since order is not membership. Refused too, with
    /// nothing undecided and no level, until a reply to the current filter has named its revision.
    /// @return The view, or the refusal with the undecided rows and the filtered columns' awaited levels.
    [[nodiscard]] std::expected<ViewSelection, SelectAllRefusal> selectView() const override {
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

    /// @brief How many rows a selection holds, as far as the window knows.
    ///
    /// A selection of the view holds the view's total less each key of `except` the window holds; a key owed a
    /// refetch still subtracts. The count is exact when the total is, and every key of `except` is held and owed
    /// nothing, or known to have left the view.
    /// @param selection The selection.
    /// @return The count.
    [[nodiscard]] SelectedCount selectedCount(TableSelection const& selection) const override {
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

    /// @brief Whether a keyed reply left a key out of the view since the filter was set.
    /// @param key The key.
    /// @return `true` until a reply holds the key again.
    [[nodiscard]] bool leftView(table::RowId const& key) const override { return _outOfView.contains(key); }

    /// @brief Fetches a range of view rows as page queries of `pageSize` rows naming @p columns, under the held sort
    ///        and filter, whether or not the window holds them.
    /// @param first   The first view row.
    /// @param count   The rows.
    /// @param columns The column ids whose cells each row carries.
    /// @return The rows of the range, delivered on the owner; `Unavailable` without a description.
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> rowsOf(
        std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
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
            std::stop_source stop;
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
            reply.onError(_scope, [this, state](std::exception_ptr const& error) {
                if (!wasCancelled(error)) {
                    rowsAnswered(*state, std::unexpected(failureOf(error)));
                }
            });
        }
        return std::move(completion);
    }

    /// @brief Sets the one receiver of the rows' changes.
    /// @param handler The receiver; empty to stop receiving.
    void onChange(std::function<void(table::ViewChange const&)> handler) override { _onChange = std::move(handler); }

private:
    friend struct testing::WindowRowsProbe;

    // A row's record, kept by key until its page is fetched again or the columns named change.
    struct Entry {
        table::RowId key;
        Value record;
        std::uint64_t stamp = 0;
    };

    // More operations than this in one page's diff are one `Reset`, as for the engine's default.
    static constexpr std::size_t kResetThreshold = 5000;

    struct PageFlight {
        table::PageFetch fetch;
        std::stop_source stop;
        // The retained set the query carried, by `_retainStamp`.
        std::uint64_t retained = 0;
    };

    // A range's keys in the making: the pages fetched for it, by page, and how many it still waits for.
    struct KeysRequest {
        async::Completion<std::expected<std::vector<table::RowId>, table::TableError>>::Promise promise;
        std::int64_t first = 0;
        std::int64_t count = 0;
        std::map<std::int64_t, std::vector<table::RowId>> pages;
        std::size_t waiting = 0;
        std::optional<table::TableError> failed;
        std::vector<std::stop_source> stops;
    };

    void keysAnswered(KeysRequest& request, std::int64_t page,
                      std::expected<std::vector<table::RowId>, table::TableError> keys) {
        if (keys) {
            request.pages.insert_or_assign(page, std::move(*keys));
        } else if (!request.failed) {
            request.failed = std::move(keys.error());
        }
        if (request.waiting > 0 && --request.waiting == 0) {
            finishKeys(request);
        }
    }

    // The range's keys: a fetched page's where one was fetched, the window's otherwise.
    void finishKeys(KeysRequest& request) {
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
                auto const place = static_cast<std::size_t>(at - (at / size) * size);
                if (place < page->second.size()) {
                    keys.push_back(page->second.at(place));
                    continue;
                }
            } else if (auto const* const key = _window.key(at)) {
                keys.push_back(*key);
                continue;
            }
            request.promise.resolve(Result{std::unexpected(table::TableError{
                .code = table::TableErrorCode::Unavailable,
                .column = {},
                .message = std::format("view row {} is in no page", at)})});
            return;
        }
        request.promise.resolve(Result{std::move(keys)});
    }

    // A `fetchRows` in the making: the rows its replies held so far, and how many replies it still waits for.
    struct RowsRequest {
        async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>>::Promise promise;
        std::vector<FetchedRow> rows;
        std::size_t waiting = 0;
        std::optional<table::TableError> failed;
        std::vector<std::stop_source> stops;
    };

    void rowsAnswered(RowsRequest& request, std::expected<std::vector<FetchedRow>, table::TableError> rows) {
        if (rows) {
            request.rows.insert(request.rows.end(), std::make_move_iterator(rows->begin()),
                                std::make_move_iterator(rows->end()));
        } else if (!request.failed) {
            request.failed = std::move(rows.error());
        }
        if (request.waiting > 0 && --request.waiting == 0) {
            finishRows(request);
        }
    }

    static void finishRows(RowsRequest& request) {
        using Result = std::expected<std::vector<FetchedRow>, table::TableError>;
        if (request.failed) {
            request.promise.resolve(Result{std::unexpected(*request.failed)});
            return;
        }
        request.promise.resolve(Result{std::move(request.rows)});
    }

    // A keyed reply's rows, each with the cells of @p columns in that order; a cell of a level the reply names its
    // row as lacking is unloaded, as it is `null` on the wire.
    [[nodiscard]] std::expected<std::vector<FetchedRow>, table::TableError> fetchedOf(
        table::GenericPage& page, std::vector<std::string> const& columns) const {
        if (!page.rows.empty() && page.indices.size() != page.rows.size()) {
            return std::unexpected(table::TableError{.code = table::TableErrorCode::InvalidValue,
                                                     .column = {},
                                                     .message = "the keyed reply gives its rows no index"});
        }
        std::vector<std::optional<std::size_t>> places;
        places.reserve(columns.size());
        for (std::string const& id : columns) {
            auto const found = std::ranges::find(page.columns, id);
            places.push_back(found == page.columns.end()
                                 ? std::nullopt
                                 : std::optional{static_cast<std::size_t>(found - page.columns.begin())});
        }
        std::vector<FetchedRow> out;
        out.reserve(page.rows.size());
        for (std::size_t at = 0; at < page.rows.size() && at < page.keys.size(); ++at) {
            auto const unloaded = std::ranges::find(page.unloadedRows, static_cast<std::int64_t>(at),
                                                    &table::UnloadedRow::index);
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

    struct RowsFlight {
        table::RowsFetch fetch;
        std::stop_source stop;
        // The retained set the query carried, by `_retainStamp`.
        std::uint64_t retained = 0;
    };

    [[nodiscard]] std::chrono::milliseconds now() const {
        auto const& clock = _session->options().clock;
        if (clock) {
            return clock();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
    }

    [[nodiscard]] bool ready() const noexcept {
        return _watched && _description.has_value() && !_descriptionError && !_restoring && _hasVisible;
    }

    [[nodiscard]] std::optional<table::TableError> sortError(SortChain const& sort) const {
        auto const error = [](table::TableErrorCode code, std::string column, std::string message) {
            return table::TableError{.code = code, .column = std::move(column), .message = std::move(message)};
        };
        if (sort.size() > _description->capabilities.maxSortKeys) {
            return error(table::TableErrorCode::LimitExceeded, {},
                         std::format("{} sort keys; the table takes {}", sort.size(),
                                     _description->capabilities.maxSortKeys));
        }
        for (std::size_t at = 0; at < sort.size(); ++at) {
            auto const& id = sort[at].column;
            auto const column = std::ranges::find(_description->columns, id,
                                                  [](table::TableColumn const& c) -> auto const& { return c.info.id; });
            if (column == _description->columns.end()) {
                return error(table::TableErrorCode::UnknownColumn, id, "no column " + id);
            }
            if (!column->sortable) {
                return error(table::TableErrorCode::InvalidSpec, id, "the column " + id + " is not sortable");
            }
            if (std::ranges::contains(sort.begin(), sort.begin() + static_cast<std::ptrdiff_t>(at), id,
                                      &table::SortKey::column)) {
                return error(table::TableErrorCode::InvalidSpec, id, "the column " + id + " is sorted twice");
            }
        }
        return std::nullopt;
    }

    // A new query: the window drops every page, every fetch in flight is stopped, and the view is reset.
    void restart() {
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

    void stopAll() {
        for (auto& flight : _pageFlights) {
            flight.stop.request_stop();
        }
        for (auto& flight : _rowsFlights) {
            flight.stop.request_stop();
        }
        _pageFlights.clear();
        _rowsFlights.clear();
    }

    [[nodiscard]] bool heldPage(std::int64_t page) const {
        auto const size = std::max<std::int64_t>(_options.pageSize, 1);
        return page >= 0 && _window.key(page * size) != nullptr;
    }

    // Whether a fetch goes now rather than when the visible rows settle.
    [[nodiscard]] bool due(table::PageFetch const& fetch, std::chrono::milliseconds at) const {
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
        return at - _anchorSince >= _session->options().tableSettle;
    }

    // Whether a refetch of a held page waits: the order awaits a level and the last refetch was too recent.
    [[nodiscard]] bool spaced(std::chrono::milliseconds at) const {
        return !_window.awaiting().empty() && _session->options().scheduler != nullptr && _awaitedLast &&
               at - *_awaitedLast < _session->options().tableAwaitedRefetch;
    }

    // Asks the window for what the visible rows need, cancels what it no longer covers, sends what is due and
    // leaves the rest to the settle timer, or, for a refetch while the order awaits a level, to the refetch timer.
    void pump() {
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
        auto const at = now();
        auto const size = std::max<std::int64_t>(_options.pageSize, 1);
        bool settling = false;
        bool spacing = false;
        bool refetched = false;
        for (auto& fetch : fetches) {
            bool const refetch = heldPage(fetch.request.offset / size);
            if (refetch && spaced(at)) {
                _window.cancelled(fetch);
                spacing = true;
            } else if (due(fetch, at)) {
                refetched = refetched || refetch;
                send(std::move(fetch));
            } else {
                _window.cancelled(fetch);
                settling = true;
            }
        }
        if (refetched && !_window.awaiting().empty()) {
            _awaitedLast = at;
        }
        // A retry's fetches carry the flag until the last of them, held back or not, has gone.
        if (!settling && !spacing) {
            _retry = false;
        }
        auto* const scheduler = _session->options().scheduler;
        if (settling) {
            auto const wait = _anchorSince + _session->options().tableSettle - at;
            _settleTimer = scheduler->after(std::max(wait, std::chrono::milliseconds{1}), _scope.token().guard([this] {
                _settleTimer = {};
                pump();
            }));
        }
        if (spacing && !_awaitedTimer.active()) {
            auto const wait = *_awaitedLast + _session->options().tableAwaitedRefetch - at;
            _awaitedTimer = scheduler->after(std::max(wait, std::chrono::milliseconds{1}), _scope.token().guard([this] {
                _awaitedTimer = {};
                pump();
            }));
        }
    }

    // The request body: the node's body, its query member (`query`, or a typed list action's `queryMember`)
    // overlaid with the window's sort, filter, rows and columns.
    [[nodiscard]] std::expected<Value, table::TableError> bodyOf(table::TableQuery const& query) const {
        auto const invalid = [](std::string message) {
            return std::unexpected(
                table::TableError{.code = table::TableErrorCode::InvalidValue, .column = {}, .message = std::move(message)});
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

    [[nodiscard]] table::TableQuery queryOf(table::PageRequest page, std::vector<std::string> const& columns) const {
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

    void send(table::PageFetch fetch) {
        auto body = bodyOf(queryOf(fetch.request, fetch.columns));
        if (!body) {
            _window.fail(fetch, body.error());
            _error = std::move(body.error());
            return;
        }
        std::stop_source stop;
        ++_stats.pageFetches;
        auto reply = _session->read(_options.channel, actionName(), *body, stop.get_token());
        _pageFlights.push_back(PageFlight{.fetch = fetch, .stop = std::move(stop), .retained = _retainStamp});
        reply.then(_scope, [this, fetch](std::string const& text) { pageArrived(fetch, text); });
        reply.onError(_scope, [this, fetch](std::exception_ptr const& error) { pageFailed(fetch, error); });
        status();
    }

    void send(table::RowsFetch fetch) {
        auto query = queryOf(table::PageRequest{}, fetch.columns);
        query.keys = fetch.keys;
        auto body = bodyOf(query);
        if (!body) {
            keyedFailed(std::move(body.error()));
            return;
        }
        std::stop_source stop;
        ++_stats.rowsFetches;
        auto reply = _session->read(_options.channel, actionName(), *body, stop.get_token());
        _rowsFlights.push_back(RowsFlight{.fetch = fetch, .stop = std::move(stop), .retained = _retainStamp});
        reply.then(_scope, [this, fetch](std::string const& text) { rowsArrived(fetch, text); });
        reply.onError(_scope, [this, fetch](std::exception_ptr const& error) { rowsFailed(fetch, error); });
        status();
    }

    [[nodiscard]] std::string actionName() const {
        return _options.action.empty() ? "Table." + _options.table : _options.action;
    }

    void answered() {
        if (_unanswered > 0 && --_unanswered == 0) {
            _watched = true;
            pump();
        }
    }

    // A reply's page against the held description. A typed list action's first reply with rows also fixes the kinds
    // of the columns `inferKinds` names, from the same DOM the page is read from.
    [[nodiscard]] std::expected<table::GenericPage, table::TableError> decode(std::string const& text) {
        if (_options.action.empty()) {
            return table::readGenericPage(text, *_description, _units.get());
        }
        auto dom = detail::typedPageDom(text);
        if (!dom) {
            return std::unexpected(std::move(dom.error()));
        }
        if (!_inferring || dom->rows.empty()) {
            listUnits(dom->rows);
            return detail::typedPageOf(*dom, *_description, _units.get());
        }
        // The kinds are fixed only by a reply that reads: one that does not leaves them to the next.
        auto const before = _description->columns;
        inferKinds(dom->rows);
        listUnits(dom->rows);
        auto page = detail::typedPageOf(*dom, *_description, _units.get());
        if (!page) {
            _description->columns = before;
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

    // A unit a `Measure` column's member names that the description does not list is its own canonical unit, in a
    // dimension of its own named by its id, as an expression reads a unit missing from the screen's table. It is
    // added after the units listed, so the numbers held cells carry keep their meaning; at most
    // `detail::kMaxUnlistedUnits` are, the rest staying unknown.
    void listUnits(std::span<glz::generic_u64 const> rows) {
        auto const added = detail::unlistedUnits(rows, *_description, _units.get(),
                                                 detail::kMaxUnlistedUnits - std::min(_unlisted, detail::kMaxUnlistedUnits));
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

    // The kinds the first rows give the columns `inferKinds` names. The description keeps its revision, so the
    // query, the window and the fetches in flight stand; only the records made of the cells are made again.
    void inferKinds(std::span<glz::generic_u64 const> rows) {
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

    [[nodiscard]] static bool wasCancelled(std::exception_ptr const& error) {
        try {
            std::rethrow_exception(error);
        } catch (::core::async::OperationCancelled const&) {
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] static table::TableError failureOf(std::exception_ptr const& error) {
        return table::TableError{
            .code = table::TableErrorCode::Unavailable, .column = {}, .message = ui::detail::failureText(error)};
    }

    [[nodiscard]] static table::TableError unavailable() {
        return table::TableError{.code = table::TableErrorCode::Unavailable, .column = {}, .message = "no description"};
    }

    void heardRevision(std::string const& revision) {
        if (_description && revision != _description->revision && _staleRevisions.insert(revision).second &&
            _onStale) {
            _onStale();
        }
    }

    // The key held at each place of a page, or nothing at a place not held.
    [[nodiscard]] std::vector<std::optional<table::RowId>> slotsOfPage(std::int64_t page) const {
        auto const size = std::max<std::int64_t>(_options.pageSize, 1);
        std::vector<std::optional<table::RowId>> out;
        out.reserve(static_cast<std::size_t>(size));
        for (std::int64_t at = page * size; at < (page + 1) * size; ++at) {
            auto const* const key = _window.key(at);
            out.push_back(key == nullptr ? std::nullopt : std::optional{*key});
        }
        return out;
    }

    // The view's length as the mount holds it: the total, or nothing while none is known.
    [[nodiscard]] std::int64_t shownRows() const { return _window.total().value_or(0); }

    // The change a page reply made, by key, as operations each valid on the list the one before it leaves:
    // - a view that shrank loses its end first, so no row there is still held when the page's rows move in;
    // - the page's places both views have are diffed by key, so a row that moved keeps its identity, and a place
    //   held on one side only is the same row when that row is nowhere else on the other side (its cells arrived
    //   or left);
    // - a view that grew gains its end last, taken from the new view, once the rows that moved there have left
    //   their old places;
    // - the pages the reply evicted are changed places.
    // Rows of other pages do not move, since a page's places are fixed by its index.
    [[nodiscard]] table::ViewChange pageChange(std::int64_t page, std::int64_t rowsBefore,
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
        auto const places = static_cast<std::size_t>(
            std::clamp<std::int64_t>(std::min(rowsBefore, rowsAfter) - start, 0, size));
        std::unordered_map<table::RowId, std::size_t> wasAt;
        std::unordered_map<table::RowId, std::size_t> nowAt;
        for (std::size_t at = 0; at < places; ++at) {
            bool const unique = (!before[at] || wasAt.emplace(*before[at], at).second) &&
                                (!after[at] || nowAt.emplace(*after[at], at).second);
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
            if (before[at]) {
                idOf(*before[at]);
            }
            if (after[at]) {
                changed.push_back(idOf(*after[at]));
            }
        }
        auto const emptyBase = static_cast<std::uint32_t>(ids.size());
        auto const emptyAt = [emptyBase](std::size_t at) { return emptyBase + static_cast<std::uint32_t>(at); };
        std::vector<std::uint32_t> was(places);
        std::vector<std::uint32_t> now(places);
        for (std::size_t at = 0; at < places; ++at) {
            if (before[at]) {
                was[at] = ids.at(*before[at]);
            } else if (after[at] && !wasAt.contains(*after[at])) {
                was[at] = ids.at(*after[at]);
            } else {
                was[at] = emptyAt(at);
            }
            if (after[at]) {
                now[at] = ids.at(*after[at]);
            } else if (before[at] && !nowAt.contains(*before[at])) {
                now[at] = ids.at(*before[at]);
            } else {
                now[at] = emptyAt(at);
            }
        }
        auto const inPage =
            table::diffViews(was, now, changed, static_cast<std::size_t>(emptyBase) + places, kResetThreshold);
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

    void pageArrived(table::PageFetch const& fetch, std::string const& text) {
        auto const flight = std::ranges::find(_pageFlights, fetch, &PageFlight::fetch);
        std::uint64_t const stamp = flight == _pageFlights.end() ? 0 : flight->retained;
        std::erase_if(_pageFlights, [&fetch](PageFlight const& flight) { return flight.fetch == fetch; });
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

    void pageFailed(table::PageFetch const& fetch, std::exception_ptr const& error) {
        std::erase_if(_pageFlights, [&fetch](PageFlight const& flight) { return flight.fetch == fetch; });
        if (wasCancelled(error)) {
            return;
        }
        auto failure = failureOf(error);
        _window.fail(fetch, failure);
        _error = std::move(failure);
        status();
    }

    void rowsArrived(table::RowsFetch const& fetch, std::string const& text) {
        auto const flight = std::ranges::find(_rowsFlights, fetch, &RowsFlight::fetch);
        std::uint64_t const stamp = flight == _rowsFlights.end() ? 0 : flight->retained;
        std::erase_if(_rowsFlights, [&fetch](RowsFlight const& flight) { return flight.fetch == fetch; });
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
            auto const at = static_cast<std::size_t>(index);
            if (!change.ops.empty() && change.ops.back().first + change.ops.back().count == at) {
                ++change.ops.back().count;
            } else {
                change.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Changed, .first = at, .count = 1, .to = 0});
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

    void rowsFailed(table::RowsFetch const& fetch, std::exception_ptr const& error) {
        std::erase_if(_rowsFlights, [&fetch](RowsFlight const& flight) { return flight.fetch == fetch; });
        if (wasCancelled(error)) {
            return;
        }
        keyedFailed(failureOf(error));
    }

    // A keyed fetch that brought nothing: the rows stay as they were and may be out of date, so the pages are
    // fetched again.
    void keyedFailed(table::TableError error) {
        _error = std::move(error);
        _window.invalidate();
        pump();
        status();
    }

    void planned(table::RefreshPlan plan) {
        if (plan.kind == table::Refresh::Rows) {
            send(std::move(plan.rows));
        } else if (plan.kind == table::Refresh::Pages) {
            pump();
        }
    }

    // The values lists kept and in flight are bounded: a menu's pages and searches and an Apply's paging of every
    // value (up to `QueryLimits::maxLimit` values a reply) are what fill them, and the oldest is the least likely to
    // be asked for again.
    static constexpr std::size_t kMaxDistinctReplies = 64;
    static constexpr std::size_t kMaxDistinctFlights = 64;

    // Keeps a values list's reply by its query's body, the oldest going past the bound.
    void keepDistinct(std::string const& key, table::DistinctValues const& values) {
        if (!_distinct.contains(key)) {
            _distinctOrder.push_back(key);
        }
        _distinct.insert_or_assign(key, values);
        while (_distinctOrder.size() > kMaxDistinctReplies) {
            _distinct.erase(_distinctOrder.front());
            _distinctOrder.pop_front();
        }
    }

    // Drops every values list kept, and marks the replies in flight as asked for before the change.
    void forgetDistinct() {
        _distinct.clear();
        _distinctOrder.clear();
        ++_distinctEpoch;
    }

    // A change of the table's record. While the order awaits a level every event invalidates; `pump` spaces the
    // refetches.
    void heard(effects::RecordChange const& change) {
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

    // Fetches @p keys again, as a change event naming them does; marked instead while a run holds the refetches.
    void refetch(std::vector<table::RowId> const& keys) {
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
        planned(std::move(plan));
    }

    // Sends what the events marked while the refetches were held, as one refetch.
    void flushMarked() {
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

    // Takes a reply's word on which of its rows are retained and unmatched.
    void heardUnmatched(table::GenericPage const& page) {
        for (std::size_t at = 0; at < page.keys.size(); ++at) {
            if (std::ranges::binary_search(page.unmatched, static_cast<std::int64_t>(at))) {
                _unmatched.insert(page.keys[at]);
            } else {
                _unmatched.erase(page.keys[at]);
            }
        }
    }

    // The unmatched rows among those held now.
    void heldUnmatched() {
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

    void status() const {
        if (_onStatus) {
            _onStatus();
        }
    }

    void announce(table::ViewChange const& change) {
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

    Session* _session;
    WindowRowsOptions _options;
    table::PageWindow<table::GenericRow> _window;
    std::optional<table::TableDescription> _description;
    std::shared_ptr<table::UnitRegistry const> _units;
    std::map<std::string, table::ColumnInfo, std::less<>> _infos;
    bool _descriptionError = false;
    SortChain _sort;
    table::FilterSpec _filter;
    bool _restoring;
    bool _watched = false;
    std::size_t _unanswered = 0;
    bool _inferring = false;
    bool _retry = false;
    std::vector<WatchHolder> _watches;
    std::shared_ptr<void> _reconnect;
    std::size_t _unlisted = 0;
    std::function<void()> _onInferred;
    std::optional<table::TableError> _error;
    std::int64_t _first = 0;
    std::int64_t _count = 0;
    std::vector<std::string> _columns;
    bool _hasVisible = false;
    std::uint64_t _columnsStamp = 1;
    std::int64_t _anchor = -1;
    std::chrono::milliseconds _anchorSince{0};
    reactive::TimerHandle _settleTimer;
    std::optional<std::chrono::milliseconds> _awaitedLast;
    reactive::TimerHandle _awaitedTimer;
    std::vector<PageFlight> _pageFlights;
    // The pages the mount has been told of, so one the window evicts is reported as changed places.
    std::set<std::int64_t> _heldPages;
    std::vector<RowsFlight> _rowsFlights;
    // The latest reply's `viewRevision`: the membership a selection of the view names.
    std::string _viewRevision;
    // Keys a keyed reply asked for and left out of the view, until a reply holds them again or the filter changes.
    std::unordered_set<table::RowId> _outOfView;
    // The retained rows, in the order they were added, and as a set; each change advances the stamp, by which a
    // reply to a query of another set is told apart.
    std::vector<table::RowId> _retained;
    std::unordered_set<table::RowId> _retainedSet;
    std::uint64_t _retainStamp = 0;
    // The rows an action of this session works on, and those one could not change.
    RowStates _states;
    // Keys the latest reply that supplied them listed as unmatched, and those of them held.
    std::unordered_set<table::RowId> _unmatched;
    std::unordered_set<table::RowId> _unmatchedHeld;
    // The runs holding the refetches, the hold's timer, and what events marked meanwhile.
    std::set<std::uint64_t> _holding;
    reactive::TimerHandle _holdTimer;
    std::vector<table::RowId> _marked;
    bool _markedAll = false;
    std::set<std::string, std::less<>> _staleRevisions;
    std::function<void()> _onStale;
    std::function<void(std::span<table::RowId const>)> _onRowsAbsent;
    std::function<void(table::ViewChange const&)> _onChange;
    std::function<void()> _onStatus;
    mutable std::unordered_map<table::RowId, Entry> _memo;
    mutable WindowRowsStats _stats;
    // The stops of the key pages a range selection fetched, and of the fetch that locates a highlighted row.
    std::vector<std::weak_ptr<KeysRequest>> _keyRequests;
    // The stops of the keyed fetches `fetchRows` sent.
    std::vector<std::weak_ptr<RowsRequest>> _rowsRequests;
    std::map<std::string, table::DistinctValues, std::less<>> _distinct;
    std::deque<std::string> _distinctOrder;
    std::uint64_t _distinctEpoch = 0;
    std::map<std::uint64_t, std::stop_source> _distinctFlights;
    std::uint64_t _distinctFlight = 0;
    std::stop_source _locateStop;
    // Last, so it is the first to go: no reply or timer reaches a half-destroyed object.
    async::CallbackScope _scope;
};

}  // namespace morph::ui
