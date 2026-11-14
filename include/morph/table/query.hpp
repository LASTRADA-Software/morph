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

/// @brief What a page's `total` says about the filtered view.
enum class TotalKind : std::uint8_t {
    Exact,    ///< `total` is the count of the filtered view.
    AtLeast,  ///< `total` is a lower bound: the view has at least that many rows.
};

/// @brief One page of a list action's reply.
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
    // One copy of the services for this call, the filter's operands included:
    // a collator that is not safe to share clones itself here, so concurrent
    // calls never fold on the one the caller passed.
    auto services = options.services.forTask();
    auto filter = compileFilterGuarded(checked->filters, columns, services);
    if (!filter) {
        return std::unexpected(std::move(filter.error()));
    }
    detail::JobInput input;
    input.snapshot = source.snapshot();
    input.columns.assign(columns.begin(), columns.end());
    input.services = std::move(services);
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
    if (job.failure()) {
        return std::unexpected(TableError{
            .code = TableErrorCode::ServiceFailed, .column = {}, .message = "a service failed: " + *job.failure()});
    }
    auto const view = job.takeResult().view;

    PageRows out;
    out.offset = checked->page.offset;
    auto const count = static_cast<std::int64_t>(view.size());
    if (count <= options.limits.totalCap) {
        out.total = count;
    }
    auto const begin = std::min(checked->page.offset, count);
    // Compared, not added: `maxLimit` may be as large as INT64_MAX.
    auto const end = checked->page.limit >= count - begin ? count : begin + checked->page.limit;
    for (auto i = begin; i < end; ++i) {
        out.rows.push_back(view[static_cast<std::size_t>(i)]);
    }
    return out;
}

// ── Query plans ─────────────────────────────────────────────────────────────

/// @brief What a sort or a filter on a column is used for, as `planQuery`'s `translatable` predicate sees it.
enum class PlanUse : std::uint8_t {
    Sort,    ///< The column orders the view.
    Filter,  ///< The column is filtered.
};

/// @brief The operators a planned condition carries.
using FilterOp = CompiledFilter::Op;

/// @brief A planned operand, typed by its column's kind: `Rational` for integer, key, decimal and quantity columns
///        (quantities in the canonical unit), `double` for numbers, `int64` for dates (days), date-times (UTC
///        seconds) and booleans (0 or 1), `string` for text (folded, or raw for a case-sensitive condition), and
///        empty when the operator takes no operand.
using PlannedOperand = std::variant<std::monostate, std::int64_t, math::Rational, double, std::string>;

/// @brief One sort key of a plan.
struct PlannedSortKey {
    /// @brief The column's id.
    std::string column;
    /// @brief How the column orders.
    ColumnKind kind = ColumnKind::Text;
    /// @brief For a `Custom` column, its comparator's name; empty otherwise.
    std::string comparator;
    /// @brief The key's direction.
    SortDirection dir = SortDirection::Ascending;
};

/// @brief One condition of a plan: an operator and its parsed operands on one column.
struct PlannedCondition {
    /// @brief The column's id.
    std::string column;
    /// @brief The column's kind.
    ColumnKind kind = ColumnKind::Text;
    /// @brief The operator.
    FilterOp op = FilterOp::IsEmpty;
    /// @brief The operand, typed by `kind`.
    PlannedOperand value;
    /// @brief `Between`'s second operand, typed by `kind`; empty otherwise.
    PlannedOperand upper;
    /// @brief The operand as the request gave it (`Between`: the first).
    std::string text;
    /// @brief `Between`'s second operand as the request gave it.
    std::string upperText;
    /// @brief Whether text operators compare the text as given rather than folded.
    bool caseSensitive = false;
};

/// @brief The conditions on one column.
struct PlannedColumnFilter {
    /// @brief The column's id.
    std::string column;
    /// @brief How `include` entries combine.
    Combine combine = Combine::Any;
    /// @brief Conditions a row must meet; empty means none.
    std::vector<PlannedCondition> include;
    /// @brief Conditions that reject a row when any matches; empty for a group's columns.
    std::vector<PlannedCondition> exclude;
};

/// @brief A filter over several columns: a row matches when the group's entries match on any of its columns.
struct PlannedGroup {
    /// @brief The columns, each with the entries that apply to it; `combine` is the group's.
    std::vector<PlannedColumnFilter> columns;
};

/// @brief A `TableQuery` validated and normalised for translation into a store's own query.
struct QueryPlan {
    /// @brief The sort chain, most significant key first. A store-backed model appends its row key, ascending, to
    ///        this (or uses it alone when this is empty) so the order is total.
    std::vector<PlannedSortKey> sort;
    /// @brief The column filters, ANDed, in evaluation order.
    std::vector<PlannedColumnFilter> filters;
    /// @brief The groups, ANDed with `filters`.
    std::vector<PlannedGroup> groups;
    /// @brief The first view row wanted, within `[0, QueryLimits::maxOffset]`.
    std::int64_t offset = 0;
    /// @brief The rows wanted, clamped to `QueryLimits::maxLimit`; zero asks for the count only.
    std::int64_t limit = 0;
    /// @brief `QueryLimits::totalCap`: a count above it may be reported as a lower bound.
    std::int64_t totalCap = 0;
};

namespace detail {

/// @brief The operand a compiled entry holds in @p slot, typed by the column's kind.
/// @param kind  The column's kind.
/// @param entry The compiled entry.
/// @param slot  0, or 1 for `Between`'s second operand.
/// @return The operand.
[[nodiscard]] inline PlannedOperand plannedOperand(ColumnKind kind, CompiledFilter::Entry const& entry,
                                                   std::size_t slot) {
    switch (kind) {
        case ColumnKind::Integer:
        case ColumnKind::Key:
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
            return entry.exact.at(slot);
        case ColumnKind::Number:
            return entry.reals.at(slot);
        case ColumnKind::Date:
        case ColumnKind::DateTime:
        case ColumnKind::Bool:
            return entry.ints.at(slot);
        case ColumnKind::Text:
            return slot == 0 ? PlannedOperand{entry.text} : PlannedOperand{};
        case ColumnKind::Custom:
        default:
            return {};
    }
}

/// @brief A compiled entry as a planned condition.
/// @param info  The column.
/// @param entry The entry.
/// @return The condition.
[[nodiscard]] inline PlannedCondition plannedCondition(ColumnInfo const& info, CompiledFilter::Entry const& entry) {
    PlannedCondition out{.column = info.id,
                         .kind = info.kind,
                         .op = entry.op,
                         .value = {},
                         .upper = {},
                         .text = entry.operands[0],
                         .upperText = entry.operands[1],
                         .caseSensitive = entry.caseSensitive};
    if (entry.op != FilterOp::IsEmpty && entry.op != FilterOp::NotEmpty) {
        out.value = plannedOperand(info.kind, entry, 0);
        if (entry.op == FilterOp::Between) {
            out.upper = plannedOperand(info.kind, entry, 1);
        }
    }
    return out;
}

/// @brief A compiled column filter as a planned one.
/// @param columns The table's columns.
/// @param filter  The compiled filter.
/// @return The planned filter.
[[nodiscard]] inline PlannedColumnFilter plannedColumnFilter(std::span<ColumnInfo const> columns,
                                                             CompiledFilter::Column const& filter) {
    auto const& info = columns[filter.column];
    PlannedColumnFilter out{.column = info.id, .combine = filter.combine, .include = {}, .exclude = {}};
    for (auto const& entry : filter.include) {
        out.include.push_back(plannedCondition(info, entry));
    }
    for (auto const& entry : filter.exclude) {
        out.exclude.push_back(plannedCondition(info, entry));
    }
    return out;
}

/// @brief The first sort or filter column of @p query that @p translatable refuses.
/// @param query        The validated query.
/// @param translatable The model's predicate; empty accepts everything.
/// @return The `Unavailable` error, or nothing.
[[nodiscard]] inline std::optional<TableError> untranslatable(
    TableQuery const& query, std::function<bool(std::string_view, PlanUse)> const& translatable) {
    if (!translatable) {
        return std::nullopt;
    }
    auto const refuse = [](std::string const& column, std::string_view use) {
        return TableError{.code = TableErrorCode::Unavailable,
                          .column = column,
                          .message = "column " + column + " cannot be " + std::string{use} + " by this table"};
    };
    for (auto const& key : query.sort) {
        if (!translatable(key.column, PlanUse::Sort)) {
            return refuse(key.column, "sorted");
        }
    }
    for (auto const& [column, filter] : query.filters.columns) {
        if (!translatable(column, PlanUse::Filter) && (!filter.include.empty() || !filter.exclude.empty())) {
            return refuse(column, "filtered");
        }
    }
    for (auto const& group : query.filters.groups) {
        for (auto const& column : group.columns) {
            if (!translatable(column, PlanUse::Filter)) {
                return refuse(column, "filtered");
            }
        }
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief Validates @p query and normalises it into a plan a model translates into its own query.
///
/// In order: `validate`, the `translatable` refusal, the guarded compile step `apply` also uses, and a pure
/// projection of the compiled filter. A refusal is therefore the value `apply` returns for the same query, except
/// the `Unavailable` of `translatable`, which only a model gives. `apply` over the same rows is the oracle the
/// model's pages are held to (`conformanceFailures`).
/// @param query        The request body's table query.
/// @param columns      The row schema's columns.
/// @param limits       The bounds of `validate`.
/// @param mayRead      Whether the principal may read a column; empty allows every column.
/// @param services     The collator and date parser operands are compiled with; null members mean their defaults.
/// @param translatable Whether the model can sort or filter a column; empty accepts all. A column it refuses is
///                     `Unavailable`, naming the column.
/// @return The plan, or the typed error the client shows.
[[nodiscard]] inline std::expected<QueryPlan, TableError> planQuery(
    TableQuery const& query, std::span<ColumnInfo const> columns, QueryLimits const& limits = {},
    std::function<bool(std::string_view)> const& mayRead = {}, Services const& services = {},
    std::function<bool(std::string_view, PlanUse)> const& translatable = {}) {
    auto checked = validate(query, columns, limits, mayRead);
    if (!checked) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto error = detail::untranslatable(*checked, translatable)) {
        return std::unexpected(std::move(*error));
    }
    auto compiled = compileFilterGuarded(checked->filters, columns, services.forTask());
    if (!compiled) {
        return std::unexpected(std::move(compiled.error()));
    }
    QueryPlan plan;
    plan.offset = checked->page.offset;
    plan.limit = checked->page.limit;
    plan.totalCap = limits.totalCap;
    for (auto const& key : checked->sort) {
        auto const& info = columns[*detail::findColumn(columns, key.column)];
        plan.sort.push_back(
            PlannedSortKey{.column = info.id, .kind = info.kind, .comparator = info.comparator, .dir = key.dir});
    }
    for (auto const& filter : compiled->columns()) {
        plan.filters.push_back(detail::plannedColumnFilter(columns, filter));
    }
    for (auto const& group : compiled->groups()) {
        PlannedGroup planned;
        for (auto const& filter : group.columns) {
            planned.columns.push_back(detail::plannedColumnFilter(columns, filter));
        }
        plan.groups.push_back(std::move(planned));
    }
    return plan;
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

/// @brief One column per key glaze writes, its kind from the value's type.
/// @tparam Row The row type.
/// @return The columns.
template <typename Row>
[[nodiscard]] std::vector<ColumnInfo> reflectedColumns() {
    constexpr auto count = glz::reflect<Row>::size;
    std::vector<ColumnInfo> out;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (out.push_back(
             ColumnInfo{.id = std::string{glz::reflect<Row>::keys[I]},
                        .kind = kindOf<decltype(morph::detail::reflectedMember<I>(std::declval<Row const&>()))>(),
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
/// columns its meta names. `setRows` replaces the rows and notifies `Reset`.
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
    RowsSource(std::shared_ptr<std::vector<Row> const> rows, RowKey<Row> key,
               std::map<std::string, ColumnKind, std::less<>> const& kinds = {},
               std::map<std::string, std::string, std::less<>> const& comparators = {})
        : _columns{detail::reflectedColumns<Row>()},
          _readers{std::make_shared<std::vector<detail::CellReader<Row>> const>(detail::cellReaders<Row>())},
          _key{std::move(key)},
          _snapshot{std::make_shared<RowsSnapshot<Row>>(std::move(rows), _key, _readers)} {
        for (auto& column : _columns) {
            if (auto const found = kinds.find(column.id); found != kinds.end()) {
                column.kind = found->second;
            }
            if (auto const found = comparators.find(column.id); found != comparators.end()) {
                column.kind = ColumnKind::Custom;
                column.comparator = found->second;
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
    void subscribe(ChangeListener& listener) override { _listeners.add(listener); }

    /// @brief Removes @p listener; safe from inside a notification.
    /// @param listener The listener.
    void unsubscribe(ChangeListener& listener) override { _listeners.remove(listener); }

    /// @brief Replaces the rows and notifies `Reset`.
    /// @param rows The new rows.
    void setRows(std::shared_ptr<std::vector<Row> const> rows) {
        _snapshot = std::make_shared<RowsSnapshot<Row>>(std::move(rows), _key, _readers);
        _listeners.notify(RowChange{.kind = ChangeKind::Reset, .rows = {}});
    }

private:
    std::vector<ColumnInfo> _columns;
    std::shared_ptr<std::vector<detail::CellReader<Row>> const> _readers;
    RowKey<Row> _key;
    std::shared_ptr<RowsSnapshot<Row> const> _snapshot;
    detail::Listeners _listeners;
};

/// @brief Runs @p query over typed rows and returns the page of rows: what a
///        server's list action returns.
/// @tparam Row A glaze-reflectable aggregate.
/// @param rows    The server's rows; read during the call only.
/// @param key     The key projection.
/// @param query   The request body's table query.
/// @param options     Limits, services and column access.
/// @param kinds       Column kinds that replace the inferred ones.
/// @param comparators Comparator names by column id; a column named here is `Custom`.
/// @return The page, or the typed error the client shows.
template <typename Row>
[[nodiscard]] std::expected<Page<Row>, TableError> apply(
    std::vector<Row> const& rows, RowKey<Row> key, TableQuery const& query, ApplyOptions const& options = {},
    std::map<std::string, ColumnKind, std::less<>> const& kinds = {},
    std::map<std::string, std::string, std::less<>> const& comparators = {}) {
    // Borrowed for the call: an aliasing pointer that owns nothing.
    std::shared_ptr<std::vector<Row> const> const borrowed{std::shared_ptr<void>{}, &rows};
    RowsSource<Row> const source{borrowed, std::move(key), kinds, comparators};
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

/// @brief JSON names of `morph::table::TotalKind`: `exact` and `atLeast`.
template <>
struct glz::meta<morph::table::TotalKind> {
    using enum morph::table::TotalKind;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("exact", Exact, "atLeast", AtLeast);
};
