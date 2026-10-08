// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/query.hpp
/// @brief `TableQuery`, `Page` and `table::apply`: a table's sort, filter and
///        page as a request body, and the server's reference implementation.
///
/// In `server` mode a list action's body embeds a `TableQuery` and its reply
/// is a `Page<Row>`. A server implements the action with `table::apply` over
/// its own rows, or with a database query that honours the same semantics
/// (spec 7 §5–6). `validate` applies the limits every server applies to an
/// untrusted query. On the client, `PageWindow` holds the pages fetched
/// around the visible rows.
///
/// See `docs/spec/table/engine.md`.

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/sort.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/quantity.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace morph::table {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- row and column indices are bounded by the snapshot's own counts, and the sort and filter loops index once per comparison, where at() would check every access

/// @brief Which rows of the view a request wants.
struct PageRequest {
    /// @brief First view row.
    std::int64_t offset = 0;
    /// @brief Rows wanted.
    std::int64_t limit = 200;

    /// @brief Member-wise equality.
    /// @param other The request to compare with.
    /// @return `true` when offset and limit match.
    [[nodiscard]] bool operator==(PageRequest const& other) const = default;
};

/// @brief A table's sort, filters and page, as a list action's body carries them.
///
/// The member that holds one is marked `x-table` in the action's schema, which
/// is how a client knows to send the table's state in it.
struct TableQuery {
    /// @brief The sort chain.
    std::vector<SortKey> sort;
    /// @brief The filter.
    FilterSpec filters;
    /// @brief The page.
    PageRequest page;

    /// @brief The schema keyword that marks a member of this type.
    /// @return `"x-table"`.
    [[nodiscard]] static constexpr std::string_view schemaMarker() noexcept { return "x-table"; }
};

/// @brief One page of a list action's reply.
/// @tparam Row The row type.
template <typename Row>
struct Page {
    /// @brief The rows, in view order.
    std::vector<Row> rows;
    /// @brief Rows in the whole filtered view; absent when the server did not
    ///        count them or the count is above its cap.
    std::optional<std::int64_t> total;
    /// @brief The view row of `rows[0]`.
    std::int64_t offset = 0;
};

/// @brief The bounds a server applies to every `TableQuery`, an untrusted body.
struct QueryLimits {
    /// @brief `page.limit` above this is clamped to it.
    std::int64_t maxLimit = 1000;
    /// @brief `page.offset` above this is refused.
    std::int64_t maxOffset = 10'000'000;
    /// @brief Sort keys.
    std::size_t maxSortKeys = 8;
    /// @brief Filtered columns.
    std::size_t maxFilterColumns = 32;
    /// @brief `include` plus `exclude` entries of one column or group.
    std::size_t maxEntries = 32;
    /// @brief Groups.
    std::size_t maxGroups = 8;
    /// @brief Columns of one group.
    std::size_t maxGroupColumns = 16;
    /// @brief Bytes of one filter value.
    std::size_t maxTextLength = 256;
    /// @brief A total above this is not reported.
    std::int64_t totalCap = 100'000;
};

namespace detail {

/// @brief A `LimitExceeded` error.
/// @param column  The column concerned, or empty.
/// @param message The description.
/// @return The error.
[[nodiscard]] inline TableError limitError(std::string column, std::string message) {
    return TableError{
        .code = TableErrorCode::LimitExceeded, .column = std::move(column), .message = std::move(message)};
}

/// @brief Whether any operand of an entry is longer than @p max bytes.
/// @param entry The entry.
/// @param max   The bound.
/// @return `true` when one is.
[[nodiscard]] inline bool entryTooLong(FilterEntry const& entry, std::size_t max) {
    auto const tooLong = [max](std::optional<std::string> const& value) { return value && value->size() > max; };
    return tooLong(entry.eq) || tooLong(entry.ne) || tooLong(entry.lt) || tooLong(entry.le) || tooLong(entry.gt) ||
           tooLong(entry.ge) || tooLong(entry.contains) || tooLong(entry.startsWith) || tooLong(entry.endsWith) ||
           (entry.between && ((*entry.between)[0].size() > max || (*entry.between)[1].size() > max));
}

/// @brief Whether any of @p entries has an operand longer than @p max bytes.
/// @param entries The entries.
/// @param max     The bound.
/// @return `true` when one has.
[[nodiscard]] inline bool anyTooLong(std::span<FilterEntry const> entries, std::size_t max) {
    return std::ranges::any_of(entries, [max](FilterEntry const& entry) { return entryTooLong(entry, max); });
}

/// @brief Checks that a column exists and the principal may read it.
/// @param columnId The column's id.
/// @param columns  The row schema's columns.
/// @param mayRead  Whether the principal may read a column; empty allows every column.
/// @return The error, or nothing.
[[nodiscard]] inline std::optional<TableError> checkColumn(std::string const& columnId,
                                                           std::span<ColumnInfo const> columns,
                                                           std::function<bool(std::string_view)> const& mayRead) {
    if (!findColumn(columns, columnId)) {
        return unknownColumn(columnId);
    }
    if (mayRead && !mayRead(columnId)) {
        return TableError{.code = TableErrorCode::ColumnNotReadable,
                          .column = columnId,
                          .message = "column " + columnId + " is not readable"};
    }
    return std::nullopt;
}

/// @brief Checks a filter's columns, entry counts and value lengths.
/// @param filters The filter.
/// @param columns The row schema's columns.
/// @param limits  The bounds.
/// @param mayRead Whether the principal may read a column.
/// @return The first violation, or nothing.
[[nodiscard]] inline std::optional<TableError> checkFilters(FilterSpec const& filters,
                                                            std::span<ColumnInfo const> columns,
                                                            QueryLimits const& limits,
                                                            std::function<bool(std::string_view)> const& mayRead) {
    if (filters.columns.size() > limits.maxFilterColumns) {
        return limitError({}, "too many filtered columns");
    }
    for (auto const& [columnId, filter] : filters.columns) {
        if (auto error = checkColumn(columnId, columns, mayRead)) {
            return error;
        }
        if (filter.include.size() + filter.exclude.size() > limits.maxEntries) {
            return limitError(columnId, "too many filter entries");
        }
        if (anyTooLong(filter.include, limits.maxTextLength) || anyTooLong(filter.exclude, limits.maxTextLength)) {
            return limitError(columnId, "filter value too long");
        }
    }
    if (filters.groups.size() > limits.maxGroups) {
        return limitError({}, "too many filter groups");
    }
    for (auto const& group : filters.groups) {
        if (group.columns.size() > limits.maxGroupColumns || group.include.size() > limits.maxEntries) {
            return limitError({}, "filter group too large");
        }
        for (auto const& columnId : group.columns) {
            if (auto error = checkColumn(columnId, columns, mayRead)) {
                return error;
            }
        }
        if (anyTooLong(group.include, limits.maxTextLength)) {
            return limitError({}, "filter value too long");
        }
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief Checks @p query against the table's columns and @p limits.
///
/// Every column named must be one of @p columns, a closed set, and one the
/// principal may read: a filter on a hidden column is an oracle for its value,
/// so hiding a column is not a security control. `page.limit` is clamped;
/// every other bound is a refusal.
/// @param query   The query, as received.
/// @param columns The row schema's columns.
/// @param limits  The bounds.
/// @param mayRead Whether the principal may read a column; empty allows every column.
/// @return The query with its limit clamped, or the first violation.
[[nodiscard]] inline std::expected<TableQuery, TableError> validate(
    TableQuery query, std::span<ColumnInfo const> columns, QueryLimits const& limits = {},
    std::function<bool(std::string_view)> const& mayRead = {}) {
    if (query.page.offset < 0 || query.page.offset > limits.maxOffset) {
        return std::unexpected(detail::limitError({}, "page.offset out of range"));
    }
    if (query.page.limit < 0) {
        return std::unexpected(detail::limitError({}, "page.limit is negative"));
    }
    query.page.limit = std::min(query.page.limit, limits.maxLimit);
    if (query.sort.size() > limits.maxSortKeys) {
        return std::unexpected(detail::limitError({}, "too many sort keys"));
    }
    for (auto const& key : query.sort) {
        if (auto error = detail::checkColumn(key.column, columns, mayRead)) {
            return std::unexpected(std::move(*error));
        }
    }
    if (auto error = detail::checkFilters(query.filters, columns, limits, mayRead)) {
        return std::unexpected(std::move(*error));
    }
    return query;
}

/// @brief Escapes `%`, `_` and the escape character for a SQL `LIKE`
///        pattern, so a database-backed server keeps them ordinary characters
///        as the filter semantics require.
/// @param text   A filter value.
/// @param escape The `ESCAPE` character the query declares.
/// @return The escaped text; wrap it in `%` for `contains` and so on.
[[nodiscard]] inline std::string escapeLikePattern(std::string_view text, char escape = '\\') {
    std::string out;
    out.reserve(text.size());
    for (char const character : text) {
        if (character == '%' || character == '_' || character == escape) {
            out.push_back(escape);
        }
        out.push_back(character);
    }
    return out;
}

/// @brief How `apply` runs.
struct ApplyOptions {
    /// @brief The bounds of `validate`.
    QueryLimits limits;
    /// @brief The server's collator, date parser and comparators. Its collator
    ///        decides the order of text in the pages it returns.
    Services services;
    /// @brief Whether the principal may read a column; empty allows every column.
    std::function<bool(std::string_view)> mayRead;
};

/// @brief The rows of one page, as source row indices.
struct PageRows {
    /// @brief Source rows, in view order.
    std::vector<std::size_t> rows;
    /// @brief Rows in the whole filtered view, unless above `QueryLimits::totalCap`.
    std::optional<std::int64_t> total;
    /// @brief The view row of `rows[0]`.
    std::int64_t offset = 0;
};

/// @brief Runs @p query over @p source in the calling thread: the reference
///        implementation of `server` mode.
/// @param source  The server's rows.
/// @param query   The request body's table query.
/// @param options Limits, services and column access.
/// @return The page, or the typed error the client shows.
[[nodiscard]] inline std::expected<PageRows, TableError> apply(DataSource const& source, TableQuery const& query,
                                                               ApplyOptions const& options = {}) {
    auto const columns = source.columns();
    auto checked = validate(query, columns, options.limits, options.mayRead);
    if (!checked) {
        return std::unexpected(std::move(checked.error()));
    }
    auto filter = compileFilter(checked->filters, columns, options.services);
    if (!filter) {
        return std::unexpected(std::move(filter.error()));
    }
    detail::JobInput input;
    input.snapshot = source.snapshot();
    input.columns.assign(columns.begin(), columns.end());
    input.services = options.services.forTask();
    input.keys.resize(columns.size());
    for (auto const& key : checked->sort) {
        if (auto const column = detail::findColumn(columns, key.column)) {
            input.needed.push_back(*column);
        }
    }
    for (auto const column : filter->columnsUsed()) {
        input.needed.push_back(column);
    }
    std::ranges::sort(input.needed);
    auto const [first, last] = std::ranges::unique(input.needed);
    input.needed.erase(first, last);
    input.sort = checked->sort;
    input.filter = std::make_shared<CompiledFilter const>(std::move(*filter));
    input.previousSnapshot = input.snapshot;
    detail::ViewJob job{std::move(input)};
    static_cast<void>(job.step(kNoDeadline, ::core::async::StopToken{}));
    auto const view = job.takeResult().view;

    PageRows out;
    out.offset = checked->page.offset;
    auto const count = static_cast<std::int64_t>(view.size());
    if (count <= options.limits.totalCap) {
        out.total = count;
    }
    auto const begin = std::min(checked->page.offset, count);
    auto const end = std::min(begin + checked->page.limit, count);
    for (auto i = begin; i < end; ++i) {
        out.rows.push_back(view[static_cast<std::size_t>(i)]);
    }
    return out;
}

// ── Typed rows ──────────────────────────────────────────────────────────────

namespace detail {

/// @brief `true` for `std::optional<T>`.
/// @tparam T A type.
template <typename T>
struct IsOptional : std::false_type {};
/// @brief `true` for `std::optional<T>`.
/// @tparam T The optional's value type.
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

/// @brief The column kind a member type gives.
/// @tparam T The member type.
/// @return The kind.
template <typename T>
[[nodiscard]] constexpr ColumnKind kindOf() {
    using V = std::remove_cvref_t<T>;
    if constexpr (IsOptional<V>::value) {
        return kindOf<typename V::value_type>();
    } else if constexpr (std::is_same_v<V, bool>) {
        return ColumnKind::Bool;
    } else if constexpr (std::integral<V>) {
        return ColumnKind::Integer;
    } else if constexpr (std::floating_point<V>) {
        return ColumnKind::Number;
    } else if constexpr (std::is_same_v<V, math::Rational>) {
        return ColumnKind::Decimal;
    } else if constexpr (units::isQuantity<V>) {
        return ColumnKind::Quantity;
    } else if constexpr (std::is_same_v<V, time::DateTime> || std::is_same_v<V, time::Timestamp>) {
        return ColumnKind::DateTime;
    } else {
        return ColumnKind::Text;
    }
}

/// @brief A member value as a cell.
/// @tparam T The member type.
/// @param value The member.
/// @return The cell.
template <typename T>
[[nodiscard]] Cell toCell(T const& value) {
    using V = std::remove_cvref_t<T>;
    if constexpr (IsOptional<V>::value) {
        return value ? toCell(*value) : Cell{};
    } else if constexpr (std::is_same_v<V, bool> || std::is_same_v<V, std::string> ||
                         std::is_same_v<V, math::Rational>) {
        return Cell{value};
    } else if constexpr (std::integral<V>) {
        return Cell{static_cast<std::int64_t>(value)};
    } else if constexpr (std::floating_point<V>) {
        return Cell{static_cast<double>(value)};
    } else if constexpr (units::isQuantity<V>) {
        return value.value() ? Cell{*value.value()} : Cell{};
    } else if constexpr (std::is_same_v<V, time::DateTime>) {
        return Cell{std::chrono::floor<std::chrono::seconds>(value.value).time_since_epoch().count()};
    } else if constexpr (std::is_same_v<V, time::Timestamp>) {
        return value.value ? toCell(*value.value) : Cell{};
    } else {
        // Enums with a glaze name, and anything else glaze writes, as text.
        std::string text;
        if (glz::write_json(value, text)) {
            return Cell{};
        }
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
            text = text.substr(1, text.size() - 2);
        }
        return Cell{std::move(text)};
    }
}

/// @brief Reads one column's cell from a row.
/// @tparam Row The row type.
template <typename Row>
using CellReader = Cell (*)(Row const&);

/// @brief One cell reader per reflected member, in member order.
/// @tparam Row The row type.
/// @return The readers.
template <typename Row>
[[nodiscard]] std::vector<CellReader<Row>> cellReaders() {
    constexpr auto count = glz::reflect<Row>::size;
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::vector<CellReader<Row>>{+[](Row const& row) -> Cell {
            auto tie = glz::to_tie(row);
            return toCell(glz::get_member(row, glz::get<I>(tie)));
        }...};
    }(std::make_index_sequence<count>{});
}

/// @brief One column per reflected member, its kind from the member's type.
/// @tparam Row The row type.
/// @return The columns.
template <typename Row>
[[nodiscard]] std::vector<ColumnInfo> reflectedColumns() {
    using Members = decltype(glz::to_tie(std::declval<Row&>()));
    constexpr auto count = glz::reflect<Row>::size;
    std::vector<ColumnInfo> out;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (out.push_back(ColumnInfo{.id = std::string{glz::reflect<Row>::keys[I]},
                                  .kind = kindOf<std::remove_cvref_t<decltype(glz::get_member(
                                      std::declval<Row const&>(), glz::get<I>(std::declval<Members>())))>>(),
                                  .comparator = {},
                                  .format = {}}),
         ...);
    }(std::make_index_sequence<count>{});
    return out;
}

}  // namespace detail

/// @brief A key projection: the `RowId` of a typed row.
/// @tparam Row The row type.
template <typename Row>
using RowKey = std::function<RowId(Row const&)>;

/// @brief A snapshot over a shared, immutable vector of typed rows.
/// @tparam Row A glaze-reflectable aggregate.
template <typename Row>
class RowsSnapshot final : public RowSnapshot {
public:
    /// @brief Wraps @p rows.
    /// @param rows    The rows; shared, never modified.
    /// @param key     The key projection.
    /// @param readers One cell reader per column.
    RowsSnapshot(std::shared_ptr<std::vector<Row> const> rows, RowKey<Row> key,
                 std::shared_ptr<std::vector<detail::CellReader<Row>> const> readers)
        : _rows{std::move(rows)}, _key{std::move(key)}, _readers{std::move(readers)} {}

    /// @brief The number of rows.
    /// @return Row count.
    [[nodiscard]] std::size_t rowCount() const override { return _rows->size(); }

    /// @brief A row's key.
    /// @param row Row index.
    /// @return The key.
    [[nodiscard]] RowId rowId(std::size_t row) const override { return _key((*_rows)[row]); }

    /// @brief One cell.
    /// @param row    Row index.
    /// @param column Column index.
    /// @return The cell.
    [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override {
        return (*_readers)[column]((*_rows)[row]);
    }

    /// @brief Hands rows `[first, last)` of @p column to @p sink in runs.
    /// @param column Column index.
    /// @param sink   Receives the cells.
    /// @param first  First row.
    /// @param last   One past the last row.
    void readColumn(std::size_t column, ColumnSink& sink, std::size_t first, std::size_t last) const override {
        constexpr std::size_t kRun = 256;
        std::vector<Cell> run;
        run.reserve(kRun);
        auto const reader = (*_readers)[column];
        while (first < last) {
            auto const end = std::min(first + kRun, last);
            run.clear();
            for (auto row = first; row < end; ++row) {
                run.push_back(reader((*_rows)[row]));
            }
            sink.cells(first, run);
            first = end;
        }
    }

private:
    std::shared_ptr<std::vector<Row> const> _rows;
    RowKey<Row> _key;
    std::shared_ptr<std::vector<detail::CellReader<Row>> const> _readers;
};

/// @brief A `DataSource` over typed rows: one column per reflected member,
///        its kind inferred from the member's type.
///
/// `bool` is `Bool`; other integers `Integer`; floating point `Number`;
/// `Rational` `Decimal`; a `Quantity` `Quantity`; `DateTime` and `Timestamp`
/// `DateTime`; everything else `Text` (an enum by its glaze name). A
/// `std::optional` member's empty state is an empty cell. `setRows` replaces
/// the rows and notifies `Reset`.
/// @tparam Row A glaze-reflectable aggregate.
template <typename Row>
class RowsSource final : public DataSource {
public:
    /// @brief Builds the source.
    /// @param rows  The rows.
    /// @param key   The key projection.
    /// @param kinds Column kinds that replace the inferred ones, by column id
    ///              (for example `Date` for an `int64` of epoch days).
    RowsSource(std::shared_ptr<std::vector<Row> const> rows, RowKey<Row> key,
               std::map<std::string, ColumnKind, std::less<>> const& kinds = {})
        : _columns{detail::reflectedColumns<Row>()},
          _readers{std::make_shared<std::vector<detail::CellReader<Row>> const>(detail::cellReaders<Row>())},
          _key{std::move(key)},
          _snapshot{std::make_shared<RowsSnapshot<Row>>(std::move(rows), _key, _readers)} {
        for (auto& column : _columns) {
            if (auto const found = kinds.find(column.id); found != kinds.end()) {
                column.kind = found->second;
            }
        }
    }

    /// @brief The columns.
    /// @return One per reflected member.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _columns; }

    /// @brief The current rows; a pointer copy.
    /// @return The snapshot.
    [[nodiscard]] std::shared_ptr<RowSnapshot const> snapshot() const override { return _snapshot; }

    /// @brief Registers @p listener.
    /// @param listener The listener.
    void subscribe(ChangeListener& listener) override { _listeners.push_back(&listener); }

    /// @brief Removes @p listener.
    /// @param listener The listener.
    void unsubscribe(ChangeListener& listener) override { std::erase(_listeners, &listener); }

    /// @brief Replaces the rows and notifies `Reset`.
    /// @param rows The new rows.
    void setRows(std::shared_ptr<std::vector<Row> const> rows) {
        _snapshot = std::make_shared<RowsSnapshot<Row>>(std::move(rows), _key, _readers);
        auto const listeners = _listeners;
        for (auto* listener : listeners) {
            listener->rowsChanged(RowChange{.kind = ChangeKind::Reset, .rows = {}});
        }
    }

private:
    std::vector<ColumnInfo> _columns;
    std::shared_ptr<std::vector<detail::CellReader<Row>> const> _readers;
    RowKey<Row> _key;
    std::shared_ptr<RowsSnapshot<Row> const> _snapshot;
    std::vector<ChangeListener*> _listeners;
};

/// @brief Runs @p query over typed rows and returns the page of rows: what a
///        server's list action returns.
/// @tparam Row A glaze-reflectable aggregate.
/// @param rows    The server's rows; read during the call only.
/// @param key     The key projection.
/// @param query   The request body's table query.
/// @param options Limits, services and column access.
/// @param kinds   Column kinds that replace the inferred ones.
/// @return The page, or the typed error the client shows.
template <typename Row>
[[nodiscard]] std::expected<Page<Row>, TableError> apply(
    std::vector<Row> const& rows, RowKey<Row> key, TableQuery const& query, ApplyOptions const& options = {},
    std::map<std::string, ColumnKind, std::less<>> const& kinds = {}) {
    // Borrowed for the call: an aliasing pointer that owns nothing.
    std::shared_ptr<std::vector<Row> const> const borrowed{std::shared_ptr<void>{}, &rows};
    RowsSource<Row> const source{borrowed, std::move(key), kinds};
    auto page = apply(source, query, options);
    if (!page) {
        return std::unexpected(std::move(page.error()));
    }
    Page<Row> out;
    out.total = page->total;
    out.offset = page->offset;
    out.rows.reserve(page->rows.size());
    for (auto const row : page->rows) {
        out.rows.push_back(rows[row]);
    }
    return out;
}

// ── Server mode on the client ───────────────────────────────────────────────

/// @brief The pages a `server`-mode table holds: those around the visible
///        rows, up to a bound, plus the request state and the last error.
///
/// The client calls `visible` as the view scrolls and sends a request for each
/// page it returns, then hands each reply to `accept` (or its error to
/// `fail`). A change of sort or filter is a new query: call `reset`.
/// @tparam Row The row type.
template <typename Row>
class PageWindow {
public:
    /// @brief An empty window.
    /// @param pageSize Rows per page (default 200).
    /// @param maxPages Pages held at most; the farthest from the visible rows go first.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - page size then page count, both documented above
    explicit PageWindow(std::int64_t pageSize = 200, std::size_t maxPages = 8)
        : _pageSize{std::max<std::int64_t>(pageSize, 1)}, _maxPages{std::max<std::size_t>(maxPages, 1)} {}

    /// @brief Declares the visible rows; returns the pages to fetch.
    ///
    /// Covers the visible rows and one page either side, skipping pages held,
    /// pages in flight, and pages past a known total. The returned pages are
    /// marked in flight.
    /// @param first First visible view row.
    /// @param count Visible rows.
    /// @return The requests to send.
    [[nodiscard]] std::vector<PageRequest> visible(std::int64_t first, std::int64_t count) {
        _visibleFirst = std::max<std::int64_t>(first, 0);
        _visibleCount = std::max<std::int64_t>(count, 0);
        auto const firstPage = std::max<std::int64_t>((_visibleFirst / _pageSize) - 1, 0);
        auto const lastPage = ((_visibleFirst + std::max<std::int64_t>(_visibleCount, 1) - 1) / _pageSize) + 1;
        std::vector<PageRequest> out;
        for (auto page = firstPage; page <= lastPage; ++page) {
            if (_total && page * _pageSize >= *_total) {
                break;
            }
            if (_pages.contains(page) || std::ranges::find(_inFlight, page) != _inFlight.end()) {
                continue;
            }
            _inFlight.push_back(page);
            out.push_back(PageRequest{.offset = page * _pageSize, .limit = _pageSize});
        }
        return out;
    }

    /// @brief Stores a reply.
    /// @param page The page the server returned.
    void accept(Page<Row> page) {
        auto const index = page.offset / _pageSize;
        std::erase(_inFlight, index);
        _total = page.total;
        _error.reset();
        _pages.insert_or_assign(index, std::move(page.rows));
        evict();
    }

    /// @brief Records a failed request; the error is shown until a reply arrives.
    /// @param request The request that failed.
    /// @param error   The server's error.
    void fail(PageRequest const& request, TableError error) {
        std::erase(_inFlight, request.offset / _pageSize);
        _error = std::move(error);
    }

    /// @brief Drops every page, request and error: the query changed.
    void reset() {
        _pages.clear();
        _inFlight.clear();
        _total.reset();
        _error.reset();
    }

    /// @brief The row at a view row, when its page is held.
    /// @param index View row.
    /// @return The row, or null.
    [[nodiscard]] Row const* row(std::int64_t index) const {
        if (index < 0) {
            return nullptr;
        }
        auto const found = _pages.find(index / _pageSize);
        if (found == _pages.end()) {
            return nullptr;
        }
        auto const offset = static_cast<std::size_t>(index % _pageSize);
        return offset < found->second.size() ? &found->second[offset] : nullptr;
    }

    /// @brief The view's total rows, when the server reported it.
    /// @return The total.
    [[nodiscard]] std::optional<std::int64_t> total() const noexcept { return _total; }

    /// @brief The last error, until a reply clears it.
    /// @return The error, or nothing.
    [[nodiscard]] std::optional<TableError> const& error() const noexcept { return _error; }

    /// @brief Pages held.
    /// @return Page count.
    [[nodiscard]] std::size_t pagesHeld() const noexcept { return _pages.size(); }

    /// @brief Whether any request is in flight.
    /// @return `true` while a page is awaited.
    [[nodiscard]] bool pending() const noexcept { return !_inFlight.empty(); }

private:
    void evict() {
        auto const center = (_visibleFirst + (_visibleCount / 2)) / _pageSize;
        while (_pages.size() > _maxPages) {
            auto farthest = _pages.begin();
            for (auto at = _pages.begin(); at != _pages.end(); ++at) {
                if (std::abs(at->first - center) > std::abs(farthest->first - center)) {
                    farthest = at;
                }
            }
            _pages.erase(farthest);
        }
    }

    std::int64_t _pageSize;
    std::size_t _maxPages;
    std::map<std::int64_t, std::vector<Row>> _pages;
    std::vector<std::int64_t> _inFlight;
    std::optional<std::int64_t> _total;
    std::optional<TableError> _error;
    std::int64_t _visibleFirst = 0;
    std::int64_t _visibleCount = 0;
};

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

/// @brief JSON names of `morph::table::TableErrorCode`, for a typed error on the wire.
template <>
struct glz::meta<morph::table::TableErrorCode> {
    using enum morph::table::TableErrorCode;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value =
        glz::enumerate("unknownColumn", UnknownColumn, "unsupportedOperator", UnsupportedOperator, "invalidValue",
                       InvalidValue, "invalidSpec", InvalidSpec, "limitExceeded", LimitExceeded, "columnNotReadable",
                       ColumnNotReadable, "unavailable", Unavailable);
};
