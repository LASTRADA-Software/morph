// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/query.hpp
/// @brief `TableQuery`, `Page` and `table::apply`: a table's sort, filter and
///        page as a request body, and the server's reference implementation.
///
/// In `server` mode a list action's body embeds a `TableQuery` and its reply
/// is a `Page<Row>`. A server implements the action with `table::apply` over
/// its own rows (spec 7 §5–6). `validate` applies the limits every server
/// applies to an untrusted query. On the client, `PageWindow` holds the pages fetched
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
#include <limits>
#include <map>
#include <memory>
#include <morph/detail/reflected_member.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/sort.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/quantity.hpp>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
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
    /// @brief The columns the reply carries, by id. Absent means every column the caller may read; empty means
    ///        none, and the reply then holds keys alone. `validate` puts it in the table's column order, each id
    ///        once.
    std::optional<std::vector<std::string>> columns;
    /// @brief When set, the reply holds only the view's rows with these keys, in view order, and `page` does not
    ///        apply. A key not in the view (removed, rejected or undecided) has no row in the reply.
    std::optional<std::vector<RowId>> keys;
    /// @brief The user asked for failed loads to be tried again: the level caches load the failed keys this query
    ///        needs without waiting out their retry delay.
    bool retry = false;

    /// @brief The schema keyword that marks a member of this type.
    /// @return `"x-table"`.
    [[nodiscard]] static constexpr std::string_view schemaMarker() noexcept { return "x-table"; }
};

/// @brief What a page's `total` says about the filtered view.
enum class TotalKind : std::uint8_t {
    Exact,    ///< `total` is the count of the filtered view.
    AtLeast,  ///< `total` is a lower bound: the view has at least that many rows.
};

/// @brief A row of a page with levels that are not loaded.
struct UnloadedRow {
    /// @brief The row's index into `Page::rows`.
    std::int64_t index = 0;
    /// @brief The row's unloaded levels among those the reply needed: sorted, without duplicates, never the base
    ///        level.
    std::vector<std::string> levels;
    /// @brief The subset of `levels` whose load failed; usually empty.
    std::vector<std::string> failed;

    /// @brief Member-wise equality.
    /// @param other The row to compare with.
    /// @return `true` when index, levels and failed levels match.
    [[nodiscard]] bool operator==(UnloadedRow const& other) const = default;
};

/// @brief What is not loaded for one row: views into the page or window that holds the row.
struct UnloadedLevels {
    /// @brief The row's unloaded levels, sorted.
    std::span<std::string const> levels;
    /// @brief The subset of `levels` whose load failed.
    std::span<std::string const> failed;
};

/// @brief One page of a list action's reply.
///
/// The members from `unloadedRows` to `failed` say which of its cells are still coming, and `indices` places a
/// keyed reply's rows in the view. An absent `total` and an empty `failed` or `indices` are left out of the JSON
/// written; a page read without the members after `totalKind` is a fully loaded page that is not a keyed reply.
/// @tparam Row The row type.
template <typename Row>
struct Page {
    /// @brief The rows, in view order.
    std::vector<Row> rows;
    /// @brief Rows in the whole filtered view, or a lower bound (see `totalKind`);
    ///        absent when the server did not count them.
    std::optional<std::int64_t> total;
    /// @brief The view row of `rows[0]`.
    std::int64_t offset = 0;
    /// @brief Whether `total` is the count or a lower bound; meaningful only when `total` is present.
    TotalKind totalKind = TotalKind::Exact;
    /// @brief The rows of this reply that have unloaded levels, ascending by index. A row not listed is fully
    ///        loaded; the members of a listed row at those levels hold whatever the server left, and are not read.
    std::vector<UnloadedRow> unloadedRows;
    /// @brief Rows the filter can neither admit nor reject until a level loads. They are in no page.
    std::int64_t undecided = 0;
    /// @brief The order levels with a load wanted or in flight for at least one row. Not empty means the order and
    ///        the membership of the view may change without any row being edited.
    std::vector<std::string> awaiting;
    /// @brief The order levels whose load failed for at least one row, and is not being tried again yet.
    std::vector<std::string> failed;
    /// @brief A keyed reply only: the view row of each of `rows`.
    std::vector<std::int64_t> indices;
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
    /// @brief Entries of `TableQuery::columns`, counted as received, duplicates included.
    std::size_t maxColumns = 512;
    /// @brief Entries of `TableQuery::keys`, counted as received, duplicates included.
    std::size_t maxKeys = 1000;
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

/// @brief The ids of @p asked in the order of @p columns, each once.
///
/// An id @p columns lacks is dropped; `validate` refuses such an id before it
/// calls this.
/// @param columns The table's columns, in their declared order.
/// @param asked   Column ids in any order, duplicates allowed.
/// @return The ids of @p columns that @p asked names, in the order of @p columns.
[[nodiscard]] inline std::vector<std::string> orderedColumns(std::span<ColumnInfo const> columns,
                                                             std::vector<std::string> const& asked) {
    std::vector<std::string_view> wanted(asked.begin(), asked.end());
    std::ranges::sort(wanted);
    std::vector<std::string> out;
    out.reserve(std::min(asked.size(), columns.size()));
    for (auto const& column : columns) {
        if (std::ranges::binary_search(wanted, std::string_view{column.id})) {
            out.push_back(column.id);
        }
    }
    return out;
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
/// every other bound is a refusal. A `columns` list is put in the order of
/// @p columns with each id once, so every caller of `validate` or `apply`
/// sees it that way; an absent list stays absent and an empty one empty.
/// A keyed query has no page, so `keys` with a non-zero `page.offset` is
/// `InvalidSpec`; the keys are bounded by `maxKeys`, and a text key by
/// `maxTextLength`.
/// @param query   The query, as received.
/// @param columns The row schema's columns.
/// @param limits  The bounds.
/// @param mayRead Whether the principal may read a column; empty allows every column.
/// @return The query with its limit clamped and its `columns` ordered, or the first violation.
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
    if (query.columns) {
        if (query.columns->size() > limits.maxColumns) {
            return std::unexpected(detail::limitError({}, "too many columns"));
        }
        for (auto const& columnId : *query.columns) {
            if (auto error = detail::checkColumn(columnId, columns, mayRead)) {
                return std::unexpected(std::move(*error));
            }
        }
        query.columns = detail::orderedColumns(columns, *query.columns);
    }
    if (query.keys) {
        if (query.page.offset != 0) {
            return std::unexpected(
                TableError{.code = TableErrorCode::InvalidSpec, .column = {}, .message = "a keyed query has no page"});
        }
        if (query.keys->size() > limits.maxKeys) {
            return std::unexpected(detail::limitError({}, "too many keys"));
        }
        auto const tooLong = [&limits](RowId const& key) {
            auto const* const text = std::get_if<std::string>(&key);
            return text != nullptr && text->size() > limits.maxTextLength;
        };
        if (std::ranges::any_of(*query.keys, tooLong)) {
            return std::unexpected(detail::limitError({}, "key too long"));
        }
    }
    return query;
}

/// @brief The data levels a query needs, by what it needs them for.
struct LevelsNeeded {
    /// @brief Levels of the sorted and filtered columns: the view's membership and order depend on them for every
    ///        row.
    std::vector<std::string> order;
    /// @brief Levels of the columns the query names: needed only for the rows of the reply.
    std::vector<std::string> display;
};

namespace detail {

/// @brief @p levels sorted, each once.
/// @param levels Level names, views into strings that outlive the call.
/// @return The levels as strings.
[[nodiscard]] inline std::vector<std::string> sortedLevels(std::vector<std::string_view> levels) {
    std::ranges::sort(levels);
    auto const [first, last] = std::ranges::unique(levels);
    levels.erase(first, last);
    return {levels.begin(), levels.end()};
}

}  // namespace detail

/// @brief The levels @p query needs: `order` for the columns it sorts and filters, `display` for the columns it
///        names.
///
/// `order` is the levels of the columns `apply` builds keys for: every sort key's column, and the columns the
/// compiled filter reads. The filter is compiled here as `apply` compiles it, so a filtered column without an entry,
/// a group without an `include` entry, and a group's column on which none of its entries compiles add nothing: a
/// filter that reads no cell of a level does not need it. A filter that does not compile adds nothing either, since
/// `apply` refuses the query. `display` is the levels of `query.columns`, or of every column @p mayRead allows when
/// `columns` is absent: a caller that does not say what it shows is taken to show everything. Both are sorted,
/// without duplicates, and never hold the base level; a level may be in both. No row is read. Called on the query
/// `validate` returned; a name that is no column is skipped.
/// @param query    The validated query.
/// @param columns  The table's columns.
/// @param mayRead  Whether the caller may read a column; empty allows every column.
/// @param services The services `apply` compiles the filter with: whether an entry applies to a date column depends
///                 on the date parser. Null members mean their defaults.
/// @return The levels.
[[nodiscard]] inline LevelsNeeded levelsNeeded(TableQuery const& query, std::span<ColumnInfo const> columns,
                                               std::function<bool(std::string_view)> const& mayRead = {},
                                               Services const& services = {}) {
    // The columns that have a level, sorted by id: each column the query names is then one lookup.
    std::vector<std::pair<std::string_view, std::string_view>> levelled;
    for (auto const& column : columns) {
        if (!column.level.empty()) {
            levelled.emplace_back(column.id, column.level);
        }
    }
    std::ranges::sort(levelled);
    auto const levelOf = [&levelled](std::string_view id) {
        auto const found = std::ranges::lower_bound(levelled, id, {}, [](auto const& entry) { return entry.first; });
        return found != levelled.end() && found->first == id ? found->second : std::string_view{};
    };
    std::vector<std::string_view> order;
    std::vector<std::string_view> display;
    auto const add = [&levelOf](std::vector<std::string_view>& out, std::string_view id) {
        if (auto const level = levelOf(id); !level.empty()) {
            out.push_back(level);
        }
    };
    for (auto const& key : query.sort) {
        add(order, key.column);
    }
    if (!query.filters.empty()) {
        if (auto const filter = compileFilterGuarded(query.filters, columns, services)) {
            for (auto const column : filter->columnsUsed()) {
                if (!columns[column].level.empty()) {
                    order.push_back(columns[column].level);
                }
            }
        }
    }
    if (query.columns) {
        for (auto const& columnId : *query.columns) {
            add(display, columnId);
        }
    } else {
        for (auto const& [columnId, level] : levelled) {
            if (!mayRead || mayRead(columnId)) {
                display.push_back(level);
            }
        }
    }
    return LevelsNeeded{.order = detail::sortedLevels(std::move(order)),
                        .display = detail::sortedLevels(std::move(display))};
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
    /// @brief `AtLeast` while rows are undecided, since the view may still grow; `Exact` otherwise.
    TotalKind totalKind = TotalKind::Exact;
    /// @brief Rows the filter can neither admit nor reject until a level loads. They are in no page.
    std::int64_t undecided = 0;
    /// @brief The order levels with a load wanted or in flight for at least one row.
    std::vector<std::string> awaiting;
    /// @brief The order levels whose load failed for at least one row, and is not being tried again yet.
    std::vector<std::string> failed;
    /// @brief A keyed query only: the view row of each of `rows`.
    std::vector<std::int64_t> indices;
};

namespace detail {

/// @brief A query's view over one snapshot: what `pageRows` pages.
struct ComputedView {
    /// @brief The sorted, filtered view, as source rows.
    std::vector<std::uint32_t> rows;
    /// @brief The rows the filter could neither admit nor reject, source rows ascending. They are in no view.
    std::vector<std::uint32_t> undecided;
};

/// @brief Computes the view of a validated query over @p source in the calling thread.
/// @param source    The rows; read through one snapshot.
/// @param validated The query as `validate` returned it.
/// @param options   The services; `validate` applied its limits and `mayRead`.
/// @return The view, or the typed error: `ServiceFailed` from a throwing service, `SourceContract` when a sorted
///         or filtered base-level column holds an unloaded cell.
[[nodiscard]] inline std::expected<ComputedView, TableError> computeView(DataSource const& source,
                                                                         TableQuery const& validated,
                                                                         ApplyOptions const& options) {
    auto const columns = source.columns();
    // One copy of the services for this call, the filter's operands included:
    // a collator that is not safe to share clones itself here, so concurrent
    // calls never fold on the one the caller passed.
    auto services = options.services.forTask();
    auto filter = compileFilterGuarded(validated.filters, columns, services);
    if (!filter) {
        return std::unexpected(std::move(filter.error()));
    }
    JobInput input;
    input.snapshot = source.snapshot();
    input.columns.assign(columns.begin(), columns.end());
    input.services = std::move(services);
    input.keys.resize(columns.size());
    for (auto const& key : validated.sort) {
        if (auto const column = findColumn(columns, key.column)) {
            input.needed.push_back(*column);
        }
    }
    for (auto const column : filter->columnsUsed()) {
        input.needed.push_back(column);
    }
    std::ranges::sort(input.needed);
    auto const [first, last] = std::ranges::unique(input.needed);
    input.needed.erase(first, last);
    input.sort = validated.sort;
    input.filter = std::make_shared<CompiledFilter const>(std::move(*filter));
    input.previousSnapshot = input.snapshot;
    ViewJob job{std::move(input)};
    static_cast<void>(job.step(kNoDeadline, ::core::async::StopToken{}));
    if (job.failure()) {
        return std::unexpected(*job.failure());
    }
    auto result = job.takeResult();
    return ComputedView{.rows = std::move(result.view), .undecided = std::move(result.undecided)};
}

/// @brief The source row of a key, or nothing when the snapshot has no row with that key.
using SourceRowOf = std::function<std::optional<std::uint32_t>(RowId const&)>;

/// @brief What a view-row table holds for a source row that is not in the view.
inline constexpr std::uint32_t kNotInView = std::numeric_limits<std::uint32_t>::max();

/// @brief Fills a keyed page's `rows` and `indices`: the rows of @p keys in the view, ascending by view row.
/// @param view        The view.
/// @param snapshot    The snapshot the view was computed over.
/// @param keys        The keys asked for.
/// @param viewRowOf   View row by source row, `kNotInView` for a row not in the view; built from @p view when
///                    empty.
/// @param sourceRowOf A key's source row; when empty, the snapshot's row ids are scanned once.
/// @param out         The page to fill.
inline void keyedRows(ComputedView const& view, RowSnapshot const& snapshot, std::span<RowId const> keys,
                      std::span<std::uint32_t const> viewRowOf, SourceRowOf const& sourceRowOf, PageRows& out) {
    if (view.rows.empty() || keys.empty()) {
        return;
    }
    std::vector<std::uint32_t> built;
    if (viewRowOf.empty()) {
        built.assign(snapshot.rowCount(), kNotInView);
        for (std::size_t at = 0; at < view.rows.size(); ++at) {
            built[view.rows[at]] = static_cast<std::uint32_t>(at);
        }
        viewRowOf = built;
    }
    // (view row, source row) of each key found in the view.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;
    found.reserve(keys.size());
    auto const take = [&found, viewRowOf](std::size_t row) {
        if (row < viewRowOf.size() && viewRowOf[row] != kNotInView) {
            found.emplace_back(viewRowOf[row], static_cast<std::uint32_t>(row));
        }
    };
    if (sourceRowOf) {
        for (auto const& key : keys) {
            if (auto const row = sourceRowOf(key)) {
                take(*row);
            }
        }
    } else {
        std::unordered_set<RowId> const wanted(keys.begin(), keys.end());
        std::size_t matched = 0;
        for (std::size_t row = 0; row < snapshot.rowCount() && matched < wanted.size(); ++row) {
            if (wanted.contains(snapshot.rowId(row))) {
                ++matched;
                take(row);
            }
        }
    }
    // A key asked for twice is one row of the reply.
    std::ranges::sort(found);
    auto const [first, last] = std::ranges::unique(found);
    found.erase(first, last);
    out.rows.reserve(found.size());
    out.indices.reserve(found.size());
    for (auto const& [viewRow, sourceRow] : found) {
        out.rows.push_back(sourceRow);
        out.indices.push_back(viewRow);
    }
}

/// @brief The page of a view: its rows, total and undecided count.
///
/// A query without `keys` gets the view rows its `page` names. A keyed query gets the source rows whose key is in
/// `keys` and whose row is in the view, ascending by view row, each one's view row in `indices`, and offset 0.
/// Either way `total`, `totalKind` and `undecided` are the whole view's.
/// @param view        The view, as `computeView` returns it.
/// @param snapshot    The snapshot the view was computed over.
/// @param validated   The query as `validate` returned it.
/// @param limits      The bounds; a total above `totalCap` is left out.
/// @param viewRowOf   View row by source row, `kNotInView` for a row not in the view; built on demand when empty.
///                    Read by a keyed query only.
/// @param sourceRowOf A key's source row, one call a key; when empty, a keyed query scans the snapshot's row ids
///                    once.
/// @return The page.
[[nodiscard]] inline PageRows pageRows(ComputedView const& view, RowSnapshot const& snapshot,
                                       TableQuery const& validated, QueryLimits const& limits,
                                       std::span<std::uint32_t const> viewRowOf = {},
                                       SourceRowOf const& sourceRowOf = {}) {
    PageRows out;
    out.undecided = static_cast<std::int64_t>(view.undecided.size());
    out.totalKind = out.undecided > 0 ? TotalKind::AtLeast : TotalKind::Exact;
    auto const count = static_cast<std::int64_t>(view.rows.size());
    if (count <= limits.totalCap) {
        out.total = count;
    }
    if (validated.keys) {
        keyedRows(view, snapshot, *validated.keys, viewRowOf, sourceRowOf, out);
        return out;
    }
    out.offset = validated.page.offset;
    auto const begin = std::min(validated.page.offset, count);
    // Compared, not added: `maxLimit` may be as large as INT64_MAX.
    auto const end = validated.page.limit >= count - begin ? count : begin + validated.page.limit;
    for (auto i = begin; i < end; ++i) {
        out.rows.push_back(view.rows[static_cast<std::size_t>(i)]);
    }
    return out;
}

}  // namespace detail

/// @brief Runs @p query over @p source in the calling thread: the reference
///        implementation of `server` mode.
///
/// `validate`, then `detail::computeView`, then `detail::pageRows`; a keyed
/// query finds its keys by scanning the snapshot's row ids once.
/// @param source  The server's rows.
/// @param query   The request body's table query.
/// @param options Limits, services and column access.
/// @return The page, or the typed error the client shows: `SourceContract`
///         when a sorted or filtered base-level column holds an unloaded cell.
[[nodiscard]] inline std::expected<PageRows, TableError> apply(DataSource const& source, TableQuery const& query,
                                                               ApplyOptions const& options = {}) {
    auto const checked = validate(query, source.columns(), options.limits, options.mayRead);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    auto const view = detail::computeView(source, *checked, options);
    if (!view) {
        return std::unexpected(view.error());
    }
    // A source is read on its owner's thread, which this call is on, so this is the snapshot the view was
    // computed over.
    return detail::pageRows(*view, *source.snapshot(), *checked, options.limits);
}

// ── Typed rows ──────────────────────────────────────────────────────────────

/// @brief One entry of a row type's `columnLevels()`: the data level a column's cells need.
///
/// A row type declares its levels once, as
/// `static constexpr std::array<table::ColumnLevel, N> columnLevels() noexcept`.
struct ColumnLevel {
    /// @brief The column, by the member's JSON name.
    std::string_view column;
    /// @brief The level; never empty, which is the base level every column has without an entry.
    std::string_view level;
};

/// @brief Whether @p level is loaded for @p row: how a typed table says which of its cells are unloaded. Empty
///        means every level is loaded.
/// @tparam Row The row type.
template <typename Row>
using LevelLoaded = std::function<bool(Row const& row, std::string_view level)>;

namespace detail {

/// @brief A row type that declares the levels of its columns.
/// @tparam Row The row type.
template <typename Row>
concept HasColumnLevels = requires {
    { Row::columnLevels() };
};

/// @brief `true` for `std::optional<T>`.
/// @tparam T A type.
template <typename T>
struct IsOptional : std::false_type {};
/// @brief `true` for `std::optional<T>`.
/// @tparam T The optional's value type.
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

/// @brief A type glaze writes as one of its own members, as morph's `Ranged`,
///        `Choice`, `Multiline` and `Tagged` are: its `glz::meta` value is a
///        member pointer.
template <typename V>
concept MemberWrapper = requires { glz::meta<V>::value; } &&
                        std::is_member_object_pointer_v<std::remove_cvref_t<decltype(glz::meta<V>::value)>>;

/// @brief The member a `MemberWrapper` is written as.
/// @tparam V The wrapper type.
template <typename V>
using WrappedMember = std::remove_cvref_t<decltype(std::declval<V const&>().*(glz::meta<V>::value))>;

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
    } else if constexpr (MemberWrapper<V>) {
        return kindOf<WrappedMember<V>>();
    } else {
        return ColumnKind::Text;
    }
}

/// @brief An integer member as a cell.
/// @tparam T The integer type.
/// @param value The member.
/// @return The cell: an `int64`, or text that an integer column reads as
///         invalid when no `int64` holds the value (rather than a wrapped
///         negative number).
template <typename T>
[[nodiscard]] Cell integerCell(T value) {
    if constexpr (std::is_unsigned_v<T> && sizeof(T) >= sizeof(std::int64_t)) {
        if (value > static_cast<T>(std::numeric_limits<std::int64_t>::max())) {
            return Cell{std::to_string(value)};
        }
    }
    return Cell{static_cast<std::int64_t>(value)};
}

/// @brief A member as the text glaze writes for it: an enum by its glaze name,
///        a JSON string decoded, `null` as an empty cell.
/// @tparam T The member type.
/// @param value The member.
/// @return The cell.
template <typename T>
[[nodiscard]] Cell writtenCell(T const& value) {
    std::string text;
    if (glz::write_json(value, text) || text == "null") {
        return Cell{};
    }
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        std::string decoded;
        if (!glz::read_json(decoded, text)) {
            return Cell{std::move(decoded)};
        }
    }
    return Cell{std::move(text)};
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
        return integerCell(value);
    } else if constexpr (std::floating_point<V>) {
        return Cell{static_cast<double>(value)};
    } else if constexpr (units::isQuantity<V>) {
        return value.value() ? Cell{*value.value()} : Cell{};
    } else if constexpr (std::is_same_v<V, time::DateTime>) {
        return Cell{std::chrono::floor<std::chrono::seconds>(value.value).time_since_epoch().count()};
    } else if constexpr (std::is_same_v<V, time::Timestamp>) {
        return value.value ? toCell(*value.value) : Cell{};
    } else if constexpr (MemberWrapper<V>) {
        return toCell(value.*(glz::meta<V>::value));
    } else {
        return writtenCell(value);
    }
}

/// @brief Reads one column's cell from a row.
/// @tparam Row The row type.
template <typename Row>
using CellReader = Cell (*)(Row const&);

/// @brief One cell reader per key glaze writes, in key order.
/// @tparam Row The row type.
/// @return The readers.
template <typename Row>
[[nodiscard]] std::vector<CellReader<Row>> cellReaders() {
    constexpr auto count = glz::reflect<Row>::size;
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::vector<CellReader<Row>>{
            +[](Row const& row) -> Cell { return toCell(morph::detail::reflectedMember<I>(row)); }...};
    }(std::make_index_sequence<count>{});
}

/// @brief One column per key glaze writes, its kind from the value's type and its level from `Row::columnLevels()`
///        when the row type declares it.
/// @tparam Row The row type.
/// @return The columns.
/// @throws std::invalid_argument when a `columnLevels()` entry names no column.
template <typename Row>
[[nodiscard]] std::vector<ColumnInfo> reflectedColumns() {
    constexpr auto count = glz::reflect<Row>::size;
    std::vector<ColumnInfo> out;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (out.push_back(
             ColumnInfo{.id = std::string{glz::reflect<Row>::keys[I]},
                        .kind = kindOf<decltype(morph::detail::reflectedMember<I>(std::declval<Row const&>()))>(),
                        .comparator = {},
                        .format = {},
                        .level = {}}),
         ...);
    }(std::make_index_sequence<count>{});
    if constexpr (HasColumnLevels<Row>) {
        for (auto const& entry : Row::columnLevels()) {
            auto const column = std::ranges::find(out, entry.column, &ColumnInfo::id);
            if (column == out.end()) {
                throw std::invalid_argument{"columnLevels() names no column: " + std::string{entry.column}};
            }
            column->level = std::string{entry.level};
        }
    }
    return out;
}

}  // namespace detail

/// @brief A key projection: the `RowId` of a typed row.
/// @tparam Row The row type.
template <typename Row>
using RowKey = std::function<RowId(Row const&)>;

/// @brief A snapshot over a shared, immutable vector of typed rows.
///
/// A cell of a column whose level @p loaded refuses for its row is `Unloaded`; the member is not read.
/// @tparam Row A glaze-reflectable aggregate.
template <typename Row>
class RowsSnapshot final : public RowSnapshot {
public:
    /// @brief Wraps @p rows.
    /// @param rows    The rows; shared, never modified.
    /// @param key     The key projection.
    /// @param readers One cell reader per column.
    /// @param levels  Each column's level, by column index; null when no column has one.
    /// @param loaded  Whether a level is loaded for a row; empty when every level is.
    RowsSnapshot(std::shared_ptr<std::vector<Row> const> rows, RowKey<Row> key,
                 std::shared_ptr<std::vector<detail::CellReader<Row>> const> readers,
                 std::shared_ptr<std::vector<std::string> const> levels = nullptr, LevelLoaded<Row> loaded = {})
        : _rows{std::move(rows)},
          _key{std::move(key)},
          _readers{std::move(readers)},
          _levels{std::move(levels)},
          _loaded{std::move(loaded)} {}

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
        auto const& value = (*_rows)[row];
        auto const level = levelOf(column);
        if (!level.empty() && !_loaded(value, level)) {
            return Cell{Unloaded{}};
        }
        return (*_readers)[column](value);
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
        auto const level = levelOf(column);
        while (first < last) {
            auto const end = std::min(first + kRun, last);
            run.clear();
            for (auto row = first; row < end; ++row) {
                auto const& value = (*_rows)[row];
                run.push_back(!level.empty() && !_loaded(value, level) ? Cell{Unloaded{}} : reader(value));
            }
            sink.cells(first, run);
            first = end;
        }
    }

private:
    // The level a column's cells are checked against: empty when the column has none or no predicate is set.
    [[nodiscard]] std::string_view levelOf(std::size_t column) const {
        return _loaded && _levels != nullptr ? std::string_view{(*_levels)[column]} : std::string_view{};
    }

    std::shared_ptr<std::vector<Row> const> _rows;
    RowKey<Row> _key;
    std::shared_ptr<std::vector<detail::CellReader<Row>> const> _readers;
    std::shared_ptr<std::vector<std::string> const> _levels;
    LevelLoaded<Row> _loaded;
};

/// @brief A `DataSource` over typed rows: one column per key glaze writes,
///        its kind inferred from the value's type.
///
/// `bool` is `Bool`; other integers `Integer` (an unsigned value above
/// `INT64_MAX` is an invalid cell); floating point `Number`; `Rational`
/// `Decimal`; a `Quantity` `Quantity`; `DateTime` and `Timestamp`
/// `DateTime`; a type glaze writes as one of its members (`Ranged`,
/// `Choice`, `Tagged`) that member's kind; everything else `Text` (an enum by
/// its glaze name). A `std::optional` member's empty state, or anything glaze
/// writes as `null`, is an empty cell. A row with a `glz::meta` has the
/// columns its meta names. A row type's `columnLevels()` gives its columns
/// their levels, and a cell of a column whose level the `LevelLoaded`
/// predicate refuses for its row is `Unloaded`. `setRows` replaces the rows
/// and notifies `Reset`.
/// @tparam Row A glaze-reflectable aggregate.
template <typename Row>
class RowsSource final : public DataSource {
public:
    /// @brief Builds the source.
    /// @param rows        The rows.
    /// @param key         The key projection.
    /// @param kinds       Column kinds that replace the inferred ones, by column
    ///                    id (for example `Date` for an `int64` of epoch days).
    /// @param comparators Comparator names by column id, from
    ///                    `Services::comparators`; a column named here is `Custom`.
    /// @param loaded      Whether a level is loaded for a row; empty means every level is.
    /// @throws std::invalid_argument when `Row::columnLevels()` names no column.
    RowsSource(std::shared_ptr<std::vector<Row> const> rows, RowKey<Row> key,
               std::map<std::string, ColumnKind, std::less<>> const& kinds = {},
               std::map<std::string, std::string, std::less<>> const& comparators = {}, LevelLoaded<Row> loaded = {})
        : _columns{detail::reflectedColumns<Row>()},
          _readers{std::make_shared<std::vector<detail::CellReader<Row>> const>(detail::cellReaders<Row>())},
          _key{std::move(key)},
          _loaded{std::move(loaded)} {
        auto levels = std::make_shared<std::vector<std::string>>();
        levels->reserve(_columns.size());
        for (auto& column : _columns) {
            if (auto const found = kinds.find(column.id); found != kinds.end()) {
                column.kind = found->second;
            }
            if (auto const found = comparators.find(column.id); found != comparators.end()) {
                column.kind = ColumnKind::Custom;
                column.comparator = found->second;
            }
            levels->push_back(column.level);
        }
        _levels = std::move(levels);
        _snapshot = std::make_shared<RowsSnapshot<Row>>(std::move(rows), _key, _readers, _levels, _loaded);
    }

    /// @brief The columns.
    /// @return One per reflected member.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _columns; }

    /// @brief The current rows; a pointer copy.
    /// @return The snapshot.
    [[nodiscard]] std::shared_ptr<RowSnapshot const> snapshot() const override { return _snapshot; }

    /// @brief Registers @p listener.
    /// @param listener The listener.
    void subscribe(ChangeListener& listener) override { _listeners.add(listener); }

    /// @brief Removes @p listener; safe from inside a notification.
    /// @param listener The listener.
    void unsubscribe(ChangeListener& listener) override { _listeners.remove(listener); }

    /// @brief Replaces the rows and notifies `Reset`.
    /// @param rows The new rows.
    void setRows(std::shared_ptr<std::vector<Row> const> rows) {
        _snapshot = std::make_shared<RowsSnapshot<Row>>(std::move(rows), _key, _readers, _levels, _loaded);
        _listeners.notify(RowChange{.kind = ChangeKind::Reset, .rows = {}});
    }

private:
    std::vector<ColumnInfo> _columns;
    std::shared_ptr<std::vector<detail::CellReader<Row>> const> _readers;
    RowKey<Row> _key;
    LevelLoaded<Row> _loaded;
    std::shared_ptr<std::vector<std::string> const> _levels;
    std::shared_ptr<RowsSnapshot<Row> const> _snapshot;
    detail::Listeners _listeners;
};

namespace detail {

/// @brief The levels of @p levels that @p loaded refuses for @p row, in the order of @p levels.
/// @tparam Row The row type.
/// @param row    The row.
/// @param levels Levels, sorted.
/// @param loaded The predicate; not empty.
/// @return The refused levels.
template <typename Row>
[[nodiscard]] std::vector<std::string> refusedLevels(Row const& row, std::span<std::string const> levels,
                                                     LevelLoaded<Row> const& loaded) {
    std::vector<std::string> out;
    for (auto const& level : levels) {
        if (!loaded(row, level)) {
            out.push_back(level);
        }
    }
    return out;
}

/// @brief The order levels @p loaded refuses for at least one row of the view or of the undecided set.
/// @tparam Row The row type.
/// @param rows   The rows the view was computed over.
/// @param view   The view.
/// @param order  The query's order levels, sorted.
/// @param loaded The predicate; not empty.
/// @return The levels, sorted.
template <typename Row>
[[nodiscard]] std::vector<std::string> awaitedLevels(std::vector<Row> const& rows, ComputedView const& view,
                                                     std::span<std::string const> order,
                                                     LevelLoaded<Row> const& loaded) {
    std::vector<std::string> out;
    for (auto const& level : order) {
        auto const refused = [&](std::uint32_t row) { return !loaded(rows[row], level); };
        if (std::ranges::any_of(view.rows, refused) || std::ranges::any_of(view.undecided, refused)) {
            out.push_back(level);
        }
    }
    return out;
}

}  // namespace detail

/// @brief Runs @p query over typed rows and returns the page of rows: what a
///        server's list action returns.
///
/// With @p loaded set, `unloadedRows` names each row of the page with the levels the query needs (`levelsNeeded`:
/// `order` and `display`) that @p loaded refuses for it, and `awaiting` the `order` levels it refuses for at least
/// one row of the view or of the undecided rows. A predicate cannot say that a load failed, so `failed` stays
/// empty. Without @p loaded every level is loaded, and all three stay empty.
/// @tparam Row A glaze-reflectable aggregate.
/// @param rows    The server's rows; read during the call only.
/// @param key     The key projection.
/// @param query   The request body's table query.
/// @param options     Limits, services and column access.
/// @param kinds       Column kinds that replace the inferred ones.
/// @param comparators Comparator names by column id; a column named here is `Custom`.
/// @param loaded      Whether a level is loaded for a row; empty means every level is.
/// @return The page, or the typed error the client shows.
template <typename Row>
[[nodiscard]] std::expected<Page<Row>, TableError> apply(
    std::vector<Row> const& rows, RowKey<Row> key, TableQuery const& query, ApplyOptions const& options = {},
    std::map<std::string, ColumnKind, std::less<>> const& kinds = {},
    std::map<std::string, std::string, std::less<>> const& comparators = {}, LevelLoaded<Row> loaded = {}) {
    // Borrowed for the call: an aliasing pointer that owns nothing.
    std::shared_ptr<std::vector<Row> const> const borrowed{std::shared_ptr<void>{}, &rows};
    RowsSource<Row> const source{borrowed, std::move(key), kinds, comparators, loaded};
    auto const checked = validate(query, source.columns(), options.limits, options.mayRead);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    auto const view = detail::computeView(source, *checked, options);
    if (!view) {
        return std::unexpected(view.error());
    }
    auto page = detail::pageRows(*view, *source.snapshot(), *checked, options.limits);
    Page<Row> out;
    out.total = page.total;
    out.offset = page.offset;
    out.totalKind = page.totalKind;
    out.undecided = page.undecided;
    out.awaiting = std::move(page.awaiting);
    out.failed = std::move(page.failed);
    out.indices = std::move(page.indices);
    out.rows.reserve(page.rows.size());
    for (auto const row : page.rows) {
        out.rows.push_back(rows[row]);
    }
    if (!loaded) {
        return out;
    }
    auto const needed = levelsNeeded(*checked, source.columns(), options.mayRead, options.services.forTask());
    std::vector<std::string> wanted = needed.order;
    wanted.insert(wanted.end(), needed.display.begin(), needed.display.end());
    std::ranges::sort(wanted);
    auto const [first, last] = std::ranges::unique(wanted);
    wanted.erase(first, last);
    for (std::size_t index = 0; index < page.rows.size(); ++index) {
        auto levels = detail::refusedLevels(rows[page.rows[index]], wanted, loaded);
        if (!levels.empty()) {
            out.unloadedRows.push_back(
                UnloadedRow{.index = static_cast<std::int64_t>(index), .levels = std::move(levels), .failed = {}});
        }
    }
    auto awaited = detail::awaitedLevels(rows, *view, needed.order, loaded);
    out.awaiting.insert(out.awaiting.end(), awaited.begin(), awaited.end());
    std::ranges::sort(out.awaiting);
    auto const [awaitFirst, awaitLast] = std::ranges::unique(out.awaiting);
    out.awaiting.erase(awaitFirst, awaitLast);
    return out;
}

// ── Server mode on the client ───────────────────────────────────────────────

/// @brief A request a `PageWindow` asks the client to send, tagged with the
///        query it belongs to.
struct PageFetch {
    /// @brief The rows to request.
    PageRequest request;
    /// @brief The window's query when the fetch was made (`PageWindow::query`).
    std::uint64_t query = 0;

    /// @brief Member-wise equality.
    /// @param other The fetch to compare with.
    /// @return `true` when request and query match.
    [[nodiscard]] bool operator==(PageFetch const& other) const = default;
};

/// @brief The pages a `server`-mode table holds: those around the visible
///        rows, up to a bound, plus the request state and the last error.
///
/// The client calls `visible` as the view scrolls and sends the request of
/// each fetch it returns, then hands the fetch back with its reply to
/// `accept` (or with its error to `fail`). A change of sort or filter is a
/// new query: call `reset`. A reply to a fetch made before the last `reset`
/// is ignored, so a late page of the old query never shows under the new one.
///
/// A server may return fewer rows than a fetch asked for (`validate` clamps
/// the limit to its `maxLimit`): the window then fetches the rest of the page
/// from where the reply ended. A page is complete when it is full, reaches the
/// known total, or a reply to it is empty.
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

    /// @brief Declares the visible rows; returns the fetches to send.
    ///
    /// Covers the visible rows and one page either side, skipping complete
    /// pages, pages in flight, and pages past a known total. A page held in
    /// part is fetched from where it ends. The returned pages are marked in
    /// flight.
    /// @param first First visible view row.
    /// @param count Visible rows.
    /// @return The fetches to send.
    [[nodiscard]] std::vector<PageFetch> visible(std::int64_t first, std::int64_t count) {
        _visibleFirst = std::max<std::int64_t>(first, 0);
        _visibleCount = std::max<std::int64_t>(count, 0);
        auto const firstPage = std::max<std::int64_t>((_visibleFirst / _pageSize) - 1, 0);
        auto const lastPage = ((_visibleFirst + std::max<std::int64_t>(_visibleCount, 1) - 1) / _pageSize) + 1;
        std::vector<PageFetch> out;
        for (auto page = firstPage; page <= lastPage; ++page) {
            if (exactTotal() && page * _pageSize >= *_total) {
                break;
            }
            if (_complete.contains(page) || std::ranges::find(_inFlight, page) != _inFlight.end()) {
                continue;
            }
            auto const held = _pages.find(page);
            auto const have = held == _pages.end() ? std::int64_t{0} : static_cast<std::int64_t>(held->second.size());
            _inFlight.push_back(page);
            out.push_back(
                PageFetch{.request = PageRequest{.offset = (page * _pageSize) + have, .limit = _pageSize - have},
                          .query = _query});
        }
        return out;
    }

    /// @brief Stores a reply.
    /// @param fetch The fetch the reply answers, as `visible` returned it.
    /// @param page  The page the server returned.
    /// @return `false` when the reply was ignored: its fetch belongs to an
    ///         earlier query, or is not awaited.
    bool accept(PageFetch const& fetch, Page<Row> page) {
        if (fetch.query != _query) {
            return false;
        }
        auto const index = fetch.request.offset / _pageSize;
        auto const flight = std::ranges::find(_inFlight, index);
        if (flight == _inFlight.end()) {
            return false;
        }
        _inFlight.erase(flight);
        if (page.total) {
            _total = page.total;
            _totalKind = page.totalKind;
        }
        _error.reset();
        auto& rows = _pages[index];
        bool const replyEmpty = page.rows.empty();
        if (fetch.request.offset == (index * _pageSize) + static_cast<std::int64_t>(rows.size())) {
            auto const room = static_cast<std::size_t>(_pageSize) - rows.size();
            auto const take = std::min(room, page.rows.size());
            rows.insert(rows.end(), std::make_move_iterator(page.rows.begin()),
                        std::make_move_iterator(page.rows.begin() + static_cast<std::ptrdiff_t>(take)));
        }
        auto const end = (index * _pageSize) + static_cast<std::int64_t>(rows.size());
        if (std::cmp_greater_equal(rows.size(), _pageSize) || (exactTotal() && end >= *_total) || replyEmpty) {
            _complete.insert(index);
        }
        evict();
        return true;
    }

    /// @brief Records a failed request; the error is shown until a reply arrives.
    /// @param fetch The fetch that failed, as `visible` returned it.
    /// @param error The server's error.
    /// @return `false` when the failure was ignored: its fetch belongs to an earlier query.
    bool fail(PageFetch const& fetch, TableError error) {
        if (fetch.query != _query) {
            return false;
        }
        std::erase(_inFlight, fetch.request.offset / _pageSize);
        _error = std::move(error);
        return true;
    }

    /// @brief Drops every page, request and error and starts a new query: the
    ///        sort or filter changed.
    void reset() {
        ++_query;
        _pages.clear();
        _complete.clear();
        _inFlight.clear();
        _total.reset();
        _totalKind = TotalKind::Exact;
        _error.reset();
    }

    /// @brief The current query, which `reset` advances.
    /// @return The query's number.
    [[nodiscard]] std::uint64_t query() const noexcept { return _query; }

    /// @brief The row at a view row, when it is held.
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

    /// @brief The view's total rows, when a reply of this query reported it.
    /// @return The total.
    [[nodiscard]] std::optional<std::int64_t> total() const noexcept { return _total; }

    /// @brief Whether `total()` is the count of the view or a lower bound, as the latest reply that carried a total
    ///        said.
    /// @return The kind; `Exact` while no total is known.
    [[nodiscard]] TotalKind totalKind() const noexcept { return _totalKind; }

    /// @brief The last error, until a reply clears it.
    /// @return The error, or nothing.
    [[nodiscard]] std::optional<TableError> const& error() const noexcept { return _error; }

    /// @brief Pages held, complete or in part.
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
            _complete.erase(farthest->first);
            _pages.erase(farthest);
        }
    }

    // Whether a known total bounds the view; a lower bound does not.
    [[nodiscard]] bool exactTotal() const noexcept { return _total && _totalKind == TotalKind::Exact; }

    std::int64_t _pageSize;
    std::size_t _maxPages;
    std::uint64_t _query = 0;
    std::map<std::int64_t, std::vector<Row>> _pages;
    std::set<std::int64_t> _complete;
    std::vector<std::int64_t> _inFlight;
    std::optional<std::int64_t> _total;
    TotalKind _totalKind = TotalKind::Exact;
    std::optional<TableError> _error;
    std::int64_t _visibleFirst = 0;
    std::int64_t _visibleCount = 0;
};

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

namespace morph::table::detail {

/// @brief Whether a page member is left out of its JSON: `failed` and `indices` when empty.
///
/// The typed page, the generic page and `UnloadedRow` share this rule. An optional member that is unset is left
/// out by glaze's own rule for null members. A reader takes every member as present or absent.
/// @tparam T The member's type.
/// @param value The member.
/// @param key   Its JSON name.
/// @return `true` when it is left out.
template <typename T>
[[nodiscard]] constexpr bool omittedPageMember(T const& value, std::string_view key) {
    if constexpr (requires { value.empty(); }) {
        return (key == "failed" || key == "indices") && value.empty();
    } else {
        return false;
    }
}

}  // namespace morph::table::detail

/// @brief The JSON of `morph::table::Page`: every member by reflection, an empty `failed` or `indices` left out.
/// @tparam Row The row type.
template <typename Row>
struct glz::meta<morph::table::Page<Row>> {
    /// @brief Whether a member is left out of the JSON written.
    /// @tparam T The member's type.
    /// @param member  The member.
    /// @param key     Its JSON name.
    /// @param context Unused.
    /// @return `true` for an empty `failed` or `indices`.
    template <typename T>
    static constexpr bool skip_if(T const& member, std::string_view key, glz::meta_context const& context) {
        static_cast<void>(context);
        return morph::table::detail::omittedPageMember(member, key);
    }
};

/// @brief The JSON of `morph::table::UnloadedRow`: every member by reflection, an empty `failed` left out.
template <>
struct glz::meta<morph::table::UnloadedRow> {
    /// @brief Whether a member is left out of the JSON written.
    /// @tparam T The member's type.
    /// @param member  The member.
    /// @param key     Its JSON name.
    /// @param context Unused.
    /// @return `true` for an empty `failed`.
    template <typename T>
    static constexpr bool skip_if(T const& member, std::string_view key, glz::meta_context const& context) {
        static_cast<void>(context);
        return morph::table::detail::omittedPageMember(member, key);
    }
};

/// @brief JSON names of `morph::table::TotalKind`: `exact` and `atLeast`.
template <>
struct glz::meta<morph::table::TotalKind> {
    using enum morph::table::TotalKind;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("exact", Exact, "atLeast", AtLeast);
};
