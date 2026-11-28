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
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/payload_schema.hpp>
#include <morph/detail/reflected_member.hpp>
#include <morph/export.hpp>
#include <morph/table/engine.hpp>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
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

/// @brief What a distinct-values request on a date or date-time column counts by.
enum class DateBucket : std::uint8_t {
    Value,  ///< Each value: a date, or a date-time to the second.
    Year,   ///< The year a value is in.
    Month,  ///< The month a value is in.
    Day,    ///< The day a value is in.
};

/// @brief A request for the distinct values of a column: what a header's filter menu lists.
struct DistinctRequest {
    /// @brief The column whose values are listed.
    std::string column;
    /// @brief In place of `column`: a declared group's id, for values pooled over its columns.
    std::string group;
    /// @brief Narrows to values whose text contains this, both folded; empty for all.
    std::string search;
    /// @brief For `Date` and `DateTime`: `Value`, or the bucket to count by.
    DateBucket bucket = DateBucket::Value;
    /// @brief The first value of the reply, in the list's order.
    std::int64_t offset = 0;
    /// @brief Values wanted; clamped to `QueryLimits::maxLimit`.
    std::int64_t limit = 200;

    /// @brief Member-wise equality.
    /// @param other The request to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(DistinctRequest const& other) const = default;
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
    /// @brief When set, the reply holds this request's distinct values and no rows; `page`, `sort` and `columns`
    ///        do not apply. A query may not set both `keys` and `distinct`.
    std::optional<DistinctRequest> distinct;
    /// @brief Rows the view keeps although the filter rejects them: the rows the session's own actions changed. Each
    ///        one the caller may see is in the view at the place the sort gives it, and the reply's `unmatched` lists
    ///        those the filter rejects. Distinct values ignore it. Left out of the JSON when empty.
    std::vector<RowId> retain;

    /// @brief The schema keyword that marks a member of this type.
    /// @return `"x-table"`.
    [[nodiscard]] static constexpr std::string_view schemaMarker() noexcept { return "x-table"; }
};

/// @brief What a page's `total` says about the filtered view.
enum class TotalKind : std::uint8_t {
    Exact,    ///< `total` is the count of the filtered view.
    AtLeast,  ///< `total` is a lower bound: the view has at least that many rows.
};

/// @brief One distinct value of a column and the rows that hold it.
struct DistinctValue {
    /// @brief The value as the cell shows it: its first row's text.
    std::string text;
    /// @brief The value as a filter entry takes it: a `oneOf` member, or for a date bucket the two ends of a
    ///        `between` joined by `..`.
    std::string operand;
    /// @brief The rows that hold the value.
    std::int64_t count = 0;
    /// @brief A `Measure` column's value only: its dimension's name, by which a menu draws a heading.
    std::string dimension;
    /// @brief A `Measure` column's value only: the id of the unit its first row has it in.
    std::string unit;

    /// @brief Member-wise equality.
    /// @param other The value to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(DistinctValue const& other) const = default;
};

/// @brief A page of a column's distinct values, with what the rows held besides values.
struct DistinctValues {
    /// @brief The values, in the column's ascending order.
    std::vector<DistinctValue> values;
    /// @brief Values matching the search, before offset and limit; counted up to `QueryLimits::totalCap`.
    std::int64_t distinct = 0;
    /// @brief `AtLeast` when `distinct` stopped at the cap.
    TotalKind distinctKind = TotalKind::Exact;
    /// @brief Rows whose cell is empty.
    std::int64_t blanks = 0;
    /// @brief Rows whose cell is invalid; they are under no value.
    std::int64_t invalid = 0;
    /// @brief Rows were left out because a cell was unloaded or another filter could not decide them.
    bool partial = false;

    /// @brief Member-wise equality.
    /// @param other The values to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(DistinctValues const& other) const = default;
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
    /// @brief A distinct-values reply only: the values; the page then has no rows.
    std::optional<DistinctValues> distinct;
    /// @brief The indices, into `rows`, of the retained rows (`TableQuery::retain`) the filter rejects, ascending.
    ///        Left out of the JSON when empty.
    std::vector<std::int64_t> unmatched;
    /// @brief The view's membership (`MembershipDigest::text`): two pages with the same one are of views of the same
    ///        rows, whatever their sort. Empty on a distinct-values reply, and then left out of the JSON.
    std::string viewRevision;

    /// @brief The members that open a query cache entry holding the page: while either is set, a row the page
    ///        does not hold may come to belong to it, so every change of its record marks the entry. A member the
    ///        page does not have is skipped.
    /// @return `awaiting` and `distinct`.
    [[nodiscard]] static constexpr std::array<std::string_view, 2> openMembers() noexcept {
        return {"awaiting", "distinct"};
    }

    /// @brief The member that makes a query cache entry holding the page partial: while it is set, a merge by key
    ///        would leave it describing rows it no longer indexes, so an `updates` effect marks the entry instead.
    /// @return `unloadedRows`.
    [[nodiscard]] static constexpr std::string_view partialMember() noexcept { return "unloadedRows"; }
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
    /// @brief Members of one `oneOf` entry.
    std::size_t maxSetValues = 10'000;
    /// @brief Members of every `oneOf` entry of one query together.
    std::size_t maxQuerySetValues = 20'000;
    /// @brief Bytes of the members of every `oneOf` entry of one query together.
    std::size_t maxQuerySetBytes = std::size_t{1} << 20U;
    /// @brief Rows of a view a selection of the view may resolve to; a larger view is `LimitExceeded`.
    std::int64_t maxSelectedRows = 1'000'000;
    /// @brief Entries of `TableQuery::retain`, counted as received, duplicates included.
    std::size_t maxRetained = 10'000;
};

/// @brief A digest of a set of row keys: what names a view's membership.
///
/// It does not depend on the order the keys are added in, so a view's revision is the same under every sort, and
/// a retained row merged into a reply is one more `add`. Each key is hashed (FNV-1a over `"i:"` and the decimal
/// of an integer key, `"s:"` and the text of a text key) and mixed (splitmix64); the digest keeps the count, the
/// sum of the hashes and the sum of the mixes, each modulo 2^64. A caller adds each key once.
class MORPH_API MembershipDigest {
public:
    /// @brief Adds one key to the set.
    /// @param key The key; added once.
    void add(RowId const& key) noexcept;

    /// @brief The digest as text.
    /// @return The count, the sum and the mix, each as 16 lowercase hex digits: 48 digits.
    [[nodiscard]] std::string text() const {
        return model::detail::hex64(_count) + model::detail::hex64(_sum) + model::detail::hex64(_mix);
    }

private:
    std::uint64_t _count = 0;
    std::uint64_t _sum = 0;
    std::uint64_t _mix = 0;
};

namespace detail {

/// @brief A `LimitExceeded` error.
/// @param column  The column concerned, or empty.
/// @param message The description.
/// @return The error.
[[nodiscard]] MORPH_API TableError limitError(std::string column, std::string message);

/// @brief Whether any operand of an entry is longer than @p max bytes.
/// @param entry The entry.
/// @param max   The bound.
/// @return `true` when one is.
[[nodiscard]] MORPH_API bool entryTooLong(FilterEntry const& entry, std::size_t max);

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
[[nodiscard]] MORPH_API std::optional<TableError> checkColumn(std::string const& columnId,
                                                              std::span<ColumnInfo const> columns,
                                                              std::function<bool(std::string_view)> const& mayRead);

/// @brief The ids of @p asked in the order of @p columns, each once.
///
/// An id @p columns lacks is dropped; `validate` refuses such an id before it
/// calls this.
/// @param columns The table's columns, in their declared order.
/// @param asked   Column ids in any order, duplicates allowed.
/// @return The ids of @p columns that @p asked names, in the order of @p columns.
[[nodiscard]] MORPH_API std::vector<std::string> orderedColumns(std::span<ColumnInfo const> columns,
                                                                std::vector<std::string> const& asked);

/// @brief The members of a query's `oneOf` entries, counted against the per-entry and per-query bounds.
struct MORPH_API SetTally {
    /// @brief Members of every set so far.
    std::size_t values = 0;
    /// @brief Bytes of those members.
    std::size_t bytes = 0;

    /// @brief Counts the sets of @p entries.
    /// @param entries The entries of one column or group.
    /// @param column  The column, for the error; empty for a group.
    /// @param limits  The bounds.
    /// @return `LimitExceeded` naming @p column when one set is over `maxSetValues`, or nothing.
    [[nodiscard]] std::optional<TableError> add(std::span<FilterEntry const> entries, std::string const& column,
                                                QueryLimits const& limits);
};

/// @brief The columns of a declared group.
/// @param groups  The table's declared groups.
/// @param groupId A group's id.
/// @return Its columns, or nothing when no group has that id.
[[nodiscard]] MORPH_API std::optional<std::span<std::string const>> groupColumns(
    std::span<ColumnGroupInfo const> groups, std::string_view groupId);

/// @brief An `UnknownColumn` error for a group id no declared group has.
/// @param groupId The id.
/// @return The error, naming the id.
[[nodiscard]] MORPH_API TableError unknownGroup(std::string const& groupId);

/// @brief Checks a filter's columns, entry counts and value lengths.
/// @param filters The filter.
/// @param columns The row schema's columns.
/// @param limits  The bounds.
/// @param mayRead Whether the principal may read a column.
/// @param groups  The table's declared groups, which a group filter's `group` names.
/// @return The first violation, or nothing.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a run of independent rules, one `if` each, in the order they are reported; splitting scatters that order.
[[nodiscard]] MORPH_API std::optional<TableError> checkFilters(FilterSpec const& filters,
                                                               std::span<ColumnInfo const> columns,
                                                               QueryLimits const& limits,
                                                               std::function<bool(std::string_view)> const& mayRead,
                                                               std::span<ColumnGroupInfo const> groups = {});

/// @brief Checks a distinct-values request, clamping its `limit`.
///
/// Exactly one of `column` and `group` is set (`InvalidSpec`). The column, or every column of the declared group,
/// exists and the caller may read it, since a values list is a read of the column (`UnknownColumn` naming a group
/// no declaration has). `search` is at most `maxTextLength` bytes and `offset` within `0..maxOffset`, `limit` not
/// negative (`LimitExceeded`). A bucket other than `Value` needs date or date-time columns (`InvalidSpec`), and
/// `Custom` columns list no values (`UnsupportedOperator`).
/// @param request The request; its `limit` is clamped to `maxLimit`.
/// @param columns The table's columns.
/// @param limits  The bounds.
/// @param mayRead Whether the caller may read a column; empty allows every column.
/// @param groups  The table's declared groups, which `request.group` names.
/// @return The first violation, or nothing.
[[nodiscard]] MORPH_API std::optional<TableError> checkDistinct(DistinctRequest& request,
                                                                std::span<ColumnInfo const> columns,
                                                                QueryLimits const& limits,
                                                                std::function<bool(std::string_view)> const& mayRead,
                                                                std::span<ColumnGroupInfo const> groups = {});

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
/// `maxTextLength`; the retained keys by `maxRetained`, and a text one by
/// `maxTextLength`. A `distinct` request is held to `detail::checkDistinct`,
/// and a query setting both `keys` and `distinct` is `InvalidSpec`.
/// @param query   The query, as received.
/// @param columns The row schema's columns.
/// @param limits  The bounds.
/// @param mayRead Whether the principal may read a column; empty allows every column.
/// @param groups  The table's declared groups (`DataSource::groups()`), which a group filter's or a distinct
///                request's `group` names.
/// @return The query with its limits clamped and its `columns` ordered, or the first violation.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a run of independent rules, one `if` each, in the order they are reported; splitting scatters that order.
[[nodiscard]] MORPH_API std::expected<TableQuery, TableError> validate(
    TableQuery query, std::span<ColumnInfo const> columns, QueryLimits const& limits = {},
    std::function<bool(std::string_view)> const& mayRead = {}, std::span<ColumnGroupInfo const> groups = {});

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
[[nodiscard]] MORPH_API std::vector<std::string> sortedLevels(std::vector<std::string_view> levels);

}  // namespace detail

/// @brief The levels @p query needs: `order` for the columns it sorts and filters, `display` for the columns it
///        names.
///
/// `order` is the levels of the columns `apply` builds keys for: every sort key's column, and the columns the
/// compiled filter reads. The filter is compiled here as `apply` compiles it, so a filtered column without an entry,
/// a group without an `include` entry, and a group's column on which none of its entries compiles add nothing: a
/// filter that reads no cell of a level does not need it. A filter that does not compile adds nothing either, since
/// `apply` refuses the query; a group filter naming a declared group is compiled through @p groups. A `distinct`
/// request adds the level of the column, or of every column of the group, it lists, which is read for every
/// candidate row. `display` is the levels of `query.columns`, or of every column @p mayRead allows when
/// `columns` is absent: a caller that does not say what it shows is taken to show everything. Both are sorted,
/// without duplicates, and never hold the base level; a level may be in both. No row is read. Called on the query
/// `validate` returned; a name that is no column is skipped.
/// @param query    The validated query.
/// @param columns  The table's columns.
/// @param mayRead  Whether the caller may read a column; empty allows every column.
/// @param services The services `apply` compiles the filter with: whether an entry applies to a date column depends
///                 on the date parser. Null members mean their defaults.
/// @param groups   The table's declared groups.
/// @return The levels.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): a run of independent rules, one `if` each, in the order they are reported; splitting scatters that order.
[[nodiscard]] MORPH_API LevelsNeeded levelsNeeded(TableQuery const& query, std::span<ColumnInfo const> columns,
                                                  std::function<bool(std::string_view)> const& mayRead = {},
                                                  Services const& services = {},
                                                  std::span<ColumnGroupInfo const> groups = {});

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
    /// @brief A distinct-values query only: the values; `rows` is then empty.
    std::optional<DistinctValues> distinct;
    /// @brief The view's membership (`MembershipDigest::text`); empty on a distinct-values reply.
    std::string viewRevision;
    /// @brief The indices, into `rows`, of the retained rows the filter rejects, ascending.
    std::vector<std::int64_t> unmatched;
};

namespace detail {

/// @brief A query's view over one snapshot: what `pageRows` pages.
struct ComputedView {
    /// @brief The sorted, filtered view, as source rows.
    std::vector<std::uint32_t> rows;
    /// @brief The rows the filter could neither admit nor reject, source rows ascending. They are in no view.
    std::vector<std::uint32_t> undecided;
    /// @brief The keys of `rows`, as a set: the view's membership.
    MembershipDigest digest;
};

/// @brief Computes the view of a validated query over @p source in the calling thread.
/// @param source    The rows; read through one snapshot.
/// @param validated The query as `validate` returned it.
/// @param options   The services; `validate` applied its limits and `mayRead`.
/// @return The view, or the typed error: `ServiceFailed` from a throwing service, `SourceContract` when a sorted
///         or filtered base-level column holds an unloaded cell, or when a sorted or filtered column is `Measure` and
///         `options.services.units` is null.
[[nodiscard]] MORPH_API std::expected<ComputedView, TableError> computeView(DataSource const& source,
                                                                            TableQuery const& validated,
                                                                            ApplyOptions const& options);

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
MORPH_API void keyedRows(ComputedView const& view, RowSnapshot const& snapshot, std::span<RowId const> keys,
                         std::span<std::uint32_t const> viewRowOf, SourceRowOf const& sourceRowOf, PageRows& out);

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
[[nodiscard]] MORPH_API PageRows pageRows(ComputedView const& view, RowSnapshot const& snapshot,
                                          TableQuery const& validated, QueryLimits const& limits,
                                          std::span<std::uint32_t const> viewRowOf = {},
                                          SourceRowOf const& sourceRowOf = {});

/// @brief A view with retained rows merged in: what `mergeRetained` returns.
struct MergedView {
    /// @brief Source rows, in view order.
    std::vector<std::uint32_t> order;
    /// @brief View rows of retained rows the filter rejects, ascending.
    std::vector<std::int64_t> unmatched;
    /// @brief The rows the filter can neither admit nor reject, less the retained ones, which are in the view.
    std::int64_t undecided = 0;
    /// @brief The membership of `order`: the base digest with each admitted row's key added.
    MembershipDigest digest;
};

/// @brief Merges retained rows into a base view: the view the sort gives over the base rows and the retained rows
///        the base lacks, each of those in it whatever the filter says.
///
/// A retained row the filter cannot decide is in the view, not undecided; one it rejects is listed in `unmatched`.
/// Its cost is that of the retained rows: a scan of @p baseOrder and a binary search of it per admitted row.
/// @param baseOrder     The view of the sort and filter, as source rows sorted by @p before.
/// @param baseUndecided The base view's undecided rows.
/// @param baseDigest    The base view's membership.
/// @param retainedRows  The retained rows the source has, as source rows.
/// @param before        The query's comparator over the sort's keys, ties to the lower source row.
/// @param match         What the filter says of a source row.
/// @param keyOf         A source row's key.
/// @return The merged view.
[[nodiscard]] MORPH_API MergedView mergeRetained(std::span<std::uint32_t const> baseOrder, std::int64_t baseUndecided,
                                                 MembershipDigest baseDigest,
                                                 std::span<std::uint32_t const> retainedRows,
                                                 std::function<bool(std::uint32_t, std::uint32_t)> const& before,
                                                 std::function<Match(std::uint32_t)> const& match,
                                                 std::function<RowId(std::uint32_t)> const& keyOf);

/// @brief The keys of a few rows of a snapshot, built as they are first asked for: what orders and filters retained
///        rows against a cached view, whose key columns are not kept.
///
/// Each row asked for gets a local index into key columns of the needed columns alone, so the cost is that of the
/// rows read, not of the snapshot.
class MORPH_API SparseKeys {
public:
    /// @brief Keys of @p needed over @p snapshot.
    /// @param snapshot The rows; outlives this.
    /// @param columns  The snapshot's columns; outlive this.
    /// @param services The services the keys are built with; outlive this.
    /// @param needed   The columns whose keys are read: the sort's and the filter's.
    SparseKeys(RowSnapshot const& snapshot, std::span<ColumnInfo const> columns, Services const& services,
               std::vector<std::size_t> needed)
        : _snapshot{&snapshot}, _columns{columns}, _services{&services}, _needed{std::move(needed)} {
        grow(16);
    }

    SparseKeys(SparseKeys const&) = delete;
    SparseKeys& operator=(SparseKeys const&) = delete;
    SparseKeys(SparseKeys&&) = delete;
    SparseKeys& operator=(SparseKeys&&) = delete;
    ~SparseKeys() = default;

    /// @brief A row's local index, its keys built at the first call.
    /// @param row The source row.
    /// @return Its index into the key columns `pointers` gives.
    [[nodiscard]] std::uint32_t local(std::uint32_t row);

    /// @brief The key columns by column index, null for a column not needed; valid until the next `local`.
    /// @return The pointers.
    [[nodiscard]] std::span<KeyColumn const* const> pointers() const noexcept { return _pointers; }

private:
    void fill(std::uint32_t position);

    void grow(std::size_t capacity);

    RowSnapshot const* _snapshot;
    std::span<ColumnInfo const> _columns;
    Services const* _services;
    std::vector<std::size_t> _needed;
    std::size_t _capacity = 0;
    std::vector<std::unique_ptr<KeyColumn>> _keys;
    std::vector<KeyColumn const*> _pointers;
    std::vector<std::uint32_t> _rows;
    std::unordered_map<std::uint32_t, std::uint32_t> _local;
};

/// @brief A view with its retained rows merged in, and the view rows of those the filter rejects.
struct RetainedView {
    /// @brief The merged view.
    ComputedView view;
    /// @brief View rows of retained rows the filter rejects, ascending.
    std::vector<std::int64_t> unmatched;
};

/// @brief Merges a validated query's retained rows into the view of its sort and filter, for one request: the
///        view's own value, which a cache may share, is not changed.
/// @param base        The view of the sort and filter.
/// @param source      The rows the view was computed over: their columns and groups.
/// @param snapshot    The snapshot the view was computed over.
/// @param validated   The query as `validate` returned it.
/// @param options     The services; `validate` applied its limits and `mayRead`.
/// @param sourceRowOf A key's source row; when empty, the snapshot's row ids are scanned once.
/// @return The merged view, or the filter's compile error.
[[nodiscard]] MORPH_API std::expected<RetainedView, TableError> retainedView(
    ComputedView const& base, DataSource const& source, RowSnapshot const& snapshot, TableQuery const& validated,
    ApplyOptions const& options, SourceRowOf const& sourceRowOf = {});

/// @brief Sets a page's `unmatched`: the indices, into its `rows`, of those whose view row is in @p unmatched.
/// @param page      The page; a keyed page's view rows are its `indices`, a page's its offset onwards.
/// @param unmatched View rows of retained rows the filter rejects, ascending.
MORPH_API void markUnmatched(PageRows& page, std::span<std::int64_t const> unmatched);

/// @brief The page of a view for a validated query: `pageRows` over the view, with the query's retained rows merged
///        in per request when it retains any.
/// @param view        The view of the sort and filter, as `computeView` returns it or a cache holds it.
/// @param source      The rows the view was computed over.
/// @param snapshot    The snapshot the view was computed over.
/// @param validated   The query as `validate` returned it.
/// @param options     The limits and services.
/// @param viewRowOf   As for `pageRows`; read only when the query retains nothing, since a merged view has its own.
/// @param sourceRowOf As for `pageRows`.
/// @return The page, or the filter's compile error.
[[nodiscard]] MORPH_API std::expected<PageRows, TableError> viewPage(
    ComputedView const& view, DataSource const& source, RowSnapshot const& snapshot, TableQuery const& validated,
    ApplyOptions const& options, std::span<std::uint32_t const> viewRowOf = {}, SourceRowOf const& sourceRowOf = {});

/// @brief What `distinctOver` reads: one snapshot, the key columns at hand, and the filter of the other columns.
struct DistinctInput {
    /// @brief The rows.
    std::shared_ptr<RowSnapshot const> snapshot;
    /// @brief The columns of @p snapshot.
    std::span<ColumnInfo const> columns;
    /// @brief Key columns by column index, built over `snapshot`; null where not built, or empty for none.
    std::span<std::shared_ptr<KeyColumn const> const> keys;
    /// @brief The filter, the listed column's own entry already erased.
    FilterSpec filters;
    /// @brief The services for this call alone (`Services::forTask`).
    Services services;
    /// @brief The bounds; `totalCap` caps the `distinct` count.
    QueryLimits limits;
    /// @brief The table's declared groups, which the filter's and the request's `group` name.
    std::span<ColumnGroupInfo const> groups;
};

/// @brief The civil date of a count of days since 1970-01-01.
/// @param days The days.
/// @return The date.
[[nodiscard]] inline std::chrono::year_month_day civilDate(std::int64_t days) {
    return std::chrono::year_month_day{std::chrono::sys_days{std::chrono::days{days}}};
}

/// @brief Days since 1970-01-01 of a civil date.
/// @param date The date.
/// @return The days.
[[nodiscard]] inline std::int64_t daysOf(std::chrono::year_month_day const& date) {
    return std::chrono::sys_days{date}.time_since_epoch().count();
}

/// @brief A date as `YYYY-MM-DD`.
/// @param days Days since 1970-01-01.
/// @return The text.
[[nodiscard]] MORPH_API std::string isoDate(std::int64_t days);

/// @brief The day a date-time falls on, in UTC.
/// @param seconds Seconds since 1970-01-01T00:00:00Z.
/// @return Days since 1970-01-01.
[[nodiscard]] MORPH_API std::int64_t dayOfSeconds(std::int64_t seconds);

/// @brief A date-time as `YYYY-MM-DDTHH:MM:SSZ`.
/// @param seconds Seconds since 1970-01-01T00:00:00Z.
/// @return The text.
[[nodiscard]] MORPH_API std::string isoDateTime(std::int64_t seconds);

/// @brief The first and last day of the bucket a day is in.
/// @param day    Days since 1970-01-01.
/// @param bucket `Year`, `Month` or `Day`.
/// @return The bucket's first and last day.
[[nodiscard]] MORPH_API std::pair<std::int64_t, std::int64_t> bucketDays(std::int64_t day, DateBucket bucket);

/// @brief One value while `distinctOver` groups the rows.
struct DistinctGroup {
    /// @brief The value's first candidate row, in source order.
    std::uint32_t first = 0;
    /// @brief The place, among the listed columns, of the first column of `first` that holds it.
    std::size_t column = 0;
    /// @brief The rows that hold it.
    std::int64_t count = 0;
    /// @brief The last row counted, so a row holding the value in several columns counts once.
    std::uint32_t last = 0;
};

/// @brief Groups the candidate rows of the listed columns by a key: a row counts once for each value it holds in
///        any of them, once in `blanks` when one of its cells is empty, and once in `invalid` when one is invalid.
/// @tparam Key  The grouping key's type.
/// @tparam Hash Its hash.
/// @param rows    The candidate rows, source order.
/// @param columns The listed columns' keys, in the order they are listed.
/// @param keyOf   The grouping key of a valid cell, from its column's keys and its row.
/// @param groups  Receives one entry a value, in order of first appearance.
/// @param out     Receives `blanks`, `invalid` and `partial`.
template <typename Key, typename Hash = std::hash<Key>>
void groupRows(std::span<std::uint32_t const> rows, std::span<KeyColumn const* const> columns, auto const& keyOf,
               std::vector<DistinctGroup>& groups, DistinctValues& out) {
    std::unordered_map<Key, std::size_t, Hash> index;
    for (auto const row : rows) {
        bool blank = false;
        bool invalid = false;
        for (std::size_t at = 0; at < columns.size(); ++at) {
            auto const& keys = *columns[at];
            switch (keys.states[row]) {
                case CellState::Empty:
                    blank = true;
                    continue;
                case CellState::Invalid:
                    invalid = true;
                    continue;
                case CellState::Unloaded:
                    out.partial = true;
                    continue;
                case CellState::Valid:
                default:
                    break;
            }
            auto const [found, added] = index.try_emplace(keyOf(keys, row), groups.size());
            if (added) {
                groups.push_back(DistinctGroup{.first = row, .column = at, .count = 1, .last = row});
                continue;
            }
            auto& group = groups[found->second];
            if (group.last != row) {
                ++group.count;
                group.last = row;
            }
        }
        out.blanks += blank ? 1 : 0;
        out.invalid += invalid ? 1 : 0;
    }
}

/// @brief Orders two valid cells of one kind, possibly in two columns, as a sort of one column would.
/// @param left      One cell's column keys.
/// @param leftRow   Its row.
/// @param right     The other cell's column keys, of the same kind.
/// @param rightRow  Its row.
/// @return Negative, zero or positive as the left cell orders before, with, or after the right one.
[[nodiscard]] MORPH_API int compareValues(KeyColumn const& left, std::uint32_t leftRow, KeyColumn const& right,
                                          std::uint32_t rightRow);

/// @brief A cell of a `Measure` column as a measure: what its key was read from.
/// @param cell  The cell; its key is valid.
/// @param units The registry the key was built with.
/// @return The measure: a plain number in the ratio's unit, text read as an operand; nothing for any other cell.
[[nodiscard]] MORPH_API std::optional<Measure> measureOf(Cell const& cell, UnitRegistry const* units);

/// @brief The `text` and `operand` of a value: from its first row.
/// @param snapshot The rows.
/// @param column   The column's index.
/// @param info     The column.
/// @param keys     The column's keys.
/// @param row      The value's first row.
/// @param bucket   The request's bucket.
/// @param units    The registry a `Measure` column's keys were built with.
/// @return The value, its count zero. A measure's `text` is its first row's amount and its unit's symbol (the ratio's
///         unit shows none), its `operand` the amount and the unit's id, as a filter takes it.
[[nodiscard]] MORPH_API DistinctValue describeValue(RowSnapshot const& snapshot, std::size_t column,
                                                    ColumnInfo const& info, KeyColumn const& keys, std::uint32_t row,
                                                    DateBucket bucket, UnitRegistry const* units = nullptr);

/// @brief The distinct values of one column, or pooled over a group's columns, over the rows @p input's filter
///        admits: the one core that `distinctValues`, `Engine::distinctValues` and a server's reply share.
///
/// Builds the key columns @p input lacks (the filter's and the listed columns') on the calling thread, into locals.
/// A row the filter leaves undecided is in no count and sets `partial`; an unloaded cell in a listed column is in no
/// count and sets `partial`, and the row's loaded cells still count. An unloaded cell in a base-level column the
/// list reads is `SourceContract`, as a view job reports it, and so is a listed `Measure` column with no unit
/// registry. Valid cells group by the key the column sorts and filters by (text by its fold; a date bucket by its
/// first day; a measure by its dimension's place and its base amount); a row counts once per value it holds, once in
/// `blanks` when it has an empty cell and once in `invalid` when it has an invalid one. A value's text is that of
/// its first row and, within the row, its first column. Values are searched by their folded text, ordered ascending
/// by their keys (ties by first row), counted up to `totalCap`, and then sliced by `offset` and `limit`.
/// @param input     The snapshot, columns, keys, filter, services, bounds and groups.
/// @param validated The request as `detail::checkDistinct` left it.
/// @return The values; `ServiceFailed` from a throwing service; `SourceContract` for an unloaded cell in a base-level
///         column it reads; or the filter's compile error.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
[[nodiscard]] MORPH_API std::expected<DistinctValues, TableError> distinctOver(DistinctInput const& input,
                                                                               DistinctRequest const& validated);

/// @brief Removes a values list's own filter: the column's entry, or every group filter naming the group.
/// @param filters The filter.
/// @param request The values list's request.
MORPH_API void withoutOwnFilter(FilterSpec& filters, DistinctRequest const& request);

}  // namespace detail

/// @brief The distinct values of one column, or pooled over a declared group's columns, with their counts, over the
///        rows the other filters of @p query leave.
///
/// The source's groups are checked (`detail::checkGroups`), and the query and the request validated together
/// (`validate` with `distinct` set to @p request). The column's own entry in `query.filters.columns`, or every
/// group filter naming the request's group, is removed, so a value the list's own filter left out is still listed.
/// Runs in the calling thread over one snapshot, building the key columns it reads.
/// @param source  The rows.
/// @param query   The query whose filters narrow the rows; its page, sort and columns are not read.
/// @param request The column, search, bucket and page of values.
/// @param options Limits, services and column access.
/// @return The values, or the typed error the client shows.
[[nodiscard]] MORPH_API std::expected<DistinctValues, TableError> distinctValues(DataSource const& source,
                                                                                 TableQuery const& query,
                                                                                 DistinctRequest const& request,
                                                                                 ApplyOptions const& options = {});

inline std::expected<DistinctValues, TableError> Engine::distinctValues(DistinctRequest const& request) const {
    auto validated = request;
    QueryLimits const limits;
    auto const groups = _source->groups();
    if (auto error = detail::checkGroups(_columns, groups)) {
        return std::unexpected(std::move(*error));
    }
    if (auto error = detail::checkDistinct(validated, _columns, limits, {}, groups)) {
        return std::unexpected(std::move(*error));
    }
    auto filters = _filterSpec;
    detail::withoutOwnFilter(filters, validated);
    detail::DistinctInput const input{.snapshot = _snapshot,
                                      .columns = _columns,
                                      .keys = _keysSnapshot == _snapshot
                                                  ? std::span<std::shared_ptr<KeyColumn const> const>{_keys}
                                                  : std::span<std::shared_ptr<KeyColumn const> const>{},
                                      .filters = std::move(filters),
                                      .services = _options.services.forTask(),
                                      .limits = limits,
                                      .groups = groups};
    return detail::distinctOver(input, validated);
}

/// @brief Runs @p query over @p source in the calling thread: the reference
///        implementation of `server` mode.
///
/// `validate`, then `detail::computeView`, then `detail::pageRows`; a keyed
/// query finds its keys by scanning the snapshot's row ids once. A query with
/// `distinct` set is answered by `distinctValues`, with no rows.
/// @param source  The server's rows.
/// @param query   The request body's table query.
/// @param options Limits, services and column access.
/// @return The page, or the typed error the client shows: `SourceContract`
///         when a sorted or filtered base-level column holds an unloaded cell,
///         or when the source's declared groups break `detail::checkGroups`.
[[nodiscard]] MORPH_API std::expected<PageRows, TableError> apply(DataSource const& source, TableQuery const& query,
                                                                  ApplyOptions const& options = {});

// ── Keys on the wire ────────────────────────────────────────────────────────

/// @brief Writes row keys as a change event carries them.
/// @param keys The keys.
/// @return Each key as raw JSON: an integer key as a JSON number, a text key as a JSON string.
[[nodiscard]] MORPH_API std::vector<glz::raw_json> changeKeys(std::span<RowId const> keys);

/// @brief Reads one row key of a change event back.
/// @param key The key as raw JSON.
/// @return The key: a JSON number that is an integer read exactly, a JSON string read as text; nothing for any
///         other JSON, which a reader takes as a change of the whole type.
[[nodiscard]] MORPH_API std::optional<RowId> rowIdOf(glz::raw_json const& key);

// ── Typed rows ──────────────────────────────────────────────────────────────

/// @brief One entry of a row type's `columnLevels()`: the data level a column's cells need.
///
/// A row type declares its levels once, as
/// `static constexpr std::array<morph::table::ColumnLevel, N> columnLevels() noexcept`.
struct ColumnLevel {
    /// @brief The column, by the member's JSON name.
    std::string_view column;
    /// @brief The level; never empty, which is the base level every column has without an entry.
    std::string_view level;
};

/// @brief A typed row's amount in a named unit: the member type of a `Measure` column.
///
/// Its JSON is `{"num", "den", "unit"}`: the amount as a `Rational` is written, with the unit's string id beside it.
/// The id is resolved through the registry the row's source is given, and an id it does not hold is an invalid
/// cell. A row's schema marks such a property `x-measure` and lists no units, which are the application's at run
/// time.
struct MORPH_API MeasureValue {
    /// @brief The amount.
    math::Rational amount;
    /// @brief The unit's id; empty is the ratio's unit.
    std::string unit;

    /// @brief The JSON shape.
    struct Wire {
        /// @brief The amount's numerator.
        std::int64_t num = 0;
        /// @brief The amount's denominator.
        std::int64_t den = 1;
        /// @brief The unit's id.
        std::string unit;
    };

    /// @brief Reads the JSON shape; the amount's places are k when the denominator is 10^k, else the most a
    ///        `Rational` holds, as a generic page reads an exact cell.
    /// @param wire The shape as read.
    void setWire(Wire const& wire);

    /// @brief The JSON shape.
    /// @return The amount's canonical numerator and denominator, and the unit's id.
    [[nodiscard]] Wire getWire() const {
        return Wire{.num = amount.numerator, .den = amount.denominator, .unit = unit};
    }

    /// @brief Member-wise equality, the amount by value.
    /// @param other The value to compare with.
    /// @return `true` when the amounts are equal and the units are the same id.
    [[nodiscard]] bool operator==(MeasureValue const& other) const = default;
};

/// @brief Whether @p level is loaded for @p row: how a typed table says which of its cells are unloaded. Empty
///        means every level is loaded.
/// @tparam Row The row type.
template <typename Row>
using LevelLoaded = std::function<bool(Row const& row, std::string_view level)>;

namespace detail {

/// @brief A row type that declares the levels of its columns.
template <typename Row>
concept HasColumnLevels = requires {
    { Row::columnLevels() };
};

/// @brief A row type that declares column groups.
template <typename Row>
concept HasColumnGroups = requires {
    { Row::columnGroups() };
};

/// @brief The column groups a row type declares, each labelled by its id.
/// @tparam Row The row type.
/// @return The groups; none when the row type declares none.
template <typename Row>
[[nodiscard]] std::vector<ColumnGroupInfo> declaredGroups() {
    std::vector<ColumnGroupInfo> out;
    if constexpr (HasColumnGroups<Row>) {
        for (auto const& group : Row::columnGroups()) {
            out.push_back(ColumnGroupInfo{.id = std::string{group.id},
                                          .label = std::string{group.id},
                                          .columns = {group.columns.begin(), group.columns.end()}});
        }
    }
    return out;
}

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
    } else if constexpr (std::is_same_v<V, MeasureValue>) {
        return ColumnKind::Measure;
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
/// @param units The registry a `MeasureValue`'s unit id is resolved through; null resolves none.
/// @return The cell; a `MeasureValue` whose id @p units does not hold is a measure of `kUnknownUnit`, an invalid cell.
template <typename T>
[[nodiscard]] Cell toCell(T const& value, UnitRegistry const* units = nullptr) {
    using V = std::remove_cvref_t<T>;
    if constexpr (IsOptional<V>::value) {
        return value ? toCell(*value, units) : Cell{};
    } else if constexpr (std::is_same_v<V, MeasureValue>) {
        auto const unit = units != nullptr ? units->find(value.unit) : std::nullopt;
        return Cell{Measure{.amount = value.amount, .unit = unit.value_or(kUnknownUnit)}};
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
        return toCell(value.*(glz::meta<V>::value), units);
    } else {
        return writtenCell(value);
    }
}

/// @brief Reads one column's cell from a row, a measure's unit through the registry given.
/// @tparam Row The row type.
template <typename Row>
using CellReader = Cell (*)(Row const&, UnitRegistry const*);

/// @brief One cell reader per key glaze writes, in key order.
/// @tparam Row The row type.
/// @return The readers.
template <typename Row>
[[nodiscard]] std::vector<CellReader<Row>> cellReaders() {
    constexpr auto count = glz::reflect<Row>::size;
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::vector<CellReader<Row>>{+[](Row const& row, UnitRegistry const* units) -> Cell {
            return toCell(morph::detail::reflectedMember<I>(row), units);
        }...};
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
    /// @param units   The registry a measure member's unit id is resolved through; null resolves none.
    RowsSnapshot(std::shared_ptr<std::vector<Row> const> rows, RowKey<Row> key,
                 std::shared_ptr<std::vector<detail::CellReader<Row>> const> readers,
                 std::shared_ptr<std::vector<std::string> const> levels = nullptr, LevelLoaded<Row> loaded = {},
                 std::shared_ptr<UnitRegistry const> units = nullptr)
        : _rows{std::move(rows)},
          _key{std::move(key)},
          _readers{std::move(readers)},
          _levels{std::move(levels)},
          _loaded{std::move(loaded)},
          _units{std::move(units)} {}

    /// @brief The number of rows.
    /// @return Row count.
    [[nodiscard]] std::size_t rowCount() const override { return _rows->size(); }

    /// @brief The rows vector's address without a level predicate, so a source rebuilt over the same vector has
    ///        the same identity; this snapshot's address with one, whose cells the predicate also decides.
    /// @return The identity.
    [[nodiscard]] void const* identity() const override {
        return _loaded ? static_cast<void const*>(this) : static_cast<void const*>(_rows.get());
    }

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
        return (*_readers)[column](value, _units.get());
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
                run.push_back(!level.empty() && !_loaded(value, level) ? Cell{Unloaded{}}
                                                                       : reader(value, _units.get()));
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
    std::shared_ptr<UnitRegistry const> _units;
};

/// @brief A `DataSource` over typed rows: one column per key glaze writes,
///        its kind inferred from the value's type.
///
/// `bool` is `Bool`; other integers `Integer` (an unsigned value above
/// `INT64_MAX` is an invalid cell); floating point `Number`; `Rational`
/// `Decimal`; a `Quantity` `Quantity`; a `MeasureValue` `Measure`, its unit id
/// resolved through the source's registry; `DateTime` and `Timestamp`
/// `DateTime`; a type glaze writes as one of its members (`Ranged`,
/// `Choice`, `Tagged`) that member's kind; everything else `Text` (an enum by
/// its glaze name). A `std::optional` member's empty state, or anything glaze
/// writes as `null`, is an empty cell. A row with a `glz::meta` has the
/// columns its meta names. A row type's `columnLevels()` gives its columns
/// their levels, and a cell of a column whose level the `LevelLoaded`
/// predicate refuses for its row is `Unloaded`. A row type's `columnGroups()`
/// are the source's `groups()`, each labelled by its id. `setRows` replaces
/// the rows and notifies `Reset`.
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
    /// @param units       The registry a `MeasureValue` member's unit id is resolved through: the one the source's
    ///                    keys are built with (`Services::units`). Null resolves none, so every measure is invalid.
    /// @throws std::invalid_argument when `Row::columnLevels()` names no column.
    RowsSource(std::shared_ptr<std::vector<Row> const> rows, RowKey<Row> key,
               std::map<std::string, ColumnKind, std::less<>> const& kinds = {},
               std::map<std::string, std::string, std::less<>> const& comparators = {}, LevelLoaded<Row> loaded = {},
               std::shared_ptr<UnitRegistry const> units = nullptr)
        : _columns{detail::reflectedColumns<Row>()},
          _groups{detail::declaredGroups<Row>()},
          _readers{std::make_shared<std::vector<detail::CellReader<Row>> const>(detail::cellReaders<Row>())},
          _key{std::move(key)},
          _loaded{std::move(loaded)},
          _units{std::move(units)} {
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
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): _levels is built in the body before the snapshot can name it.
        _snapshot = std::make_shared<RowsSnapshot<Row>>(std::move(rows), _key, _readers, _levels, _loaded, _units);
    }

    /// @brief The columns.
    /// @return One per reflected member.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _columns; }

    /// @brief The row type's declared groups.
    /// @return One per `Row::columnGroups()` entry, labelled by its id; none when the row type declares none.
    [[nodiscard]] std::span<ColumnGroupInfo const> groups() const override { return _groups; }

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
        _snapshot = std::make_shared<RowsSnapshot<Row>>(std::move(rows), _key, _readers, _levels, _loaded, _units);
        _listeners.notify(RowChange{.kind = ChangeKind::Reset, .rows = {}});
    }

private:
    std::vector<ColumnInfo> _columns;
    std::vector<ColumnGroupInfo> _groups;
    std::shared_ptr<std::vector<detail::CellReader<Row>> const> _readers;
    RowKey<Row> _key;
    LevelLoaded<Row> _loaded;
    std::shared_ptr<UnitRegistry const> _units;
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
/// @param options     Limits, services and column access; `services.units` also resolves `MeasureValue` members.
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
    RowsSource<Row> const source{borrowed, std::move(key), kinds, comparators, loaded, options.services.units};
    if (auto error = detail::checkGroups(source.columns(), source.groups())) {
        return std::unexpected(std::move(*error));
    }
    auto const checked = validate(query, source.columns(), options.limits, options.mayRead, source.groups());
    if (!checked) {
        return std::unexpected(checked.error());
    }
    if (checked->distinct) {
        auto values = distinctValues(source, *checked, *checked->distinct, options);
        if (!values) {
            return std::unexpected(std::move(values.error()));
        }
        Page<Row> out;
        out.distinct = std::move(*values);
        return out;
    }
    auto const view = detail::computeView(source, *checked, options);
    if (!view) {
        return std::unexpected(view.error());
    }
    auto viewed = detail::viewPage(*view, source, *source.snapshot(), *checked, options);
    if (!viewed) {
        return std::unexpected(std::move(viewed.error()));
    }
    auto page = std::move(*viewed);
    Page<Row> out;
    out.total = page.total;
    out.offset = page.offset;
    out.totalKind = page.totalKind;
    out.undecided = page.undecided;
    out.awaiting = std::move(page.awaiting);
    out.failed = std::move(page.failed);
    out.indices = std::move(page.indices);
    out.viewRevision = std::move(page.viewRevision);
    out.unmatched = std::move(page.unmatched);
    out.rows.reserve(page.rows.size());
    for (auto const row : page.rows) {
        out.rows.push_back(rows[row]);
    }
    if (!loaded) {
        return out;
    }
    auto const needed =
        levelsNeeded(*checked, source.columns(), options.mayRead, options.services.forTask(), source.groups());
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

/// @brief A page request a `PageWindow` asks the client to send, tagged with the
///        query and the revision it belongs to.
struct PageFetch {
    /// @brief The rows to request.
    PageRequest request;
    /// @brief The columns to ask for (`TableQuery::columns`). A held page asks only for the columns it lacks; empty
    ///        asks for every column.
    std::vector<std::string> columns;
    /// @brief The window's query when the fetch was made (`PageWindow::query`).
    std::uint64_t query = 0;
    /// @brief The window's revision when the fetch was made (`PageWindow::revision`).
    std::uint64_t revision = 0;

    /// @brief Member-wise equality.
    /// @param other The fetch to compare with.
    /// @return `true` when request, columns, query and revision match.
    [[nodiscard]] bool operator==(PageFetch const& other) const = default;
};

/// @brief A keyed request a `PageWindow` asks the client to send (`TableQuery::keys`).
struct RowsFetch {
    /// @brief The keys to ask for: the event's keys and one probe per run of held rows.
    std::vector<RowId> keys;
    /// @brief The columns to ask for: those of the latest `visible` call.
    std::vector<std::string> columns;
    /// @brief The window's query when the fetch was made.
    std::uint64_t query = 0;
    /// @brief The fetch's place among the window's keyed fetches; a later fetch has a larger one.
    std::uint64_t sequence = 0;

    /// @brief Member-wise equality.
    /// @param other The fetch to compare with.
    /// @return `true` when keys, columns, query and sequence match.
    [[nodiscard]] bool operator==(RowsFetch const& other) const = default;
};

/// @brief What a window asks for after a change of its table's record.
enum class Refresh : std::uint8_t {
    Nothing,  ///< Nothing: the window holds no row.
    Rows,     ///< The keyed fetch in `RefreshPlan::rows`.
    Pages,    ///< The window invalidated itself; the next `visible` fetches its pages again.
};

/// @brief What `PageWindow::refresh` and `PageWindow::owed` return.
struct RefreshPlan {
    /// @brief What to fetch.
    Refresh kind = Refresh::Nothing;
    /// @brief The keyed fetch to send when `kind` is `Rows`.
    RowsFetch rows;
};

template <typename Row>
class PageWindow;

/// @brief The window of generic rows; defined in `table/description.hpp`, so a translation unit that does not
///        include it cannot use the typed window for generic rows by mistake.
template <>
class PageWindow<std::vector<Cell>>;

namespace detail {

/// @brief How a fetch in flight relates to the page it is for.
enum class WindowFetchKind : std::uint8_t {
    Whole,    ///< The whole page: its reply replaces the page.
    Rest,     ///< The rows after those held: its reply is appended.
    Columns,  ///< The same rows with more columns: its reply's cells are added by key.
};

/// @brief What a window reads of a reply besides its rows' content.
struct WindowReply {
    /// @brief One key per row; empty when the window has no key function.
    std::span<RowId const> keys;
    /// @brief Rows in the reply.
    std::size_t rows = 0;
    /// @brief The reply's `unloadedRows`.
    std::span<UnloadedRow const> unloadedRows;
    /// @brief The reply's `total`.
    std::optional<std::int64_t> total;
    /// @brief The reply's `totalKind`.
    TotalKind totalKind = TotalKind::Exact;
    /// @brief The reply's `undecided`.
    std::int64_t undecided = 0;
    /// @brief The reply's `awaiting`.
    std::span<std::string const> awaiting;
    /// @brief The reply's `failed`.
    std::span<std::string const> failed;
    /// @brief The reply's `indices`.
    std::span<std::int64_t const> indices;
};

/// @brief The rows of one page of a typed window.
/// @tparam Row The row type.
template <typename Row>
struct TypedWindowRows {
    /// @brief The rows, in view order.
    std::vector<Row> rows;

    /// @brief A typed row is whole: every column is held.
    /// @param column Unused.
    /// @return `true`.
    [[nodiscard]] static bool holds(std::string_view column) noexcept {
        static_cast<void>(column);
        return true;
    }
    /// @brief The columns a rest-of-page fetch names: none, since a typed row is whole.
    /// @return Empty.
    [[nodiscard]] static std::vector<std::string> columns() { return {}; }
    /// @brief Keeps the first @p count rows.
    /// @param count Rows kept.
    void truncate(std::size_t count) {
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(std::min(count, rows.size())), rows.end());
    }
};

/// @brief The bookkeeping both windows share: pages, keys, unloaded levels, staleness, revisions, fetches in
///        flight, totals and keyed sequences. Everything except a row's content, which @p Content holds.
///
/// See `PageWindow` for the rules; each member below states its own.
/// @tparam Content A page's content: `holds(column)`, `columns()` and `truncate(count)`.
template <typename Content>
class WindowCore {
public:
    /// @brief Declares the visible rows and the columns shown; returns the fetches to send.
    ///
    /// Covers the visible rows and one page either side, skipping pages past an exact total and pages with a
    /// fetch in flight. A page not held, or held stale, is fetched whole with @p columns. A page held, current and
    /// cut short is fetched from where it ends. A page held, current and complete that lacks some of @p columns
    /// is fetched again for the same rows, naming only those columns. Each returned fetch is in flight.
    ///
    /// Only the first `maxColumns` of @p columns are taken: a page holds at most that many, and never drops one
    /// the latest call named, so a wider set would be dropped and fetched again without end.
    /// @param first   First visible view row.
    /// @param count   Visible rows.
    /// @param columns The columns shown, in order; empty asks for every column.
    /// @return The fetches to send.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
    [[nodiscard]] std::vector<PageFetch> visibleColumns(std::int64_t first, std::int64_t count,
                                                        std::span<std::string const> columns) {
        columns = columns.first(std::min(columns.size(), _maxColumns));
        _visibleFirst = std::max<std::int64_t>(first, 0);
        _visibleCount = std::max<std::int64_t>(count, 0);
        _columns.assign(columns.begin(), columns.end());
        ++_stamp;
        for (auto const& column : columns) {
            _named.insert_or_assign(column, _stamp);
        }
        auto const firstPage = std::max<std::int64_t>((_visibleFirst / _pageSize) - 1, 0);
        auto const lastPage = ((_visibleFirst + std::max<std::int64_t>(_visibleCount, 1) - 1) / _pageSize) + 1;
        _coveredFirst = firstPage;
        _coveredLast = lastPage;
        std::vector<PageFetch> out;
        for (auto page = firstPage; page <= lastPage; ++page) {
            if (exactTotal() && page * _pageSize >= *_total) {
                break;
            }
            if (_inFlight.contains(page)) {
                continue;
            }
            PageFetch fetch{.request = {}, .columns = {}, .query = _query, .revision = _revision};
            WindowFetchKind kind = WindowFetchKind::Whole;
            auto const held = _pages.find(page);
            if (held == _pages.end() || held->second.stale) {
                fetch.request = PageRequest{.offset = page * _pageSize, .limit = _pageSize};
                fetch.columns.assign(columns.begin(), columns.end());
            } else if (!held->second.complete) {
                auto const have = held->second.rows;
                fetch.request = PageRequest{.offset = (page * _pageSize) + have, .limit = _pageSize - have};
                fetch.columns = held->second.content.columns();
                kind = WindowFetchKind::Rest;
            } else {
                if (held->second.rows == 0) {
                    continue;
                }
                for (auto const& column : columns) {
                    if (!held->second.content.holds(column) &&
                        std::ranges::find(fetch.columns, column) == fetch.columns.end()) {
                        fetch.columns.push_back(column);
                    }
                }
                if (fetch.columns.empty()) {
                    continue;
                }
                fetch.request = PageRequest{.offset = page * _pageSize, .limit = held->second.rows};
                kind = WindowFetchKind::Columns;
            }
            _inFlight.insert_or_assign(page, Flight{.fetch = fetch, .kind = kind});
            out.push_back(std::move(fetch));
        }
        return out;
    }

    /// @brief The fetches in flight for pages outside the range the latest `visible` call covered.
    ///
    /// The client cancels them through their stop tokens and tells the window with `cancelled`.
    /// @return The fetches, by page.
    [[nodiscard]] std::vector<PageFetch> superseded() const {
        std::vector<PageFetch> out;
        for (auto const& [page, flight] : _inFlight) {
            if (page < _coveredFirst || page > _coveredLast) {
                out.push_back(flight.fetch);
            }
        }
        return out;
    }

    /// @brief Forgets a fetch the client cancelled; its reply, should one arrive, is ignored.
    /// @param fetch The fetch, as `visible` returned it.
    void cancelled(PageFetch const& fetch) {
        auto const found = _inFlight.find(pageOf(fetch));
        if (found != _inFlight.end() && found->second.fetch == fetch) {
            _inFlight.erase(found);
        }
    }

    /// @brief Plans the answer to a keyed change of the table's record.
    ///
    /// While any page fetch is in flight the keys are also recorded for `owed`. `Nothing` when no row is held;
    /// `Pages` (the window invalidates) without a key function, while `awaiting()` is not empty, or when the keys
    /// to ask for exceed `maxKeys`; otherwise `Rows`, asking for every key of the event, held or not, and for the
    /// first key of each run of consecutive held rows (a probe), which tells the window whether rows shifted.
    /// @param keys The event's keys.
    /// @return The plan.
    [[nodiscard]] RefreshPlan refresh(std::span<RowId const> keys) {
        if (!_inFlight.empty()) {
            _owed.insert(_owed.end(), keys.begin(), keys.end());
        }
        return plan(keys);
    }

    /// @brief Plans the keyed fetch for the events that arrived while a page fetch was in flight.
    ///
    /// The client calls it after each `accept`: an event can precede the reply whose rows it concerns. The
    /// recorded keys are dropped when no page fetch is in flight, and whenever the plan is `Pages`: the
    /// invalidation leaves every reply still in flight older than the window, so the refetch that follows covers
    /// every recorded key.
    /// @return The plan, by the rules of `refresh`; `Nothing` when no key is recorded.
    [[nodiscard]] RefreshPlan owed() {
        if (_owed.empty()) {
            return {};
        }
        auto keys = std::move(_owed);
        _owed.clear();
        auto result = plan(keys);
        if (!_inFlight.empty() && result.kind != Refresh::Pages) {
            _owed = std::move(keys);
        }
        return result;
    }

    /// @brief Takes a new revision and marks every held page stale. Rows stay shown and fetches stay in flight.
    void invalidate() {
        ++_revision;
        for (auto& [page, held] : _pages) {
            held.stale = true;
        }
    }

    /// @brief Records a failed request; the error is shown until a reply arrives.
    /// @param fetch The fetch that failed, as `visible` returned it.
    /// @param error The server's error.
    /// @return `false` when the failure was ignored: its fetch belongs to an earlier query.
    bool fail(PageFetch const& fetch, TableError error) {
        if (fetch.query != _query) {
            return false;
        }
        cancelled(fetch);
        _error = std::move(error);
        return true;
    }

    /// @brief Drops every page, request, recorded key and error and starts a new query: the sort or filter
    ///        changed.
    void reset() {
        ++_query;
        _pages.clear();
        _inFlight.clear();
        _total.reset();
        _totalKind = TotalKind::Exact;
        _undecided = 0;
        _awaiting.clear();
        _failed.clear();
        _totalsRevision = 0;
        _owed.clear();
        _latestAsked.clear();
        _error.reset();
    }

    /// @brief The current query, which `reset` advances.
    /// @return The query's number.
    [[nodiscard]] std::uint64_t query() const noexcept { return _query; }

    /// @brief The current revision, which `invalidate` advances.
    /// @return The revision's number.
    [[nodiscard]] std::uint64_t revision() const noexcept { return _revision; }

    /// @brief The unloaded levels of the held row at a view row.
    /// @param index View row.
    /// @return Views into the window, valid until it next changes; empty when the row is not held.
    [[nodiscard]] UnloadedLevels unloadedLevels(std::int64_t index) const {
        auto const* const page = heldAt(index);
        if (page == nullptr) {
            return {};
        }
        auto const position = static_cast<std::size_t>(index % _pageSize);
        return UnloadedLevels{.levels = page->levels[position], .failed = page->failedLevels[position]};
    }

    /// @brief The view's total rows, when a page reply of this query reported it.
    /// @return The total.
    [[nodiscard]] std::optional<std::int64_t> total() const noexcept { return _total; }

    /// @brief Whether `total()` is the count of the view or a lower bound, as the latest page reply that carried
    ///        a total said.
    /// @return The kind; `Exact` while no total is known.
    [[nodiscard]] TotalKind totalKind() const noexcept { return _totalKind; }

    /// @brief Rows the filter can neither admit nor reject yet, as the latest page reply said.
    /// @return The count.
    [[nodiscard]] std::int64_t undecided() const noexcept { return _undecided; }

    /// @brief The order levels with a load wanted or in flight, as the latest page reply said.
    /// @return The levels.
    [[nodiscard]] std::span<std::string const> awaiting() const noexcept { return _awaiting; }

    /// @brief The order levels whose load failed, as the latest page reply said.
    /// @return The levels.
    [[nodiscard]] std::span<std::string const> failed() const noexcept { return _failed; }

    /// @brief The last error, until a reply clears it.
    /// @return The error, or nothing.
    [[nodiscard]] std::optional<TableError> const& error() const noexcept { return _error; }

    /// @brief Pages held, complete or in part.
    /// @return Page count.
    [[nodiscard]] std::size_t pagesHeld() const noexcept { return _pages.size(); }

    /// @brief Whether any page fetch is in flight.
    /// @return `true` while a page is awaited.
    [[nodiscard]] bool pending() const noexcept { return !_inFlight.empty(); }

    /// @brief How many held keys the window remembers a keyed fetch for; never more than the rows it holds once a
    ///        reply has been accepted, whatever the number of events.
    /// @return The count.
    [[nodiscard]] std::size_t keysRemembered() const noexcept { return _latestAsked.size(); }

protected:
    /// @brief One held page.
    struct Held {
        /// @brief The rows' content.
        Content content;
        /// @brief Each row's key; empty without a key function.
        std::vector<RowId> keys;
        /// @brief Each row's unloaded levels.
        std::vector<std::vector<std::string>> levels;
        /// @brief Each row's failed levels.
        std::vector<std::vector<std::string>> failedLevels;
        /// @brief Rows held.
        std::int64_t rows = 0;
        /// @brief Whether an invalidation or another page's totals made the page out of date.
        bool stale = false;
        /// @brief Whether the page holds every row it will: full, at the total, or ended by an empty reply.
        bool complete = false;
    };

    /// @brief A fetch in flight.
    struct Flight {
        /// @brief The fetch as `visible` returned it.
        PageFetch fetch;
        /// @brief How its reply is stored.
        WindowFetchKind kind = WindowFetchKind::Whole;
    };

    /// @brief An empty window.
    /// @param pageSize   Rows per page.
    /// @param maxPages   Pages held at most.
    /// @param keyed      Whether rows have keys, so that a change can be answered with a keyed fetch.
    /// @param maxKeys    Keys one keyed fetch asks for at most.
    /// @param maxColumns Columns a page holds at most.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - sizes and bounds, each documented above
    WindowCore(std::int64_t pageSize, std::size_t maxPages, bool keyed, std::size_t maxKeys, std::size_t maxColumns)
        : _pageSize{std::max<std::int64_t>(pageSize, 1)},
          _maxPages{std::max<std::size_t>(maxPages, 1)},
          _maxKeys{maxKeys},
          _maxColumns{std::max<std::size_t>(maxColumns, 1)},
          _keyed{keyed} {}

    /// @brief Stores a page reply.
    ///
    /// Ignored when the fetch belongs to another query or is not the one in flight for its page. A whole-page
    /// reply replaces the page, which stays stale when its fetch is older than the window's revision; a
    /// rest-of-page reply is appended; a column reply is merged when its keys are the page's, row for row, and
    /// invalidates the window otherwise. A reply not older than the totals held sets them; one whose total or
    /// undecided count differs marks every other held page stale. An exact total drops every held row past it.
    /// @tparam Fill `void(Content&, WindowFetchKind, std::size_t take)`: stores the first `take` rows of the reply.
    /// @param fetch The fetch the reply answers.
    /// @param reply What the window reads of the reply.
    /// @param fill  Stores the reply's content.
    /// @return `false` when the reply was ignored.
    template <typename Fill>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): called more than once, so it is never forwarded.
    bool acceptReply(PageFetch const& fetch, WindowReply const& reply, Fill&& fill) {
        if (fetch.query != _query) {
            return false;
        }
        auto const index = pageOf(fetch);
        auto const flight = _inFlight.find(index);
        if (flight == _inFlight.end() || flight->second.fetch != fetch) {
            return false;
        }
        auto const kind = flight->second.kind;
        _inFlight.erase(flight);
        _error.reset();
        if (_keyed && reply.keys.size() != reply.rows) {
            invalidate();
            _error = TableError{.code = TableErrorCode::InvalidValue,
                                .column = {},
                                .message = "a page reply of " + std::to_string(reply.keys.size()) + " keys and " +
                                           std::to_string(reply.rows) + " rows"};
            return true;
        }
        bool const replyEmpty = reply.rows == 0;
        bool stored = false;
        if (kind == WindowFetchKind::Columns) {
            auto const held = _pages.find(index);
            if (held != _pages.end()) {
                if (sameKeys(held->second, reply)) {
                    fill(held->second.content, kind, reply.rows);
                    mergeLevels(held->second, reply);
                } else {
                    invalidate();
                }
            }
        } else if (kind == WindowFetchKind::Whole) {
            auto& held = _pages[index];
            auto const take = std::min(reply.rows, static_cast<std::size_t>(_pageSize));
            held = Held{};
            fill(held.content, kind, take);
            appendRows(held, reply, take);
            held.stale = fetch.revision != _revision;
            stored = true;
        } else {
            auto const held = _pages.find(index);
            if (held != _pages.end() && fetch.request.offset == (index * _pageSize) + held->second.rows) {
                auto const room = static_cast<std::size_t>(_pageSize - held->second.rows);
                auto const take = std::min(room, reply.rows);
                fill(held->second.content, kind, take);
                appendRows(held->second, reply, take);
                stored = true;
            }
        }
        if (fetch.revision >= _totalsRevision) {
            adoptTotals(index, fetch.revision, reply);
        }
        // After the totals: the reply's own total decides whether a page it cut short is complete, so the first page
        // of a short view is complete at once, with no fetch for a rest that does not exist.
        if (stored) {
            if (auto const held = _pages.find(index); held != _pages.end()) {
                completeIf(index, held->second, replyEmpty);
            }
        }
        dropPastTotal();
        evict();
        forgetUnheldKeys();
        return true;
    }

    /// @brief Patches held rows from a keyed reply.
    ///
    /// Ignored (empty) for another query or without a key function. A reply row whose key a later fetch asked for
    /// is ignored. The window invalidates instead of patching when a requested key it holds has no row, a held
    /// key is at another view row, an unheld key lies inside a held page, or, on the reply to the latest keyed
    /// fetch, the totals differ from the window's.
    /// @tparam Replace `void(Held&, std::size_t at, std::size_t from)`: replaces held row `at` with reply row
    ///                 `from`.
    /// @param fetch   The fetch the reply answers.
    /// @param reply   What the window reads of the reply.
    /// @param replace Stores one row's content.
    /// @return The view rows replaced, ascending.
    template <typename Replace>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward,readability-function-cognitive-complexity): called more than once, so it is never forwarded. one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
    std::vector<std::int64_t> patchRows(RowsFetch const& fetch, WindowReply const& reply, Replace&& replace) {
        if (fetch.query != _query || !_keyed) {
            return {};
        }
        if (reply.keys.size() != reply.rows || reply.indices.size() != reply.rows) {
            invalidate();
            return {};
        }
        std::unordered_map<RowId, std::int64_t> held;
        for (auto const& [page, content] : _pages) {
            for (std::size_t at = 0; at < content.keys.size(); ++at) {
                held.emplace(content.keys[at], (page * _pageSize) + static_cast<std::int64_t>(at));
            }
        }
        auto const latest = [this](RowId const& key) {
            auto const found = _latestAsked.find(key);
            return found == _latestAsked.end() ? std::uint64_t{0} : found->second;
        };
        bool shifted = false;
        std::unordered_set<RowId> answered;
        std::vector<std::size_t> current;
        for (std::size_t from = 0; from < reply.rows; ++from) {
            auto const& key = reply.keys[from];
            if (latest(key) > fetch.sequence) {
                continue;
            }
            answered.insert(key);
            current.push_back(from);
            auto const index = reply.indices[from];
            auto const found = held.find(key);
            if (found != held.end()) {
                shifted = shifted || found->second != index;
            } else if (index >= 0 && _pages.contains(index / _pageSize)) {
                shifted = true;
            }
        }
        for (auto const& key : fetch.keys) {
            if (latest(key) == fetch.sequence && held.contains(key) && !answered.contains(key)) {
                shifted = true;
            }
        }
        if (fetch.sequence == _latestKeyed &&
            ((reply.total && (reply.total != _total || reply.totalKind != _totalKind)) ||
             reply.undecided != _undecided)) {
            shifted = true;
        }
        if (shifted) {
            invalidate();
            return {};
        }
        std::vector<std::int64_t> out;
        for (auto const from : current) {
            auto const index = reply.indices[from];
            if (!held.contains(reply.keys[from])) {
                continue;
            }
            auto& page = _pages.at(index / _pageSize);
            auto const position = static_cast<std::size_t>(index % _pageSize);
            replace(page, position, from);
            page.levels[position].clear();
            page.failedLevels[position].clear();
            if (auto const* const unloaded = unloadedOf(reply, from)) {
                page.levels[position] = unloaded->levels;
                page.failedLevels[position] = unloaded->failed;
            }
            out.push_back(index);
        }
        std::ranges::sort(out);
        return out;
    }

    /// @brief The page holding a view row, when the row is held.
    /// @param index View row.
    /// @return The page, or null.
    [[nodiscard]] Held const* heldAt(std::int64_t index) const {
        if (index < 0) {
            return nullptr;
        }
        auto const found = _pages.find(index / _pageSize);
        if (found == _pages.end() || index % _pageSize >= found->second.rows) {
            return nullptr;
        }
        return &found->second;
    }

    /// @brief Whether the latest `visible` call named a column, so a page must keep it.
    /// @param column The column.
    /// @return `true` when it is among the columns shown now.
    [[nodiscard]] bool shownNow(std::string_view column) const {
        return std::ranges::find(_columns, column) != _columns.end();
    }

    /// @brief The latest `visible` call that named a column.
    /// @param column The column.
    /// @return The call's stamp; 0 when no call named it.
    [[nodiscard]] std::uint64_t namedAt(std::string_view column) const {
        auto const found = _named.find(column);
        return found == _named.end() ? std::uint64_t{0} : found->second;
    }

    /// @brief Rows per page.
    /// @return The page size.
    [[nodiscard]] std::int64_t pageSize() const noexcept { return _pageSize; }

    /// @brief Columns a page holds at most.
    /// @return The bound.
    [[nodiscard]] std::size_t maxColumns() const noexcept { return _maxColumns; }

private:
    [[nodiscard]] std::int64_t pageOf(PageFetch const& fetch) const noexcept {
        return fetch.request.offset / _pageSize;
    }

    // Whether a known total bounds the view; a lower bound does not.
    [[nodiscard]] bool exactTotal() const noexcept { return _total && _totalKind == TotalKind::Exact; }

    [[nodiscard]] static UnloadedRow const* unloadedOf(WindowReply const& reply, std::size_t row) {
        auto const found =
            std::ranges::lower_bound(reply.unloadedRows, static_cast<std::int64_t>(row), {}, &UnloadedRow::index);
        return found != reply.unloadedRows.end() && std::cmp_equal(found->index, row) ? &*found : nullptr;
    }

    [[nodiscard]] bool sameKeys(Held const& held, WindowReply const& reply) const {
        if (std::cmp_not_equal(reply.rows, held.rows)) {
            return false;
        }
        return !_keyed || std::ranges::equal(held.keys, reply.keys);
    }

    void appendRows(Held& held, WindowReply const& reply, std::size_t take) {
        for (std::size_t row = 0; row < take; ++row) {
            if (_keyed) {
                held.keys.push_back(reply.keys[row]);
            }
            auto const* const unloaded = unloadedOf(reply, row);
            held.levels.push_back(unloaded != nullptr ? unloaded->levels : std::vector<std::string>{});
            held.failedLevels.push_back(unloaded != nullptr ? unloaded->failed : std::vector<std::string>{});
        }
        held.rows += static_cast<std::int64_t>(take);
    }

    static void unite(std::vector<std::string>& into, std::vector<std::string> const& more) {
        into.insert(into.end(), more.begin(), more.end());
        std::ranges::sort(into);
        auto const [first, last] = std::ranges::unique(into);
        into.erase(first, last);
    }

    void mergeLevels(Held& held, WindowReply const& reply) {
        for (auto const& unloaded : reply.unloadedRows) {
            if (unloaded.index < 0 || unloaded.index >= held.rows) {
                continue;
            }
            auto const position = static_cast<std::size_t>(unloaded.index);
            unite(held.levels[position], unloaded.levels);
            unite(held.failedLevels[position], unloaded.failed);
        }
    }

    void completeIf(std::int64_t index, Held& held, bool replyEmpty) {
        auto const end = (index * _pageSize) + held.rows;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): exactTotal() holds, so the total is known.
        if (held.rows >= _pageSize || (exactTotal() && end >= *_total) || replyEmpty) {
            held.complete = true;
        }
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): page index, then revision, as the window's callers hold them.
    void adoptTotals(std::int64_t index, std::uint64_t revision, WindowReply const& reply) {
        bool const differs =
            (reply.total && (reply.total != _total || reply.totalKind != _totalKind)) || reply.undecided != _undecided;
        _totalsRevision = revision;
        _undecided = reply.undecided;
        _awaiting.assign(reply.awaiting.begin(), reply.awaiting.end());
        _failed.assign(reply.failed.begin(), reply.failed.end());
        if (reply.total) {
            _total = reply.total;
            _totalKind = reply.totalKind;
        }
        if (differs) {
            for (auto& [page, held] : _pages) {
                if (page != index) {
                    held.stale = true;
                }
            }
        }
    }

    // A key that left the held pages needs no memory of the fetch that last asked for it.
    void forgetUnheldKeys() {
        if (_latestAsked.empty()) {
            return;
        }
        std::unordered_set<RowId> heldKeys;
        for (auto const& [page, held] : _pages) {
            heldKeys.insert(held.keys.begin(), held.keys.end());
        }
        std::erase_if(_latestAsked, [&](auto const& entry) { return !heldKeys.contains(entry.first); });
    }

    // A view that shrank holds nothing past its end, and awaits no fetch there: a page past an exact total is
    // never fetched again, so a row held there would stay shown, and its key would be a probe no reply answers.
    void dropPastTotal() {
        if (!exactTotal()) {
            return;
        }
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): exactTotal() holds, so the total is known.
        auto const total = *_total;
        std::erase_if(_pages, [&](auto const& entry) { return entry.first * _pageSize >= total; });
        std::erase_if(_inFlight, [&](auto const& entry) { return entry.first * _pageSize >= total; });
        for (auto& [page, held] : _pages) {
            auto const keep = total - (page * _pageSize);
            if (held.rows > keep) {
                auto const count = static_cast<std::size_t>(keep);
                held.content.truncate(count);
                if (_keyed) {
                    held.keys.resize(count);
                }
                held.levels.resize(count);
                held.failedLevels.resize(count);
                held.rows = keep;
                held.complete = true;
            }
        }
    }

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

    [[nodiscard]] RefreshPlan plan(std::span<RowId const> keys) {
        if (std::ranges::none_of(_pages, [](auto const& entry) { return entry.second.rows > 0; })) {
            return {};
        }
        if (!_keyed || !_awaiting.empty()) {
            invalidate();
            return RefreshPlan{.kind = Refresh::Pages, .rows = {}};
        }
        std::vector<RowId> asked;
        asked.reserve(keys.size() + _pages.size());
        std::unordered_set<RowId> seen;
        seen.reserve(keys.size() + _pages.size());
        for (auto const& key : keys) {
            if (seen.insert(key).second) {
                asked.push_back(key);
            }
        }
        std::int64_t runEnd = -1;
        for (auto const& [page, held] : _pages) {
            if (held.rows == 0) {
                continue;
            }
            auto const start = page * _pageSize;
            if (start != runEnd && seen.insert(held.keys.front()).second) {
                asked.push_back(held.keys.front());
            }
            runEnd = start + held.rows;
        }
        if (asked.size() > _maxKeys) {
            invalidate();
            return RefreshPlan{.kind = Refresh::Pages, .rows = {}};
        }
        // Only a held key is remembered: an older reply's row for it must not overwrite the newer one, and its
        // absence from the latest reply means it left the view. A key the window does not hold needs no memory, since
        // a row for it is either outside the held pages (ignored) or inside them (the window refetches).
        auto const sequence = ++_sequence;
        std::unordered_set<RowId> heldKeys;
        for (auto const& [page, held] : _pages) {
            heldKeys.insert(held.keys.begin(), held.keys.end());
        }
        for (auto const& key : asked) {
            if (heldKeys.contains(key)) {
                _latestAsked.insert_or_assign(key, sequence);
            }
        }
        _latestKeyed = sequence;
        return RefreshPlan{
            .kind = Refresh::Rows,
            .rows = RowsFetch{.keys = std::move(asked), .columns = _columns, .query = _query, .sequence = sequence}};
    }

    std::int64_t _pageSize;
    std::size_t _maxPages;
    std::size_t _maxKeys;
    std::size_t _maxColumns;
    bool _keyed;
    std::uint64_t _query = 0;
    std::uint64_t _revision = 0;
    std::map<std::int64_t, Held> _pages;
    std::map<std::int64_t, Flight> _inFlight;
    std::optional<std::int64_t> _total;
    TotalKind _totalKind = TotalKind::Exact;
    std::int64_t _undecided = 0;
    std::vector<std::string> _awaiting;
    std::vector<std::string> _failed;
    std::uint64_t _totalsRevision = 0;
    std::uint64_t _sequence = 0;
    std::unordered_map<RowId, std::uint64_t> _latestAsked;
    std::uint64_t _latestKeyed = 0;
    std::vector<RowId> _owed;
    std::vector<std::string> _columns;
    std::map<std::string, std::uint64_t, std::less<>> _named;
    std::uint64_t _stamp = 0;
    std::optional<TableError> _error;
    std::int64_t _visibleFirst = 0;
    std::int64_t _visibleCount = 0;
    std::int64_t _coveredFirst = 0;
    std::int64_t _coveredLast = -1;
};

}  // namespace detail

/// @brief The pages a `server`-mode table holds around the visible rows, kept current under changes.
///
/// The client calls `visible` as the view scrolls and sends the request of each fetch it returns, then hands the
/// fetch back with its reply to `accept` (or with its error to `fail`). A change of sort or filter is a new
/// query: call `reset`. A reply to a fetch made before the last `reset`, or to a fetch that is no longer the one
/// in flight for its page, is ignored, so a late page never shows over a newer one.
///
/// A change of the table's record goes to `refresh` (its keys) or `invalidate` (the whole type, a resync); the
/// client sends the keyed fetch a `Rows` plan carries and hands the reply to `patch`, and calls `owed` after each
/// `accept`. The window has no clock and sends nothing: it returns what to fetch.
///
/// A server may return fewer rows than a fetch asked for (`validate` clamps the limit to its `maxLimit`): the
/// window then fetches the rest of the page from where the reply ended. A page is complete when it is full,
/// reaches the known total, or a reply to it is empty.
/// @tparam Row The row type.
template <typename Row>
class PageWindow : public detail::WindowCore<detail::TypedWindowRows<Row>> {
    using Core = detail::WindowCore<detail::TypedWindowRows<Row>>;

public:
    /// @brief An empty window.
    /// @param pageSize   Rows per page (default 200).
    /// @param maxPages   Pages held at most; the farthest from the visible rows go first.
    /// @param key        The key of a row; without one, every change refetches the visible pages.
    /// @param maxKeys    Keys one keyed fetch asks for at most; more refetch the pages instead.
    /// @param maxColumns Unused by a typed window, whose rows are whole; kept so both windows construct alike.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - sizes and bounds, each documented above
    explicit PageWindow(std::int64_t pageSize = 200, std::size_t maxPages = 8, RowKey<Row> key = {},
                        std::size_t maxKeys = 1000, std::size_t maxColumns = 96)
        : Core{pageSize, maxPages, static_cast<bool>(key), maxKeys, maxColumns}, _key{std::move(key)} {}

    /// @brief Declares the visible rows; returns the fetches to send.
    ///
    /// Covers the visible rows and one page either side, skipping complete pages, pages in flight, and pages past
    /// an exact total. A page not held or held stale is fetched whole; a page held in part is fetched from where
    /// it ends. The returned pages are marked in flight.
    /// @param first   First visible view row.
    /// @param count   Visible rows.
    /// @param columns Ignored: a typed row is whole, and every fetch names no column.
    /// @return The fetches to send.
    [[nodiscard]] std::vector<PageFetch> visible(std::int64_t first, std::int64_t count,
                                                 std::span<std::string const> columns = {}) {
        static_cast<void>(columns);
        return this->visibleColumns(first, count, {});
    }

    /// @brief Stores a page reply.
    /// @param fetch The fetch the reply answers, as `visible` returned it.
    /// @param page  The page the server returned.
    /// @return `false` when the reply was ignored: its fetch belongs to an earlier query, or is not the one in
    ///         flight for its page.
    bool accept(PageFetch const& fetch, Page<Row> page) {
        auto const keys = keysOf(page);
        return this->acceptReply(
            fetch, replyOf(page, keys),
            [&page](detail::TypedWindowRows<Row>& content, detail::WindowFetchKind kind, std::size_t take) {
                auto const end = page.rows.begin() + static_cast<std::ptrdiff_t>(take);
                if (kind == detail::WindowFetchKind::Whole) {
                    content.rows.assign(std::make_move_iterator(page.rows.begin()), std::make_move_iterator(end));
                } else if (kind == detail::WindowFetchKind::Rest) {
                    content.rows.insert(content.rows.end(), std::make_move_iterator(page.rows.begin()),
                                        std::make_move_iterator(end));
                }
            });
    }

    /// @brief Patches held rows from the reply to a `RowsFetch`.
    /// @param fetch The keyed fetch the reply answers, as `refresh` or `owed` returned it.
    /// @param reply The keyed reply.
    /// @return The view rows replaced, ascending; empty when the reply was ignored or the window invalidated.
    std::vector<std::int64_t> patch(RowsFetch const& fetch, Page<Row> reply) {
        if (!_key) {
            return {};
        }
        auto const keys = keysOf(reply);
        return this->patchRows(fetch, replyOf(reply, keys),
                               [&reply](Core::Held& page, std::size_t position, std::size_t from) {
                                   page.content.rows[position] = std::move(reply.rows[from]);
                               });
    }

    /// @brief The row at a view row, when it is held.
    /// @param index View row.
    /// @return The row, or null.
    [[nodiscard]] Row const* row(std::int64_t index) const {
        auto const* const page = this->heldAt(index);
        return page == nullptr ? nullptr : &page->content.rows[static_cast<std::size_t>(index % this->pageSize())];
    }

private:
    [[nodiscard]] std::vector<RowId> keysOf(Page<Row> const& page) const {
        std::vector<RowId> keys;
        if (_key) {
            keys.reserve(page.rows.size());
            for (auto const& row : page.rows) {
                keys.push_back(_key(row));
            }
        }
        return keys;
    }

    [[nodiscard]] static detail::WindowReply replyOf(Page<Row> const& page, std::vector<RowId> const& keys) {
        return detail::WindowReply{.keys = keys,
                                   .rows = page.rows.size(),
                                   .unloadedRows = page.unloadedRows,
                                   .total = page.total,
                                   .totalKind = page.totalKind,
                                   .undecided = page.undecided,
                                   .awaiting = page.awaiting,
                                   .failed = page.failed,
                                   .indices = page.indices};
    }

    RowKey<Row> _key;
};

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

namespace morph::table::detail {

/// @brief Whether a page member is left out of its JSON: `failed`, `indices`, `viewRevision` and `unmatched` when
///        empty.
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
        return (key == "failed" || key == "indices" || key == "viewRevision" || key == "unmatched") && value.empty();
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
    // NOLINTNEXTLINE(readability-identifier-naming): glaze looks this hook up by name.
    static constexpr bool skip_if(T const& member, std::string_view key, glz::meta_context const& context) {
        static_cast<void>(context);
        return morph::table::detail::omittedPageMember(member, key);
    }
};

/// @brief The JSON of `morph::table::TableQuery`: every member by reflection, an empty `retain` left out, so a query
///        that retains nothing carries no `retain` member.
template <>
struct glz::meta<morph::table::TableQuery> {
    /// @brief Whether a member is left out of the JSON written.
    /// @tparam T The member's type.
    /// @param member  The member.
    /// @param key     Its JSON name.
    /// @param context Unused.
    /// @return `true` for an empty `retain`.
    template <typename T>
    // NOLINTNEXTLINE(readability-identifier-naming): glaze looks this hook up by name.
    static constexpr bool skip_if(T const& member, std::string_view key, glz::meta_context const& context) {
        static_cast<void>(context);
        if constexpr (requires { member.empty(); }) {
            return key == "retain" && member.empty();
        } else {
            return false;
        }
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
    // NOLINTNEXTLINE(readability-identifier-naming): glaze looks this hook up by name.
    static constexpr bool skip_if(T const& member, std::string_view key, glz::meta_context const& context) {
        static_cast<void>(context);
        return morph::table::detail::omittedPageMember(member, key);
    }
};

/// @brief JSON names of `morph::table::DateBucket`: `value`, `year`, `month` and `day`.
template <>
struct glz::meta<morph::table::DateBucket> {
    using enum morph::table::DateBucket;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("value", Value, "year", Year, "month", Month, "day", Day);
};

/// @brief JSON names of `morph::table::TotalKind`: `exact` and `atLeast`.
template <>
struct glz::meta<morph::table::TotalKind> {
    using enum morph::table::TotalKind;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("exact", Exact, "atLeast", AtLeast);
};

/// @brief The JSON of `morph::table::MeasureValue`: `{"num", "den", "unit"}`.
template <>
struct glz::meta<morph::table::MeasureValue> {
    /// @brief Read and written through its wire shape.
    static constexpr auto value =
        glz::custom<&morph::table::MeasureValue::setWire, &morph::table::MeasureValue::getWire>;
    /// @brief The schema's name.
    static constexpr std::string_view name = "MeasureValue";
};

namespace glz::detail {

/// @brief The schema of `MeasureValue`: its wire object, so its properties are `num`, `den` and `unit`.
template <>
struct to_json_schema<morph::table::MeasureValue> {
    /// @brief Writes the wire shape's schema.
    /// @tparam Opts The write options.
    /// @param outSchema The schema written.
    /// @param defs      The definitions.
    template <auto Opts>
    static void op(auto& outSchema, auto& defs) {
        to_json_schema<morph::table::MeasureValue::Wire>::template op<Opts>(outSchema, defs);
    }
};

}  // namespace glz::detail
