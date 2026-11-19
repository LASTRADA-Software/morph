// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ui/table_rows.hpp
/// @brief What a table's mount reads, whoever serves its rows: `TableRows`, over the engine (`EngineRows`) or over
///        a server's pages (`WindowRows`).
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
#include <morph/render/locale_format.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
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
};

/// @brief Where a key's row is in the view: its view index, nothing when the view has no row with the key, or why
///        that cannot be told.
using RowLocation = std::expected<std::optional<std::int64_t>, table::TableError>;

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

    /// @brief Sets the sort chain.
    /// @param sort The keys, most significant first; empty is the table's own order.
    /// @return The error that left the sort unchanged, or nothing.
    virtual std::expected<void, table::TableError> setSort(SortChain sort) = 0;

    /// @brief Sets the filter.
    /// @param filter The filter; an empty one shows every row.
    /// @return The error that left the filter unchanged, or nothing.
    virtual std::expected<void, table::TableError> setFilter(table::FilterSpec filter) = 0;

    /// @brief What the status line counts.
    /// @return The counts.
    [[nodiscard]] virtual TableCountsView counts() const = 0;

    /// @brief Sets the one receiver of the view's changes, called after the rows changed with the operations that
    ///        turn the old view into the new one.
    ///
    /// A single slot: a call replaces the handler, and the mount is what sets it. Anything else that needs the
    /// changes wraps this `TableRows` in a forwarding decorator and hands the wrapper to the mount.
    /// @param handler The receiver; empty to stop receiving.
    virtual void onChange(std::function<void(table::ViewChange const&)> handler) = 0;
};

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

/// @brief The text of a cell until the column's formatter shows it.
/// @param cell   The cell.
/// @param column The cell's column; not read yet.
/// @return `table::displayText(cell)`; empty for an empty or an unloaded cell.
[[nodiscard]] inline std::string standInCellText(table::Cell const& cell, table::TableColumn const& column) {
    static_cast<void>(column);
    return table::displayText(cell);
}

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

    /// @brief Every row of the view, counted exactly.
    /// @return The total, always known.
    [[nodiscard]] TableTotal total() const override {
        return TableTotal{.rows = static_cast<std::int64_t>(_engine->viewRowCount()),
                          .kind = table::TotalKind::Exact,
                          .known = true};
    }

    /// @brief The row at a view index, built once and kept until it changes.
    /// @param index The view index.
    /// @return The row; nothing past the view.
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override {
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

    /// @brief The engine's counts: its view, its source's rows, its undecided rows and whether it computes.
    /// @return The counts.
    [[nodiscard]] TableCountsView counts() const override {
        return TableCountsView{.view = total(),
                               .source = static_cast<std::int64_t>(_engine->sourceRowCount()),
                               .undecided = static_cast<std::int64_t>(_engine->undecidedRowCount()),
                               .pending = _engine->pending(),
                               .awaiting = {},
                               .failed = {}};
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
};

// ── Over a server's pages ───────────────────────────────────────────────────

/// @brief How a `WindowRows` reaches its table.
struct WindowRowsOptions {
    /// @brief The model's channel.
    std::shared_ptr<ModelChannel> channel;
    /// @brief The table's id: its actions are `Table.<table>` and `Table.<table>.describe`.
    std::string table;
    /// @brief The table's record, `<model type>.<table>`, watched for the window's life.
    std::string record;
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
};

/// @brief A table's rows over a server's pages: a `table::PageWindow` of generic rows, the table's description
///        and the session.
///
/// It watches the table's record first and sends no page fetch until the watch is answered, a description is set
/// and, when restoring, `release` is called. Every fetch goes through `Session::read` as `Table.<table>`, makes no
/// cache entry and is cancelled through its own stop token; its reply's text is read with
/// `table::readGenericPage` against the held description.
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
          _restoring{_options.restoring} {
        auto watching = _session->watchRecord(
            _options.record, _scope.token().guard([this](effects::RecordChange const& change) { heard(change); }));
        watching.then(_scope, [this](WatchHolder const& holder) {
            _watch = holder;
            _watched = true;
            pump();
        });
        watching.onError(_scope, [this](std::exception_ptr const&) {
            _watched = true;
            pump();
        });
    }

    WindowRows(WindowRows const&) = delete;
    WindowRows& operator=(WindowRows const&) = delete;
    WindowRows(WindowRows&&) = delete;
    WindowRows& operator=(WindowRows&&) = delete;

    /// @brief Cancels every fetch in flight.
    ~WindowRows() override {
        stopAll();
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

    /// @brief Takes the description as the `Table.<table>.describe` query holds it.
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

    /// @brief Sets what hears that `error()` or `counts()` may have changed: a fetch was sent, answered or failed,
    ///        or a reply or a description did not read. A failed fetch changes no row, so the view's `onChange` says
    ///        nothing of it; this is how a status line learns of it. Called after the rows took the event.
    /// @param handler The receiver; empty to stop receiving.
    void onStatus(std::function<void()> handler) { _onStatus = std::move(handler); }

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
    ///        fetch failed with; `Unavailable` while no description is held.
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
        auto reply = _session->read(_options.channel, "Table." + _options.table, *body, stop);
        reply.then(_scope, [this, done, stop](std::string const& text) {
            if (stop.stop_requested()) {
                return;
            }
            if (!_description) {
                done(std::unexpected(unavailable()));
                return;
            }
            auto read = table::readGenericPage(text, *_description, _units.get());
            if (!read) {
                done(std::unexpected(read.error()));
                return;
            }
            done(read->indices.empty() ? std::nullopt : std::optional<std::int64_t>{read->indices.front()});
        });
        reply.onError(_scope, [done, stop](std::exception_ptr const& error) {
            if (!stop.stop_requested() && !wasCancelled(error)) {
                done(std::unexpected(failureOf(error)));
            }
        });
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
            auto compiled = table::compileFilterGuarded(filter, infos, table::Services{}, _description->groups);
            if (!compiled) {
                return std::unexpected(std::move(compiled.error()));
            }
        }
        if (filter == _filter) {
            return {};
        }
        _filter = std::move(filter);
        restart();
        return {};
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
                               .failed = {failed.begin(), failed.end()}};
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

    struct RowsFlight {
        table::RowsFetch fetch;
        std::stop_source stop;
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

    // The request body: the node's body, its `query` record overlaid with the window's sort, filter, rows and
    // columns.
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
        ValueRecord members;
        if (auto const found = body.find("query"); found != body.end() && found->second.asRecord() != nullptr) {
            members = *found->second.asRecord();
        }
        for (auto const& [name, member] : *value->asRecord()) {
            members.insert_or_assign(name, member);
        }
        body.insert_or_assign("query", Value{std::move(members)});
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
        auto reply = _session->read(_options.channel, "Table." + _options.table, *body, stop.get_token());
        _pageFlights.push_back(PageFlight{.fetch = fetch, .stop = std::move(stop)});
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
        auto reply = _session->read(_options.channel, "Table." + _options.table, *body, stop.get_token());
        _rowsFlights.push_back(RowsFlight{.fetch = fetch, .stop = std::move(stop)});
        reply.then(_scope, [this, fetch](std::string const& text) { rowsArrived(fetch, text); });
        reply.onError(_scope, [this, fetch](std::exception_ptr const& error) { rowsFailed(fetch, error); });
        status();
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
        std::erase_if(_pageFlights, [&fetch](PageFlight const& flight) { return flight.fetch == fetch; });
        if (!_description) {
            return;
        }
        auto page = table::readGenericPage(text, *_description, _units.get());
        if (!page) {
            _window.fail(fetch, page.error());
            _error = std::move(page.error());
            status();
            return;
        }
        heardRevision(page->revision);
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
        std::erase_if(_rowsFlights, [&fetch](RowsFlight const& flight) { return flight.fetch == fetch; });
        if (!_description) {
            return;
        }
        auto page = table::readGenericPage(text, *_description, _units.get());
        if (!page) {
            keyedFailed(std::move(page.error()));
            return;
        }
        heardRevision(page->revision);
        auto const revision = _window.revision();
        auto const replaced = _window.patch(fetch, std::move(*page));
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

    // A change of the table's record. While the order awaits a level every event invalidates; `pump` spaces the
    // refetches.
    void heard(effects::RecordChange const& change) {
        if (!change.keys) {
            _window.invalidate();
            pump();
            return;
        }
        std::vector<table::RowId> keys;
        keys.reserve(change.keys->size());
        for (auto const& raw : *change.keys) {
            auto key = table::rowIdOf(raw);
            if (!key) {
                _window.invalidate();
                pump();
                return;
            }
            keys.push_back(std::move(*key));
        }
        planned(_window.refresh(keys));
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
    bool _retry = false;
    WatchHolder _watch;
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
    std::set<std::string, std::less<>> _staleRevisions;
    std::function<void()> _onStale;
    std::function<void(table::ViewChange const&)> _onChange;
    std::function<void()> _onStatus;
    mutable std::unordered_map<table::RowId, Entry> _memo;
    mutable WindowRowsStats _stats;
    // The stops of the key pages a range selection fetched, and of the fetch that locates a highlighted row.
    std::vector<std::weak_ptr<KeysRequest>> _keyRequests;
    std::stop_source _locateStop;
    // Last, so it is the first to go: no reply or timer reaches a half-destroyed object.
    async::CallbackScope _scope;
};

}  // namespace morph::ui
