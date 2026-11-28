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
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <morph/table/row_selection.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/table_config.hpp>
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
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): a view onto the model's row.
    table::RowId const& key;
    /// @brief The row as a record keyed by column id. A field the model does not hold for the row is absent.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): a view onto the model's row.
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
[[nodiscard]] MORPH_API std::string selectionJsonOf(TableSelection const& selection);

/// @brief What the status line says when select-all is refused.
/// @param refusal Why.
/// @param locale  The formatting facts: the count's group separator.
/// @return `"Select all is available once <n> rows are decided"`, or with no undecided row `"Select all is available
///         once <levels joined by " and "> data has loaded"`, or with neither `"Select all is available once the
///         rows have loaded"`: the view has not answered its current query yet.
[[nodiscard]] MORPH_API std::string selectAllRefusalText(SelectAllRefusal const& refusal, FormatContext const& locale);

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
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one walk down the reply along the catalog's path.
[[nodiscard]] MORPH_API ChangedRowsValue changedRowsOf(Value const& reply, ActionInfo const& action,
                                                       std::string_view record);

/// @brief The rows of a table an action of this session is working on, and those an action could not change.
///
/// A run's rows are busy until it settles, whatever its outcome: the keys of a key selection or of a row, or every
/// row of a selection of the view but its `except`. Runs are kept apart, so two runs over one row keep it busy until
/// both settle. A failed row holds its message until its mark is cleared.
class MORPH_API RowStates {
public:
    /// @brief Makes rows busy for a run; a run named again adds to its keys.
    /// @param run  The run's id, unique in the session.
    /// @param keys The rows' keys.
    void busyKeys(std::uint64_t run, std::vector<table::RowId> keys);

    /// @brief Makes every row of a selection of the view busy for a run, but its `except`.
    /// @param run    The run's id, unique in the session.
    /// @param except The keys the selection leaves out.
    /// @param count  The rows the selection held when the run started: what `busyCount` counts for it.
    void busyView(std::uint64_t run, std::vector<table::RowId> except, std::int64_t count);

    /// @brief Ends a run: its rows are no longer busy for it, whatever its outcome.
    /// @param run The run's id; one that holds nothing is ignored.
    void settle(std::uint64_t run);

    /// @brief Marks rows an action could not change, each with its message; a row marked already takes the new one.
    /// @param rows The rows.
    void fail(std::span<table::FailedRow const> rows);

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
    /// @param target   The row's new key.
    void move(table::RowId const& from, table::RowId const& target);

    /// @brief Whether a row is busy: a key run holds it, or a view run does not leave it out.
    /// @param key The row's key.
    /// @return `true` while any run in flight works on it.
    [[nodiscard]] bool isBusy(table::RowId const& key) const;

    /// @brief A row's failed mark.
    /// @param key The row's key.
    /// @return Its message, or nothing for a row not marked.
    [[nodiscard]] std::optional<std::string> failure(table::RowId const& key) const;

    /// @brief How many rows are busy.
    /// @return With no view run in flight, the distinct keys of the key runs; with one, the largest `count` among
    ///         the view runs plus the distinct keys of key runs that every view run leaves out, which are outside its
    ///         rows.
    [[nodiscard]] std::int64_t busyCount() const;

    /// @brief The rows marked failed.
    /// @return Their keys, in the order they failed.
    [[nodiscard]] std::vector<table::RowId> failedKeys() const;

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
class MORPH_API TableRows {
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
    virtual async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(
        std::int64_t first, std::int64_t count) = 0;

    /// @brief Finds the view index of the row with a key, under the current sort and filters, whether or not the row
    ///        is held: what focus follows its key by when a new order moves it away from the rows in view.
    /// @param key  The row's key.
    /// @param stop Once a stop is requested, @p done is not called.
    /// @param done Called once on the owner, never before this returns, with the row's view index, nothing when the
    ///        view has no row with @p key, or the error that kept the model from telling.
    virtual void locateRow(table::RowId key, ::core::async::StopToken stop,
                           std::function<void(RowLocation const&)> done) = 0;

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
    [[nodiscard]] virtual bool leftView(table::RowId const& key) const;

    /// @brief Adds rows the view keeps although the filter rejects them: the rows an action of this session changed.
    ///        All of them are added, or, when they would take the set past `table::QueryLimits::maxRetained`, none.
    ///        A change of the filter clears them.
    /// @param keys The rows' keys; a key retained already counts once.
    /// @return `Retained`, or `OverBound` with the set unchanged. Rows that cannot retain keep nothing and answer
    ///         `Retained`.
    [[nodiscard]] virtual RetainResult retain(std::span<table::RowId const> keys);

    /// @brief Drops every retained row: the view holds the rows the filter admits again.
    virtual void clearRetained() {}

    /// @brief Whether a held row is retained and the filter rejects it.
    /// @param key The row's key; a row's look has its key, not its view index.
    /// @return `true` for a held row the latest reply that supplied it listed as unmatched; `false` for a key the
    ///         rows do not hold.
    [[nodiscard]] virtual bool unmatched(table::RowId const& key) const;

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
        std::int64_t first, std::int64_t count, std::span<std::string const> columns);

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
    /// @param target   The row's new key.
    virtual void moveKey(table::RowId const& from, table::RowId const& target) = 0;

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
[[nodiscard]] MORPH_API std::optional<std::string> lockText(TableRowRef const& row,
                                                            table::TableDescription const& description);

/// @brief Whether a row takes no cell edit: an action of this session is working on it, or it is locked.
/// @param rows        The rows.
/// @param index       The row's view index.
/// @param description The table's description.
/// @return `true` for a busy or locked row; `false` for a row the rows do not hold.
[[nodiscard]] MORPH_API bool refusesEdit(TableRows const& rows, std::int64_t index,
                                         table::TableDescription const& description);

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
[[nodiscard]] MORPH_API Value doubleValue(double value);

/// @brief A `Date` cell's days since 1970-01-01 as `YYYY-MM-DD`.
/// @param days The days.
/// @return The date's text; the number's own text past the years 0 to 9999.
[[nodiscard]] MORPH_API Value dateValue(std::int64_t days);

/// @brief A `DateTime` cell's seconds since the epoch as a timestamp.
/// @param seconds The seconds.
/// @return The timestamp; the number's own text when its milliseconds do not fit an `int64`.
[[nodiscard]] MORPH_API Value dateTimeValue(std::int64_t seconds);

}  // namespace detail

/// @brief A cell as the document reads it.
///
/// Empty and unloaded cells are `null`. An integer is an `Int`, a `Date` column's the `String` `YYYY-MM-DD` and a
/// `DateTime` column's a `Timestamp`. A `Rational` is a `Decimal` with its places. A `double` is the `Decimal` its
/// shortest fixed-notation text spells, or that text's shortest form as a `String` when it spells none
/// `table::parseDecimal` reads (non-finite, more than 18 fraction digits, beyond an `int64` numerator). Text is a
/// `String`, a flag a `Bool`, and a quantity the `Decimal` of its amount in the column's canonical unit. A measure is
/// a `Quantity` of its amount in its own unit, named by the unit's id in @p units; its amount alone, a `Decimal`, when
/// @p units is null or does not hold the unit.
/// @param cell   The cell.
/// @param column The cell's column, whose kind decides how an integer reads.
/// @param units  The registry a measure's unit number is named through; null for none.
/// @return The value.
[[nodiscard]] MORPH_API Value cellValue(table::Cell const& cell, table::ColumnInfo const& column,
                                        table::UnitRegistry const* units = nullptr);

/// @brief The status line's counts part, its numbers grouped with @p format's separator.
///
/// Nothing while no total is known; `"<view> so far, <undecided> loading"` while rows are undecided;
/// `"<selected> of <view> selected"` with a selection; `"<view> or more"` under a lower bound; otherwise `"1 row"` or
/// `"<view> rows"`. A lower bound's `<view>` is followed by `" or more"` in the selection's text too.
/// @param counts   The model's counts.
/// @param selected Rows selected.
/// @param format   The formatting facts: their numbers' group separator.
/// @return The text.
[[nodiscard]] MORPH_API std::string countsText(TableCountsView const& counts, std::int64_t selected,
                                               FormatContext const& format);

// ── Over the engine ─────────────────────────────────────────────────────────

/// @brief A table's rows over a `table::Engine`: every row is present.
///
/// `visible` does nothing and `keysOf` completes at once; a change is the engine's `ViewChange`. A row's record is
/// built by `RecordOf` once per source row of the engine's `viewSnapshot()` and kept: dropped for the rows an
/// `Updated` change names, and whole after any other change of the source, when the engine's view first shows it.
/// The view's snapshot is matched to the change that made it by pointer, so a source's `snapshot()` must return the
/// same pointer until its rows change, as `VectorSource` does; with a source that returns a new one on every call,
/// every logged change is applied whenever the view's snapshot moves.
class MORPH_API EngineRows final : public TableRows, private table::ChangeListener {
public:
    /// @brief Rows over a new engine.
    /// @param source   The rows. Must not be null.
    /// @param recordOf What builds a row's key and record.
    /// @param options  How the engine runs. `owner` must not be null: `keysOf` delivers there.
    /// @throws std::invalid_argument when @p source or `options.owner` is null.
    EngineRows(std::shared_ptr<table::DataSource> source, RecordOf recordOf, table::EngineOptions options);

    EngineRows(EngineRows const&) = delete;
    EngineRows& operator=(EngineRows const&) = delete;
    EngineRows(EngineRows&&) = delete;
    EngineRows& operator=(EngineRows&&) = delete;

    /// @brief Stops hearing the engine and the source.
    ~EngineRows() override;

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
    void setRestoring(bool restoring);

    /// @brief Whether the rows are held back (`setRestoring`).
    /// @return Whether they are.
    [[nodiscard]] bool restoring() const noexcept { return _restoring; }

    /// @brief Every row of the view, counted exactly; not known while restoring.
    /// @return The total.
    [[nodiscard]] TableTotal total() const override;

    /// @brief The row at a view index, built once and kept until it changes.
    /// @param index The view index.
    /// @return The row; nothing past the view, and nothing while restoring.
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override;

    /// @brief The levels of the row's columns that hold an unloaded cell.
    /// @param index The view index.
    /// @return The levels, sorted; none failed, since a source's level has no failure of its own.
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override;

    /// @brief Does nothing: every row is present.
    /// @param first   Unused.
    /// @param count   Unused.
    /// @param columns Unused.
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override;

    /// @brief The keys of a range of view rows, at once.
    /// @param first The first view row.
    /// @param count The rows.
    /// @return The keys, delivered on the engine's owner; `InvalidSpec` for a range outside the view.
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t first,
                                                                                          std::int64_t count) override;

    /// @brief The rows of keys in the engine's view, read from its snapshot at once.
    /// @param keys    The rows' keys.
    /// @param columns The column ids whose cells each row carries; a column the engine lacks gives an empty cell.
    /// @return The rows the view has, in the order of @p keys, delivered on the engine's owner.
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) override;

    /// @brief The engine's view as a selection takes it. A table held whole selects all by keys, so the lowering
    ///        does not ask; the answer is the engine's filter and the digest of its view's keys.
    /// @return The view; never refused, since every row is decided.
    [[nodiscard]] std::expected<ViewSelection, SelectAllRefusal> selectView() const override;

    /// @brief How many rows a selection holds: every row is here, so the count is exact.
    /// @param selection The selection.
    /// @return The keys' number, or the view's rows less the keys of `except` in it.
    [[nodiscard]] SelectedCount selectedCount(TableSelection const& selection) const override;

    /// @brief A range of view rows, read from the engine at once.
    /// @param first   The first view row.
    /// @param count   The rows.
    /// @param columns The column ids whose cells each row carries.
    /// @return The rows in view order, delivered on the engine's owner; the part of the range outside the view has
    ///         none.
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> rowsOf(
        std::int64_t first, std::int64_t count, std::span<std::string const> columns) override;

    /// @brief Finds the view index of the row with a key in the engine's view, as it is now.
    /// @param key  The row's key.
    /// @param stop Once a stop is requested, @p done is not called.
    /// @param done Called on the engine's owner with the row's view index, or nothing when the view lacks it.
    void locateRow(table::RowId key, ::core::async::StopToken stop,
                   std::function<void(RowLocation const&)> done) override;

    /// @brief Sets the engine's sort.
    /// @param sort The keys.
    /// @return The engine's error, or nothing.
    std::expected<void, table::TableError> setSort(SortChain sort) override {
        return _engine->setSort(std::move(sort));
    }

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
        table::DistinctRequest request) override;

    /// @brief The engine's counts: its view, its source's rows, its undecided rows and whether it computes.
    /// @return The counts.
    [[nodiscard]] TableCountsView counts() const override;

    /// @brief Adds keys to the engine's retained rows, all or none.
    /// @param keys The rows' keys.
    /// @return `OverBound` when they would take the set past `table::QueryLimits::maxRetained`.
    [[nodiscard]] RetainResult retain(std::span<table::RowId const> keys) override;

    /// @brief Drops the engine's retained rows.
    void clearRetained() override;

    /// @brief Whether the engine's view row of a key is retained and rejected by the filter.
    /// @param key The row's key.
    /// @return `false` for a key not in the view.
    [[nodiscard]] bool unmatched(table::RowId const& key) const override;

    /// @brief The rows an action of this session is working on, and those an action could not change.
    /// @return The rows' states.
    [[nodiscard]] RowStates& states() override { return _states; }

    /// @brief The rows an action of this session is working on, and those an action could not change.
    /// @return The rows' states.
    [[nodiscard]] RowStates const& states() const override { return _states; }

    /// @brief Moves a row's place among the engine's retained rows and its states to its new key.
    /// @param from The row's old key.
    /// @param target   The row's new key.
    void moveKey(table::RowId const& from, table::RowId const& target) override;

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
                                                                    table::EngineOptions const& options);

    void rowsChanged(table::RowChange const& change) override;

    // Brings the memo to the engine's view snapshot: every change up to the one that made it is applied. The
    // changes after it are kept, since a record built now reads the view's snapshot, which they are not in yet.
    void sync() const;

    [[nodiscard]] Entry const* entryAt(std::int64_t index) const;

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
class MORPH_API ValueRowsCollator final : public table::TextCollator {
public:
    /// @brief The fold of @p text, a NUL, then @p text: case-folded order first, then the text's own bytes.
    /// @param text UTF-8 text.
    /// @return The collation key.
    [[nodiscard]] std::string sortKey(std::string_view text) const override;

    /// @brief @p text folded as the default collator folds it, which filters match against.
    /// @param text UTF-8 text.
    /// @return The folded text.
    [[nodiscard]] std::string fold(std::string_view text) const override { return folded(text); }

    /// @brief Texts folded so far, by `sortKey` and `fold` together.
    /// @return The count.
    [[nodiscard]] std::size_t folds() const noexcept { return _folds.load(std::memory_order_relaxed); }

private:
    // The fold of a text: the one this thread made last when it was of the same text, else a new one, counted.
    [[nodiscard]] MORPH_PROCESS_UNIQUE std::string folded(std::string_view text) const;

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
inline constexpr std::string_view kGroupRunColumn =
    "\x1F"
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
[[nodiscard]] MORPH_API std::uint32_t exactPlacesOf(std::int64_t den);

/// @brief A quantity, or a record of exactly `num`, `den` and `unit` (integers and a text), read as a measure.
/// @param value The value.
/// @return Its amount and unit, or nothing for any other value.
[[nodiscard]] MORPH_API std::optional<MeasureParts> measureOf(Value const& value);

/// @brief The kind a value votes its column.
/// @param value The value.
/// @return The vote; `None` for `null`, a list and a record that is not a measure.
[[nodiscard]] MORPH_API ValueVote voteOf(Value const& value);

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
[[nodiscard]] MORPH_API table::ColumnKind inferredKind(std::array<std::size_t, kValueVotes> const& counts,
                                                       std::span<ValueVote const> votes);

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
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one conversion per column kind, kept together.
[[nodiscard]] MORPH_API ConvertedCell convertedCell(Value const& value, table::ColumnKind kind,
                                                    table::UnitRegistry const* units);

/// @brief Whether two cells are the same, a decimal's places included.
/// @param left  A cell.
/// @param right Another.
/// @return `true` when they hold the same value with the same places.
[[nodiscard]] MORPH_API bool sameCell(table::Cell const& left, table::Cell const& right);

/// @brief Whether two row values are one object: a record or a list by its storage, any other value by equality.
/// @param left  A value.
/// @param right Another.
/// @return `true` when nothing in @p right can differ from @p left.
[[nodiscard]] MORPH_API bool sameObject(Value const& left, Value const& right);

/// @brief The rows of a `ValueRowsSource` at one moment: each row's value and key, and the cells of the columns in
///        use, shared with the source until it changes them.
class MORPH_API ValueRowsSnapshot final : public table::RowSnapshot {
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
    [[nodiscard]] table::Cell cell(std::size_t row, std::size_t column) const override;

    /// @brief Hands rows `[first, last)` of @p column to @p sink in one run.
    /// @param column Column index.
    /// @param sink   Receives the cells.
    /// @param first  First row.
    /// @param last   One past the last row.
    void readColumn(std::size_t column, table::ColumnSink& sink, std::size_t first, std::size_t last) const override;

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
class MORPH_API ValueRowsSource final : public table::DataSource {
public:
    /// @brief A source of no rows over @p columns.
    /// @param runtime The runtime each `sortBy` is tracked on and duplicates are reported to; must outlive the source.
    /// @param columns The document's columns, each id once.
    /// @param keyOf   Reads a row's key.
    /// @param units   The registry `Quantity` cells' units are found in; null: none, and every quantity is invalid.
    ValueRowsSource(reactive::Runtime& runtime, std::vector<ValueColumn> columns,
                    std::function<Key(Value const& row)> keyOf, std::shared_ptr<table::UnitRegistry const> units = {});

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
    [[nodiscard]] std::shared_ptr<table::RowSnapshot const> snapshot() const override;

    /// @brief Registers @p listener.
    /// @param listener The listener.
    void subscribe(table::ChangeListener& listener) override { _listeners.add(listener); }

    /// @brief Removes @p listener; safe from inside a notification.
    /// @param listener The listener.
    void unsubscribe(table::ChangeListener& listener) override { _listeners.remove(listener); }

    /// @brief Takes a new list: matched to the held rows by key, then by the cells of the columns in use.
    /// @param rows The rows, in source order.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one diff whose steps share their working vectors.
    void assign(ValueList rows);

    /// @brief Groups the rows by the `group` part's `by`, read per row, and adds the hidden column
    ///        `kGroupRunColumn`; notifies `Reset`.
    ///
    /// `by` is read untracked: what it reads besides the row regroups the rows when `setGroup` or `assign` is called
    /// again.
    /// @param groupBy    The `by`; empty: no groups, and the hidden column goes.
    /// @param units The unit table two quantities of a run's `by` are compared through: the screen's, so 1 kg and
    ///              1000 g are one run.
    void setGroup(std::function<Value(Value const& row)> groupBy, UnitTable units);

    /// @brief The run a source row belongs to: its index among the runs of adjacent rows whose `by` is the same
    ///        element as their run's first row's, in source order.
    /// @param sourceRow The row.
    /// @return The run's index; 0 when the table is not grouped or the row is past the rows.
    [[nodiscard]] std::size_t runOf(std::size_t sourceRow) const;

    /// @brief Each run's heading record `{key, count, first, last, items}`, with its heading key in
    ///        `detail::kGroupHeadingField`, by run index, as `TableView::runs` holds them.
    /// @return The records; none when the table is not grouped.
    [[nodiscard]] std::span<Value const> runRecords() const noexcept { return _runRecords; }

    /// @brief A sort, a filter or a values list began to read @p column: its cells are derived when this is its
    ///        first use, and a `Reset` is notified so the engine reads them.
    /// @param column The column's id; one the source does not have is ignored.
    void use(std::string_view column);

    /// @brief A use of @p column ended; at its last, its cells and tracked `sortBy`s go. Nothing is notified.
    /// @param column The column's id; one not in use is ignored.
    void unuse(std::string_view column);

    /// @brief What `EngineRows` reads a row's key and record through: the snapshot's key and the row's value.
    /// @return The reader; it holds nothing of the source.
    [[nodiscard]] static RecordOf recordOf();

    /// @brief Cells held: one per row of each column in use, and of the run column while grouped.
    /// @return The count.
    [[nodiscard]] std::size_t cellsHeld() const noexcept;

    /// @brief Cells derived so far: a row's field read, or its `sortBy` run, for a column in use.
    /// @return The count.
    [[nodiscard]] std::size_t cellsDerived() const noexcept { return _derived; }

    /// @brief Cells of @p column, a column in use, that are not of its kind.
    /// @param column The column's id.
    /// @return The count; 0 for a column not in use.
    [[nodiscard]] std::size_t invalidCells(std::string_view column) const;

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

    [[nodiscard]] std::optional<std::size_t> columnOf(std::string_view columnId) const;

    void notify(table::RowChange const& change) { _listeners.notify(change); }

    // The `group`'s `by` of a row, read untracked: a change of the list is what regroups it.
    [[nodiscard]] Value readBy(Value const& row) const {
        return _runtime->untracked([&] { return _group(row); });
    }

    // A field column's value for a row.
    [[nodiscard]] Value fieldOf(std::size_t column, Value const& row) const;

    // The value a column's cell of a row converts: the tracked `sortBy`'s last, or the row's field.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): column index, then row index, in every call here.
    [[nodiscard]] Value valueOf(std::size_t column, std::size_t row, Value const& input) const;

    // A new tracked `sortBy` for a row: its first run derives the value.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): column index, then row index, in every call here.
    std::unique_ptr<Tracker> track(std::size_t column, std::size_t row, Value const& input);

    // A tracked `sortBy`'s run: the first stores the value, a later one updates the row's cell.
    void rerun(Tracker& tracker);

    // One notification for the tracked `sortBy`s that ran: `Reset` when every row's ran or a kind changed.
    void flushTracked();

    // Changes one row's vote and returns the column's kind after it.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): column index, then row index, in every call here.
    table::ColumnKind vote(Held& held, std::size_t column, std::size_t row, detail::ValueVote next);

    [[nodiscard]] table::ColumnKind kindOf(std::size_t column, Held const& held) const;

    // The column's cells, copied first when a snapshot still shares them.
    static std::vector<table::Cell>& writable(Held& held);

    // Every cell of a column converted again for its kind, from the values held.
    void reconvert(std::size_t column);

    // A column's first use: every row's cell derived.
    void rebuild(std::size_t column);

    // A held column over a new list: kept rows keep their cells, others are derived and compared. Marks the rows
    // whose cell changed in @p updated; returns whether the column's kind changed.
    bool reassign(std::size_t column, std::vector<Value> const& rows, std::vector<std::size_t> const& was,
                  std::vector<bool> const& same, std::vector<bool>& updated);

    // Each row's run, each run's first row and each run's heading record, from the `by` values held. A run's key is
    // its first row's `by`, as the interpreter keys it.
    void buildRuns(std::vector<Value> const& rows);

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
[[nodiscard]] MORPH_API std::optional<std::int64_t> jsonInteger(glz::generic_u64 const& member);

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
[[nodiscard]] MORPH_API std::optional<JsonDecimal> jsonDecimal(glz::generic_u64 const& member);

/// @brief The kind a member votes its column.
/// @param member The member.
/// @return The vote; `None` for `null`, an array and any object that is not a decimal or a measure.
[[nodiscard]] MORPH_API JsonVote jsonVoteOf(glz::generic_u64 const& member);

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
[[nodiscard]] MORPH_API table::ColumnKind inferredJsonKind(std::span<glz::generic_u64 const> rows,
                                                           std::string_view column);

/// @brief A member as a cell of a column of @p kind.
///
/// `null` is an empty cell. An integer is an `int64` (beyond its range a `double`), a `Rational` of 0 places under
/// `Decimal` and a `double` under `Number`; a fractional number a `double`; a string its text, under `Date` its days
/// when it is `YYYY-MM-DD` and under `DateTime` its seconds rounded down when it is ISO-8601; `{num, den, dp}` its
/// `Rational`; with a `unit` under `Measure` a measure in @p units' number for the unit (`kUnknownUnit`, invalid, when
/// @p units lacks it). Anything else is the member's JSON text, which only a `Text` column reads as valid.
/// @param member The member.
/// @param kind   The column's kind.
/// @param units  The registry a measure's unit is found in; null finds none.
/// @param named  The unit ids of the page's measures, by number: what a measure found adds to.
/// @return The cell.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per column kind.
[[nodiscard]] MORPH_API table::Cell typedCell(glz::generic_u64 const& member, table::ColumnKind kind,
                                              table::UnitRegistry const* units,
                                              std::map<table::UnitId, std::string>& named);

/// @brief A key member read as a row's id: an integer, or a string.
/// @param member The member.
/// @return The id, or nothing for any other member.
[[nodiscard]] MORPH_API std::optional<table::RowId> typedKey(glz::generic_u64 const& member);

/// @brief A typed list action's reply, read as a DOM, as a generic page over @p description's columns.
/// @param dom         The reply.
/// @param description The description: its columns, their kinds and its key.
/// @param units       The registry a measure's unit is found in; null resolves through `registryOf(description)`,
///                    built only when a column is `Measure`.
/// @return The page, or `InvalidValue` when a row is not an object or has no key, or `unloadedRows` or `indices` do
///         not fit the rows, or what `registryOf` refuses.
[[nodiscard]] MORPH_API std::expected<table::GenericPage, table::TableError> typedPageOf(
    TypedPageDom const& dom, table::TableDescription const& description, table::UnitRegistry const* units);

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
[[nodiscard]] MORPH_API std::vector<table::UnitInfo> unlistedUnits(std::span<glz::generic_u64 const> rows,
                                                                   table::TableDescription const& description,
                                                                   table::UnitRegistry const* units, std::size_t room);

/// @brief Reads a typed list action's reply text as a DOM.
/// @param json The reply.
/// @return The DOM, or `InvalidValue` when the text is not a page.
[[nodiscard]] MORPH_API std::expected<TypedPageDom, table::TableError> typedPageDom(std::string_view json);

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
[[nodiscard]] MORPH_API std::expected<table::GenericPage, table::TableError> readTypedPage(
    std::string_view json, table::TableDescription const& description, table::UnitRegistry const* units = nullptr);

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
class MORPH_API WindowRows final : public TableRows {
public:
    /// @brief Rows of a table, watching its record from now.
    /// @param session The session; must outlive the rows.
    /// @param options The channel, the table and the window's bounds.
    WindowRows(Session& session, WindowRowsOptions options);

    WindowRows(WindowRows const&) = delete;
    WindowRows& operator=(WindowRows const&) = delete;
    WindowRows(WindowRows&&) = delete;
    WindowRows& operator=(WindowRows&&) = delete;

    /// @brief Cancels every fetch in flight.
    // NOLINTNEXTLINE(bugprone-exception-escape): only allocation can throw, and that ends the process either way.
    ~WindowRows() override;

    /// @brief Takes the table's description: fits the sort, the filters and the visible columns to it, and starts
    ///        a new query when it is another revision than the one held.
    /// @param description The description, its revision set.
    void setDescription(table::TableDescription description);

    /// @brief Takes the description as the `Table.<id>.describe` query holds it.
    ///
    /// A description's numbers are integers or exact decimals, which a `Value` holds without loss, so it is read
    /// back from the value's canonical JSON. One that does not read sets `error()` to `InvalidValue` and stops
    /// every page fetch until a description that reads is set.
    /// @param described The query's value.
    void setDescription(Value const& described);

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
    void onRowsAbsent(std::function<void(std::span<table::RowId const>)> handler) {
        _onRowsAbsent = std::move(handler);
    }

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
    void release();

    /// @brief Fetches the visible pages again with `TableQuery::retry`, so failed level loads are tried at once.
    ///        Every fetch of that refetch carries the flag, one the timing rules hold back included.
    void retry();

    /// @brief The last error: a fetch that failed, a reply or a description that did not read. Cleared by the next
    ///        page taken.
    /// @return The error, or nothing.
    [[nodiscard]] std::optional<table::TableError> const& error() const noexcept { return _error; }

    /// @brief What the rows have done.
    /// @return The counters.
    [[nodiscard]] WindowRowsStats const& stats() const noexcept { return _stats; }

    /// @brief The view's rows, as the latest page reply said.
    /// @return The total; not known before a reply.
    [[nodiscard]] TableTotal total() const override;

    /// @brief The held row at a view index, as a record of the columns the latest `visible` named.
    /// @param index The view index.
    /// @return The row, or nothing while it is not held.
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override;

    /// @brief The view rows the window holds, as whole pages: what a reader of every held row walks instead of the
    ///        view. A page's last rows may lie past the view's end, where `row` answers nothing.
    /// @return Each held page's first view row and its rows, ascending.
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> heldRanges() const override;

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
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override;

    /// @brief The keys of a range of view rows, for a range selection: the rows held give theirs, and every page of
    ///        the range with a row not held is fetched naming no columns, so its reply carries keys alone. The pages
    ///        so fetched are not held.
    /// @param first The first view row.
    /// @param count The rows.
    /// @return The keys once every page has answered; `InvalidSpec` for a negative range, `Unavailable` when a page
    ///         failed or a row of the range is in none.
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t first,
                                                                                          std::int64_t count) override;

    /// @brief Finds the view index of the row with a key: a keyed fetch naming no columns, under the current sort and
    ///        filters.
    /// @param key  The row's key.
    /// @param stop Stops the fetch; once a stop is requested, @p done is not called.
    /// @param done Called on the owner with the row's view index, nothing when the reply has no row, or the error the
    ///        fetch failed with; `Unavailable` while no description is held, and `InvalidValue` when the reply holds
    ///        the row without its index.
    void locateRow(table::RowId key, ::core::async::StopToken stop,
                   std::function<void(RowLocation const&)> done) override;

    /// @brief Fetches rows by key: keyed queries under the current sort and filters naming @p columns, at most
    ///        `WindowRowsOptions::maxKeys` keys each, through the session's read, which makes no cache entry. The
    ///        replies are not held.
    /// @param keys    The rows' keys.
    /// @param columns The column ids whose cells each row carries.
    /// @return The rows the replies hold, with their view indices; a key no reply holds is left out. A cell of a
    ///         level the reply names the row as lacking is `table::Unloaded`. `Unavailable` while no description is
    ///         held or when a fetch failed, and `InvalidValue` for a reply that gives a row no index.
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) override;

    /// @brief Finds a row's view index, as `locateRow` does, stopping the fetch an earlier call still has in flight,
    ///        whose handler is then never called.
    /// @param key  The row's key.
    /// @param done Called with the row's view index, or nothing when the view has no row with @p key or the fetch
    ///        failed.
    void locate(table::RowId key, std::function<void(std::optional<std::int64_t>)> done);

    /// @brief Stops a `locate` fetch in flight; its handler is never called.
    void stopLocating();

    /// @brief Sets the query's sort: a new query.
    /// @param sort The keys.
    /// @return `UnknownColumn`, `InvalidSpec` for a column that is not sortable or named twice, or `LimitExceeded`
    ///         past the table's sort bound, checked against the description held; the sort is then unchanged.
    std::expected<void, table::TableError> setSort(SortChain sort) override;

    /// @brief Sets the query's filter: a new query.
    /// @param filter The filter.
    /// @return The error compiling it against the description held; the filter is then unchanged.
    std::expected<void, table::TableError> setFilter(table::FilterSpec filter) override;

    /// @brief The distinct values of a column: a query carrying `distinct` under the held filter. Its reply is kept
    ///        by the query's body until the table's record changes, so a menu opened again sends nothing.
    /// @param request The request.
    /// @return The values, delivered on the owner; `Unavailable` without a description, for a table whose
    ///         description says it lists no values, and over a typed list action, whose reply holds rows alone.
    async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest request) override;

    /// @brief The window's counts: its total, undecided rows, awaited and failed levels, and whether a fetch is in
    ///        flight. The rows before the filter are not known.
    /// @return The counts.
    [[nodiscard]] TableCountsView counts() const override;

    /// @brief The rows an action of this session is working on, and those an action could not change.
    /// @return The rows' states.
    [[nodiscard]] RowStates& states() override { return _states; }

    /// @brief The rows an action of this session is working on, and those an action could not change.
    /// @return The rows' states.
    [[nodiscard]] RowStates const& states() const override { return _states; }

    /// @brief Moves a row's place among the retained rows and its states to its new key. Nothing is fetched: the
    ///        change event of the action names both keys.
    /// @param from The row's old key.
    /// @param target   The row's new key.
    void moveKey(table::RowId const& from, table::RowId const& target) override;

    /// @brief Adds keys to the retained rows, all or none; each query from then on carries the set in `retain`.
    ///
    /// The keys added are refetched as a change event's keys are, so the rows that left the filter come back into
    /// the view; while a run holds the refetches, they are marked with the events'.
    /// @param keys The rows' keys.
    /// @return `OverBound` when they would take the set past `table::QueryLimits::maxRetained`; the set is then
    ///         unchanged.
    [[nodiscard]] RetainResult retain(std::span<table::RowId const> keys) override;

    /// @brief Drops every retained row, and fetches the held pages again without them.
    void clearRetained() override;

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
    void holdRefetches(std::uint64_t run) override;

    /// @brief Ends @p run's hold; when no run holds any more, what the events marked goes as one refetch.
    /// @param run The run's id.
    void releaseRefetches(std::uint64_t run) override;

    /// @brief The view as a selection takes it: the held filter and the latest reply's revision.
    ///
    /// Refused while the view's membership is open: rows are undecided, or an awaited level is one a filtered
    /// column needs. A level only the sort needs refuses nothing, since order is not membership. Refused too, with
    /// nothing undecided and no level, until a reply to the current filter has named its revision.
    /// @return The view, or the refusal with the undecided rows and the filtered columns' awaited levels.
    [[nodiscard]] std::expected<ViewSelection, SelectAllRefusal> selectView() const override;

    /// @brief How many rows a selection holds, as far as the window knows.
    ///
    /// A selection of the view holds the view's total less each key of `except` the window holds; a key owed a
    /// refetch still subtracts. The count is exact when the total is, and every key of `except` is held and owed
    /// nothing, or known to have left the view.
    /// @param selection The selection.
    /// @return The count.
    [[nodiscard]] SelectedCount selectedCount(TableSelection const& selection) const override;

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
        std::int64_t first, std::int64_t count, std::span<std::string const> columns) override;

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
        ::core::async::StopSource stop;
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
        std::vector<::core::async::StopSource> stops;
    };

    void keysAnswered(KeysRequest& request, std::int64_t page,
                      std::expected<std::vector<table::RowId>, table::TableError> keys);

    // The range's keys: a fetched page's where one was fetched, the window's otherwise.
    void finishKeys(KeysRequest& request);

    // A `fetchRows` in the making: the rows its replies held so far, and how many replies it still waits for.
    struct RowsRequest {
        async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>>::Promise promise;
        std::vector<FetchedRow> rows;
        std::size_t waiting = 0;
        std::optional<table::TableError> failed;
        std::vector<::core::async::StopSource> stops;
    };

    static void rowsAnswered(RowsRequest& request, std::expected<std::vector<FetchedRow>, table::TableError> rows);

    static void finishRows(RowsRequest& request);

    // A keyed reply's rows, each with the cells of @p columns in that order; a cell of a level the reply names its
    // row as lacking is unloaded, as it is `null` on the wire.
    [[nodiscard]] std::expected<std::vector<FetchedRow>, table::TableError> fetchedOf(
        table::GenericPage& page, std::vector<std::string> const& columns) const;

    struct RowsFlight {
        table::RowsFetch fetch;
        ::core::async::StopSource stop;
        // The retained set the query carried, by `_retainStamp`.
        std::uint64_t retained = 0;
    };

    [[nodiscard]] std::chrono::milliseconds now() const;

    [[nodiscard]] bool ready() const noexcept {
        return _watched && _description.has_value() && !_descriptionError && !_restoring && _hasVisible;
    }

    [[nodiscard]] std::optional<table::TableError> sortError(SortChain const& sort) const;

    // A new query: the window drops every page, every fetch in flight is stopped, and the view is reset.
    void restart();

    void stopAll();

    [[nodiscard]] bool heldPage(std::int64_t page) const;

    // Whether a fetch goes now rather than when the visible rows settle.
    [[nodiscard]] bool due(table::PageFetch const& fetch, std::chrono::milliseconds instant) const;

    // Whether a refetch of a held page waits: the order awaits a level and the last refetch was too recent.
    [[nodiscard]] bool spaced(std::chrono::milliseconds instant) const;

    // Asks the window for what the visible rows need, cancels what it no longer covers, sends what is due and
    // leaves the rest to the settle timer, or, for a refetch while the order awaits a level, to the refetch timer.
    void pump();

    // The request body: the node's body, its query member (`query`, or a typed list action's `queryMember`)
    // overlaid with the window's sort, filter, rows and columns.
    [[nodiscard]] std::expected<Value, table::TableError> bodyOf(table::TableQuery const& query) const;

    [[nodiscard]] table::TableQuery queryOf(table::PageRequest page, std::vector<std::string> const& columns) const;

    void send(table::PageFetch const& fetch);

    void send(table::RowsFetch const& fetch);

    [[nodiscard]] std::string actionName() const {
        return _options.action.empty() ? "Table." + _options.table : _options.action;
    }

    void answered();

    // A reply's page against the held description. A typed list action's first reply with rows also fixes the kinds
    // of the columns `inferKinds` names, from the same DOM the page is read from.
    [[nodiscard]] std::expected<table::GenericPage, table::TableError> decode(std::string const& text);

    // A unit a `Measure` column's member names that the description does not list is its own canonical unit, in a
    // dimension of its own named by its id, as an expression reads a unit missing from the screen's table. It is
    // added after the units listed, so the numbers held cells carry keep their meaning; at most
    // `detail::kMaxUnlistedUnits` are, the rest staying unknown.
    void listUnits(std::span<glz::generic_u64 const> rows);

    // The kinds the first rows give the columns `inferKinds` names. The description keeps its revision, so the
    // query, the window and the fetches in flight stand; only the records made of the cells are made again.
    void inferKinds(std::span<glz::generic_u64 const> rows);

    [[nodiscard]] static bool wasCancelled(std::exception_ptr const& error);

    [[nodiscard]] static table::TableError failureOf(std::exception_ptr const& error);

    [[nodiscard]] static table::TableError unavailable();

    void heardRevision(std::string const& revision);

    // The key held at each place of a page, or nothing at a place not held.
    [[nodiscard]] std::vector<std::optional<table::RowId>> slotsOfPage(std::int64_t page) const;

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
    // NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-easily-swappable-parameters): page, count
    [[nodiscard]] table::ViewChange pageChange(std::int64_t page, std::int64_t rowsBefore,
                                               std::vector<std::optional<table::RowId>> const& before,
                                               std::set<std::int64_t> const& heldBefore) const;

    void pageArrived(table::PageFetch const& fetch, std::string const& text);

    void pageFailed(table::PageFetch const& fetch, std::exception_ptr const& error);

    void rowsArrived(table::RowsFetch const& fetch, std::string const& text);

    void rowsFailed(table::RowsFetch const& fetch, std::exception_ptr const& error);

    // A keyed fetch that brought nothing: the rows stay as they were and may be out of date, so the pages are
    // fetched again.
    void keyedFailed(table::TableError error);

    void planned(table::RefreshPlan const& plan);

    // The values lists kept and in flight are bounded: a menu's pages and searches and an Apply's paging of every
    // value (up to `QueryLimits::maxLimit` values a reply) are what fill them, and the oldest is the least likely to
    // be asked for again.
    static constexpr std::size_t kMaxDistinctReplies = 64;
    static constexpr std::size_t kMaxDistinctFlights = 64;

    // Keeps a values list's reply by its query's body, the oldest going past the bound.
    void keepDistinct(std::string const& key, table::DistinctValues const& values);

    // Drops every values list kept, and marks the replies in flight as asked for before the change.
    void forgetDistinct();

    // A change of the table's record. While the order awaits a level every event invalidates; `pump` spaces the
    // refetches.
    void heard(effects::RecordChange const& change);

    // Fetches @p keys again, as a change event naming them does; marked instead while a run holds the refetches.
    void refetch(std::vector<table::RowId> const& keys);

    // Sends what the events marked while the refetches were held, as one refetch.
    void flushMarked();

    // Takes a reply's word on which of its rows are retained and unmatched.
    void heardUnmatched(table::GenericPage const& page);

    // The unmatched rows among those held now.
    void heldUnmatched();

    void status() const;

    void announce(table::ViewChange const& change);

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
    std::map<std::uint64_t, ::core::async::StopSource> _distinctFlights;
    std::uint64_t _distinctFlight = 0;
    ::core::async::StopSource _locateStop;
    // Last, so it is the first to go: no reply or timer reaches a half-destroyed object.
    async::CallbackScope _scope;
};

}  // namespace morph::ui
