// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/table/query.hpp>.

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
#include <glaze/glaze.hpp>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/payload_schema.hpp>
#include <morph/detail/reflected_member.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- row and column indices are bounded by the snapshot's own counts, and the sort and filter loops index once per comparison, where at() would check every access

namespace morph::table {

void MembershipDigest::add(RowId const& key) noexcept {
    std::uint64_t hash = 0;
    if (auto const* const number = std::get_if<std::int64_t>(&key)) {
        std::array<char, 24> text{'i', ':'};
        char const* const end = std::to_chars(std::next(text.data(), 2), std::to_address(text.end()), *number).ptr;
        hash = model::detail::fnv1a64(std::string_view{text.data(), static_cast<std::size_t>(end - text.data())});
    } else {
        constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
        constexpr std::uint64_t kPrime = 1099511628211ULL;
        // FNV-1a over "s:" and the text, without building the joined string.
        hash = kOffsetBasis;
        auto const step = [&hash](char byte) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
            hash *= kPrime;
        };
        step('s');
        step(':');
        // The key is a number or a name, and the number was handled above: `get_if` rather than `get`, which
        // would throw out of a `noexcept` function.
        for (char const byte : *std::get_if<std::string>(&key)) {
            step(byte);
        }
    }
    std::uint64_t mixed = hash + 0x9E3779B97F4A7C15ULL;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
    mixed ^= mixed >> 31U;
    ++_count;
    _sum += hash;
    _mix += mixed;
}

}  // namespace morph::table

namespace morph::table::detail {

TableError limitError(std::string column, std::string message) {
    return TableError{
        .code = TableErrorCode::LimitExceeded, .column = std::move(column), .message = std::move(message)};
}

bool entryTooLong(FilterEntry const& entry, std::size_t max) {
    auto const tooLong = [max](std::optional<std::string> const& value) { return value && value->size() > max; };
    return tooLong(entry.eq) || tooLong(entry.ne) || tooLong(entry.lt) || tooLong(entry.le) || tooLong(entry.gt) ||
           tooLong(entry.ge) || tooLong(entry.contains) || tooLong(entry.startsWith) || tooLong(entry.endsWith) ||
           (entry.between && ((*entry.between)[0].size() > max || (*entry.between)[1].size() > max)) ||
           (entry.oneOf &&
            std::ranges::any_of(*entry.oneOf, [max](std::string const& member) { return member.size() > max; }));
}

std::optional<TableError> checkColumn(std::string const& columnId, std::span<ColumnInfo const> columns,
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

std::vector<std::string> orderedColumns(std::span<ColumnInfo const> columns, std::vector<std::string> const& asked) {
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

std::optional<TableError> SetTally::add(std::span<FilterEntry const> entries, std::string const& column,
                                        QueryLimits const& limits) {
    for (auto const& entry : entries) {
        if (!entry.oneOf) {
            continue;
        }
        if (entry.oneOf->size() > limits.maxSetValues) {
            return limitError(column, "too many values in one set");
        }
        values += entry.oneOf->size();
        for (auto const& member : *entry.oneOf) {
            bytes += member.size();
        }
    }
    return std::nullopt;
}

std::optional<std::span<std::string const>> groupColumns(std::span<ColumnGroupInfo const> groups,
                                                         std::string_view groupId) {
    auto const found = std::ranges::find(groups, groupId, &ColumnGroupInfo::id);
    if (found == groups.end()) {
        return std::nullopt;
    }
    return std::span<std::string const>{found->columns};
}

TableError unknownGroup(std::string const& groupId) {
    return TableError{
        .code = TableErrorCode::UnknownColumn, .column = groupId, .message = "no column group " + groupId};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a run of independent rules, one `if` each, in the order they are reported; splitting scatters that order.
std::optional<TableError> checkFilters(FilterSpec const& filters, std::span<ColumnInfo const> columns,
                                       QueryLimits const& limits, std::function<bool(std::string_view)> const& mayRead,
                                       std::span<ColumnGroupInfo const> groups) {
    if (filters.columns.size() > limits.maxFilterColumns) {
        return limitError({}, "too many filtered columns");
    }
    SetTally sets;
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
        if (auto error = sets.add(filter.include, columnId, limits)) {
            return error;
        }
        if (auto error = sets.add(filter.exclude, columnId, limits)) {
            return error;
        }
    }
    if (filters.groups.size() > limits.maxGroups) {
        return limitError({}, "too many filter groups");
    }
    for (auto const& group : filters.groups) {
        if (group.columns.size() > limits.maxGroupColumns ||
            group.include.size() + group.exclude.size() > limits.maxEntries) {
            return limitError({}, "filter group too large");
        }
        std::span<std::string const> named = group.columns;
        if (!group.group.empty()) {
            auto const declared = groupColumns(groups, group.group);
            if (!declared) {
                return unknownGroup(group.group);
            }
            named = *declared;
        }
        for (auto const& columnId : named) {
            if (auto error = checkColumn(columnId, columns, mayRead)) {
                return error;
            }
        }
        if (anyTooLong(group.include, limits.maxTextLength) || anyTooLong(group.exclude, limits.maxTextLength)) {
            return limitError({}, "filter value too long");
        }
        if (auto error = sets.add(group.include, {}, limits)) {
            return error;
        }
        if (auto error = sets.add(group.exclude, {}, limits)) {
            return error;
        }
    }
    if (sets.values > limits.maxQuerySetValues) {
        return limitError({}, "too many set values in one query");
    }
    if (sets.bytes > limits.maxQuerySetBytes) {
        return limitError({}, "set values too long in one query");
    }
    return std::nullopt;
}

std::optional<TableError> checkDistinct(DistinctRequest& request, std::span<ColumnInfo const> columns,
                                        QueryLimits const& limits,
                                        std::function<bool(std::string_view)> const& mayRead,
                                        std::span<ColumnGroupInfo const> groups) {
    auto const invalid = [](std::string message) {
        return TableError{.code = TableErrorCode::InvalidSpec, .column = {}, .message = std::move(message)};
    };
    if (request.column.empty() == request.group.empty()) {
        return invalid("a distinct request names a column or a group");
    }
    std::span<std::string const> listed{&request.column, 1};
    if (!request.group.empty()) {
        auto const declared = groupColumns(groups, request.group);
        if (!declared) {
            return unknownGroup(request.group);
        }
        if (declared->empty()) {
            return invalid("column group " + request.group + " has no columns");
        }
        listed = *declared;
    }
    for (auto const& columnId : listed) {
        if (auto error = checkColumn(columnId, columns, mayRead)) {
            return error;
        }
    }
    if (request.search.size() > limits.maxTextLength) {
        return limitError({}, "distinct search too long");
    }
    if (request.offset < 0 || request.offset > limits.maxOffset) {
        return limitError({}, "distinct offset out of range");
    }
    if (request.limit < 0) {
        return limitError({}, "distinct limit is negative");
    }
    request.limit = std::min(request.limit, limits.maxLimit);
    auto const& first = listed.front();
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): validate() resolved every column the request names.
    auto const kind = columns[*findColumn(columns, first)].kind;
    if (kind == ColumnKind::Custom) {
        return TableError{.code = TableErrorCode::UnsupportedOperator,
                          .column = first,
                          .message = "column " + first + " has no values to list"};
    }
    if (request.bucket != DateBucket::Value && kind != ColumnKind::Date && kind != ColumnKind::DateTime) {
        return invalid("a date bucket on column " + first + ", which holds no dates");
    }
    return std::nullopt;
}

}  // namespace morph::table::detail

namespace morph::table {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a run of independent rules, one `if` each, in the order they are reported; splitting scatters that order.
std::expected<TableQuery, TableError> validate(TableQuery query, std::span<ColumnInfo const> columns,
                                               QueryLimits const& limits,
                                               std::function<bool(std::string_view)> const& mayRead,
                                               std::span<ColumnGroupInfo const> groups) {
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
    if (auto error = detail::checkFilters(query.filters, columns, limits, mayRead, groups)) {
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
    if (query.retain.size() > limits.maxRetained) {
        return std::unexpected(detail::limitError({}, "too many retained rows"));
    }
    if (std::ranges::any_of(query.retain, [&limits](RowId const& key) {
            auto const* const text = std::get_if<std::string>(&key);
            return text != nullptr && text->size() > limits.maxTextLength;
        })) {
        return std::unexpected(detail::limitError({}, "retained key too long"));
    }
    if (query.distinct) {
        if (query.keys) {
            return std::unexpected(TableError{
                .code = TableErrorCode::InvalidSpec, .column = {}, .message = "a query is keyed or distinct"});
        }
        if (auto error = detail::checkDistinct(*query.distinct, columns, limits, mayRead, groups)) {
            return std::unexpected(std::move(*error));
        }
    }
    return query;
}

}  // namespace morph::table

namespace morph::table::detail {

std::vector<std::string> sortedLevels(std::vector<std::string_view> levels) {
    std::ranges::sort(levels);
    auto const [first, last] = std::ranges::unique(levels);
    levels.erase(first, last);
    return {levels.begin(), levels.end()};
}

}  // namespace morph::table::detail

namespace morph::table {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a run of independent rules, one `if` each, in the order they are reported; splitting scatters that order.
LevelsNeeded levelsNeeded(TableQuery const& query, std::span<ColumnInfo const> columns,
                          std::function<bool(std::string_view)> const& mayRead, Services const& services,
                          std::span<ColumnGroupInfo const> groups) {
    // The columns that have a level, sorted by id: each column the query names is then one lookup.
    std::vector<std::pair<std::string_view, std::string_view>> levelled;
    for (auto const& column : columns) {
        if (!column.level.empty()) {
            levelled.emplace_back(column.id, column.level);
        }
    }
    std::ranges::sort(levelled);
    auto const levelOf = [&levelled](std::string_view columnId) {
        auto const found =
            std::ranges::lower_bound(levelled, columnId, {}, [](auto const& entry) { return entry.first; });
        return found != levelled.end() && found->first == columnId ? found->second : std::string_view{};
    };
    std::vector<std::string_view> order;
    std::vector<std::string_view> display;
    auto const add = [&levelOf](std::vector<std::string_view>& out, std::string_view columnId) {
        if (auto const level = levelOf(columnId); !level.empty()) {
            out.push_back(level);
        }
    };
    for (auto const& key : query.sort) {
        add(order, key.column);
    }
    if (!query.filters.empty()) {
        if (auto const filter = compileFilterGuarded(query.filters, columns, services, groups)) {
            for (auto const column : filter->columnsUsed()) {
                if (!columns[column].level.empty()) {
                    order.push_back(columns[column].level);
                }
            }
        }
    }
    if (query.distinct) {
        auto const named =
            query.distinct->group.empty()
                ? std::span<std::string const>{&query.distinct->column, 1}
                : detail::groupColumns(groups, query.distinct->group).value_or(std::span<std::string const>{});
        for (auto const& columnId : named) {
            add(order, columnId);
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

}  // namespace morph::table

namespace morph::table::detail {

std::expected<ComputedView, TableError> computeView(DataSource const& source, TableQuery const& validated,
                                                    ApplyOptions const& options) {
    auto const columns = source.columns();
    for (auto const& key : validated.sort) {
        if (auto const column = findColumn(columns, key.column)) {
            if (auto missing = unitsMissing(columns[*column], options.services)) {
                return std::unexpected(std::move(*missing));
            }
        }
    }
    // One copy of the services for this call, the filter's operands included:
    // a collator that is not safe to share clones itself here, so concurrent
    // calls never fold on the one the caller passed.
    auto services = options.services.forTask();
    auto filter = compileFilterGuarded(validated.filters, columns, services, source.groups());
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
    auto const snapshot = input.snapshot;
    ViewJob job{std::move(input)};
    static_cast<void>(job.step(kNoDeadline, ::core::async::StopToken{}));
    if (job.failure()) {
        return std::unexpected(*job.failure());
    }
    auto result = job.takeResult();
    ComputedView out{.rows = std::move(result.view), .undecided = std::move(result.undecided), .digest = {}};
    for (auto const row : out.rows) {
        out.digest.add(snapshot->rowId(row));
    }
    return out;
}

void keyedRows(ComputedView const& view, RowSnapshot const& snapshot, std::span<RowId const> keys,
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

PageRows pageRows(ComputedView const& view, RowSnapshot const& snapshot, TableQuery const& validated,
                  QueryLimits const& limits, std::span<std::uint32_t const> viewRowOf,
                  SourceRowOf const& sourceRowOf) {
    PageRows out;
    out.viewRevision = view.digest.text();
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

MergedView mergeRetained(std::span<std::uint32_t const> baseOrder, std::int64_t baseUndecided,
                         MembershipDigest baseDigest, std::span<std::uint32_t const> retainedRows,
                         std::function<bool(std::uint32_t, std::uint32_t)> const& before,
                         std::function<Match(std::uint32_t)> const& match,
                         std::function<RowId(std::uint32_t)> const& keyOf) {
    auto merged = mergeRetainedOrder(baseOrder, retainedRows, before, match);
    MergedView out{.order = std::move(merged.order),
                   .unmatched = std::move(merged.unmatched),
                   .undecided = baseUndecided - merged.undecided,
                   .digest = baseDigest};
    for (auto const row : merged.admitted) {
        out.digest.add(keyOf(row));
    }
    return out;
}

std::uint32_t SparseKeys::local(std::uint32_t row) {
    auto const [found, made] = _local.try_emplace(row, static_cast<std::uint32_t>(_rows.size()));
    if (made) {
        if (_rows.size() == _capacity) {
            grow(_capacity * 2);
        }
        _rows.push_back(row);
        fill(found->second);
    }
    return found->second;
}

void SparseKeys::fill(std::uint32_t position) {
    for (auto const column : _needed) {
        _keys[column]->set(position, _snapshot->cell(_rows[position], column), _columns[column], *_services);
    }
}

void SparseKeys::grow(std::size_t capacity) {
    _capacity = capacity;
    _keys.clear();
    _keys.resize(_columns.size());
    _pointers.assign(_columns.size(), nullptr);
    for (auto const column : _needed) {
        _keys[column] = std::make_unique<KeyColumn>(KeyColumn::forColumn(_columns[column], *_services, capacity));
        _pointers[column] = _keys[column].get();
    }
    for (std::size_t at = 0; at < _rows.size(); ++at) {
        fill(static_cast<std::uint32_t>(at));
    }
}

std::expected<RetainedView, TableError> retainedView(ComputedView const& base, DataSource const& source,
                                                     RowSnapshot const& snapshot, TableQuery const& validated,
                                                     ApplyOptions const& options, SourceRowOf const& sourceRowOf) {
    std::vector<std::uint32_t> rows;
    if (sourceRowOf) {
        for (auto const& key : validated.retain) {
            if (auto const row = sourceRowOf(key)) {
                rows.push_back(*row);
            }
        }
    } else {
        std::unordered_set<RowId> const wanted(validated.retain.begin(), validated.retain.end());
        for (std::size_t row = 0; row < snapshot.rowCount() && rows.size() < wanted.size(); ++row) {
            if (wanted.contains(snapshot.rowId(row))) {
                rows.push_back(static_cast<std::uint32_t>(row));
            }
        }
    }
    auto const columns = source.columns();
    auto const services = options.services.forTask();
    auto filter = compileFilterGuarded(validated.filters, columns, services, source.groups());
    if (!filter) {
        return std::unexpected(std::move(filter.error()));
    }
    std::vector<std::pair<std::size_t, SortDirection>> sortKeys;
    std::vector<std::size_t> needed;
    for (auto const& key : validated.sort) {
        if (auto const column = findColumn(columns, key.column)) {
            sortKeys.emplace_back(*column, key.dir);
            needed.push_back(*column);
        }
    }
    for (auto const column : filter->columnsUsed()) {
        needed.push_back(column);
    }
    std::ranges::sort(needed);
    auto const [first, last] = std::ranges::unique(needed);
    needed.erase(first, last);
    SparseKeys keys{snapshot, columns, services, std::move(needed)};
    auto const before = [&keys, &sortKeys](std::uint32_t left, std::uint32_t right) {
        auto const leftAt = keys.local(left);
        auto const rightAt = keys.local(right);
        auto const pointers = keys.pointers();
        for (auto const& [column, dir] : sortKeys) {
            if (int const result = compareKeys(*pointers[column], leftAt, rightAt, dir); result != 0) {
                return result < 0;
            }
        }
        return left < right;
    };
    auto const match = [&keys, &filter](std::uint32_t row) {
        auto const local = keys.local(row);
        return filter->evaluateRow(keys.pointers(), local);
    };
    auto merged = mergeRetained(base.rows, static_cast<std::int64_t>(base.undecided.size()), base.digest, rows, before,
                                match, [&snapshot](std::uint32_t row) { return snapshot.rowId(row); });
    std::ranges::sort(rows);
    RetainedView out;
    out.view.rows = std::move(merged.order);
    // The retained rows that were undecided are in the view now.
    std::ranges::set_difference(base.undecided, rows, std::back_inserter(out.view.undecided));
    out.view.digest = merged.digest;
    out.unmatched = std::move(merged.unmatched);
    return out;
}

void markUnmatched(PageRows& page, std::span<std::int64_t const> unmatched) {
    page.unmatched.clear();
    for (std::size_t at = 0; at < page.rows.size(); ++at) {
        auto const viewRow = page.indices.empty() ? page.offset + static_cast<std::int64_t>(at) : page.indices[at];
        if (std::ranges::binary_search(unmatched, viewRow)) {
            page.unmatched.push_back(static_cast<std::int64_t>(at));
        }
    }
}

std::expected<PageRows, TableError> viewPage(ComputedView const& view, DataSource const& source,
                                             RowSnapshot const& snapshot, TableQuery const& validated,
                                             ApplyOptions const& options, std::span<std::uint32_t const> viewRowOf,
                                             SourceRowOf const& sourceRowOf) {
    if (validated.retain.empty()) {
        return pageRows(view, snapshot, validated, options.limits, viewRowOf, sourceRowOf);
    }
    auto merged = retainedView(view, source, snapshot, validated, options, sourceRowOf);
    if (!merged) {
        return std::unexpected(std::move(merged.error()));
    }
    auto out = pageRows(merged->view, snapshot, validated, options.limits, {}, sourceRowOf);
    markUnmatched(out, merged->unmatched);
    return out;
}

std::string isoDate(std::int64_t days) {
    auto const date = civilDate(days);
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()));
}

std::int64_t dayOfSeconds(std::int64_t seconds) {
    constexpr std::int64_t kDay = 86'400;
    return (seconds >= 0 ? seconds : seconds - (kDay - 1)) / kDay;
}

std::string isoDateTime(std::int64_t seconds) {
    constexpr std::int64_t kDay = 86'400;
    auto const day = dayOfSeconds(seconds);
    auto const secondOfDay = seconds - (day * kDay);
    return std::format("{}T{:02}:{:02}:{:02}Z", isoDate(day), secondOfDay / 3600, (secondOfDay / 60) % 60,
                       secondOfDay % 60);
}

std::pair<std::int64_t, std::int64_t> bucketDays(std::int64_t day, DateBucket bucket) {
    using namespace std::chrono;
    auto const date = civilDate(day);
    switch (bucket) {
        case DateBucket::Year:
            return {daysOf(date.year() / January / 1), daysOf(date.year() / December / 31)};
        case DateBucket::Month:
            return {daysOf(date.year() / date.month() / 1),
                    daysOf(year_month_day{year_month_day_last{date.year(), month_day_last{date.month()}}})};
        case DateBucket::Day:
        case DateBucket::Value:
        default:
            return {day, day};
    }
}

int compareValues(KeyColumn const& left, std::uint32_t leftRow, KeyColumn const& right, std::uint32_t rightRow) {
    auto const sign = [](auto ordering) -> int {
        if (std::is_lt(ordering)) {
            return -1;
        }
        return std::is_gt(ordering) ? 1 : 0;
    };
    switch (left.kind) {
        case ColumnKind::Integer:
        case ColumnKind::Key:
        case ColumnKind::Date:
        case ColumnKind::DateTime:
        case ColumnKind::Bool:
            return sign(left.ints[leftRow] <=> right.ints[rightRow]);
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
            return sign(left.exact[leftRow] <=> right.exact[rightRow]);
        case ColumnKind::Number:
            return sign(std::strong_order(left.reals[leftRow], right.reals[rightRow]));
        case ColumnKind::Text:
            return sign(left.text[leftRow].compare(right.text[rightRow]) <=> 0);
        case ColumnKind::Measure:
            if (left.places[leftRow] != right.places[rightRow]) {
                return left.places[leftRow] < right.places[rightRow] ? -1 : 1;
            }
            return sign(left.exact[leftRow] <=> right.exact[rightRow]);
        case ColumnKind::Custom:
        default:
            return 0;
    }
}

std::optional<Measure> measureOf(Cell const& cell, UnitRegistry const* units) {
    if (auto const* const value = std::get_if<Measure>(&cell)) {
        return *value;
    }
    if (auto const* const value = std::get_if<math::Rational>(&cell)) {
        return Measure{.amount = *value, .unit = 0};
    }
    if (auto const* const value = std::get_if<std::int64_t>(&cell)) {
        return Measure{.amount = math::Rational{*value, math::DecimalPlaces{0}}, .unit = 0};
    }
    if (auto const* const value = std::get_if<std::string>(&cell); value != nullptr && units != nullptr) {
        if (auto const operand = splitMeasureOperand(*value)) {
            if (auto const unit = units->find(operand->second)) {
                return Measure{.amount = operand->first, .unit = *unit};
            }
        }
    }
    return std::nullopt;
}

DistinctValue describeValue(RowSnapshot const& snapshot, std::size_t column, ColumnInfo const& info,
                            KeyColumn const& keys, std::uint32_t row, DateBucket bucket, UnitRegistry const* units) {
    auto const both = [](std::string const& text) { return DistinctValue{.text = text, .operand = text, .count = 0}; };
    if (info.kind == ColumnKind::Measure) {
        auto const measure = measureOf(snapshot.cell(row, column), units);
        if (!measure || units == nullptr || !units->contains(measure->unit)) {
            return both({});
        }
        auto const& unit = units->info(measure->unit);
        auto const amount = units::detail::formatRationalDecimal(measure->amount);
        return DistinctValue{.text = unit.symbol.empty() ? amount : amount + " " + unit.symbol,
                             .operand = unit.id.empty() ? amount : amount + " " + unit.id,
                             .count = 0,
                             .dimension = unit.dimension,
                             .unit = unit.id};
    }
    switch (info.kind) {
        case ColumnKind::Text:
            return both(keys.raw[row]);
        case ColumnKind::Integer:
        case ColumnKind::Key:
            return both(std::to_string(keys.ints[row]));
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
            return both(units::detail::formatRationalDecimal(keys.exact[row]));
        case ColumnKind::Number:
            return both(std::format("{}", keys.reals[row]));
        case ColumnKind::Bool:
            return both(keys.ints[row] != 0 ? "true" : "false");
        case ColumnKind::Date:
        case ColumnKind::DateTime:
            break;
        case ColumnKind::Custom:
        case ColumnKind::Measure:  // answered above
        default:
            return both({});
    }
    bool const dateTime = info.kind == ColumnKind::DateTime;
    if (bucket == DateBucket::Value) {
        if (auto const cell = snapshot.cell(row, column); auto const* const text = std::get_if<std::string>(&cell)) {
            return both(*text);
        }
        return both(dateTime ? isoDateTime(keys.ints[row]) : isoDate(keys.ints[row]));
    }
    auto const day = dateTime ? dayOfSeconds(keys.ints[row]) : keys.ints[row];
    auto const [first, last] = bucketDays(day, bucket);
    auto text = isoDate(first);
    if (bucket == DateBucket::Year) {
        text.resize(4);
    } else if (bucket == DateBucket::Month) {
        text.resize(7);
    }
    auto operand = dateTime ? isoDate(first) + "T00:00:00Z.." + isoDate(last) + "T23:59:59Z"
                            : isoDate(first) + ".." + isoDate(last);
    return DistinctValue{.text = std::move(text), .operand = std::move(operand), .count = 0};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
std::expected<DistinctValues, TableError> distinctOver(DistinctInput const& input, DistinctRequest const& validated) {
    try {
        auto const filter = compileFilter(input.filters, input.columns, input.services, input.groups);
        if (!filter) {
            return std::unexpected(filter.error());
        }
        auto const& snapshot = *input.snapshot;
        auto const rowCount = snapshot.rowCount();
        std::vector<std::size_t> listed;
        if (validated.group.empty()) {
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): validate() resolved every column the request names.
            listed.push_back(*findColumn(input.columns, validated.column));
        } else {
            // A local, not the range-for's own temporary: the optional holding the span must outlive the loop.
            auto const members = groupColumns(input.groups, validated.group);
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): validate() resolved every column the request names.
            for (auto const& columnId : *members) {
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): validate() resolved every column the request names.
                listed.push_back(*findColumn(input.columns, columnId));
            }
        }
        for (auto const column : listed) {
            if (auto missing = unitsMissing(input.columns[column], input.services)) {
                return std::unexpected(std::move(*missing));
            }
        }
        std::vector<std::shared_ptr<KeyColumn const>> built(input.columns.size());
        std::vector<KeyColumn const*> pointers(input.columns.size(), nullptr);
        auto const keysOf = [&](std::size_t column) {
            if (column < input.keys.size() && input.keys[column] != nullptr) {
                pointers[column] = input.keys[column].get();
                return;
            }
            auto keys =
                std::make_shared<KeyColumn>(KeyColumn::forColumn(input.columns[column], input.services, rowCount));
            buildKeys(snapshot, column, input.columns[column], input.services, 0, rowCount, *keys);
            pointers[column] = keys.get();
            built[column] = std::move(keys);
        };
        for (auto const column : filter->columnsUsed()) {
            keysOf(column);
        }
        for (auto const column : listed) {
            if (pointers[column] == nullptr) {
                keysOf(column);
            }
        }
        // As a view job does: an unloaded cell in a base-level column the list reads is a broken source, not a
        // list that may grow.
        for (std::size_t at = 0; at < pointers.size(); ++at) {
            if (pointers[at] != nullptr && input.columns[at].level.empty() &&
                std::ranges::contains(pointers[at]->states, CellState::Unloaded)) {
                return std::unexpected(unloadedInBaseLevel(input.columns[at]));
            }
        }
        std::vector<std::uint32_t> candidates(rowCount);
        for (std::size_t row = 0; row < rowCount; ++row) {
            candidates[row] = static_cast<std::uint32_t>(row);
        }
        DistinctValues out;
        if (!filter->empty()) {
            FilterRun run{*filter, pointers, std::move(candidates)};
            static_cast<void>(run.run(kNoDeadline, ::core::async::StopToken{}));
            out.partial = !std::move(run).takeUndecided().empty();
            // NOLINTNEXTLINE(bugprone-use-after-move): take and takeUndecided read different marks of the same rows and move nothing.
            candidates = std::move(run).take();
        }
        std::vector<KeyColumn const*> listedKeys;
        listedKeys.reserve(listed.size());
        for (auto const column : listed) {
            listedKeys.push_back(pointers[column]);
        }
        // Every listed column is of one kind: a declared group's are, by `checkGroups`.
        auto const& info = input.columns[listed.front()];
        std::vector<DistinctGroup> groups;
        switch (info.kind) {
            case ColumnKind::Text:
                groupRows<std::string_view>(
                    candidates, listedKeys,
                    [](KeyColumn const& keys, std::uint32_t row) { return std::string_view{keys.folded[row]}; },
                    groups, out);
                break;
            case ColumnKind::Decimal:
            case ColumnKind::Quantity:
                groupRows<math::Rational, RationalHash>(
                    candidates, listedKeys, [](KeyColumn const& keys, std::uint32_t row) { return keys.exact[row]; },
                    groups, out);
                break;
            case ColumnKind::Measure:
                groupRows<MeasureKey, MeasureKeyHash>(
                    candidates, listedKeys,
                    [](KeyColumn const& keys, std::uint32_t row) {
                        return MeasureKey{.place = keys.places[row], .amount = keys.exact[row]};
                    },
                    groups, out);
                break;
            case ColumnKind::Number:
                groupRows<double>(
                    candidates, listedKeys,
                    [](KeyColumn const& keys, std::uint32_t row) {
                        return keys.reals[row] == 0.0 ? 0.0 : keys.reals[row];
                    },
                    groups, out);
                break;
            case ColumnKind::Date:
            case ColumnKind::DateTime: {
                bool const dateTime = info.kind == ColumnKind::DateTime;
                auto const bucket = validated.bucket;
                groupRows<std::int64_t>(
                    candidates, listedKeys,
                    [dateTime, bucket](KeyColumn const& keys, std::uint32_t row) {
                        if (bucket == DateBucket::Value) {
                            return keys.ints[row];
                        }
                        return bucketDays(dateTime ? dayOfSeconds(keys.ints[row]) : keys.ints[row], bucket).first;
                    },
                    groups, out);
                break;
            }
            case ColumnKind::Integer:
            case ColumnKind::Key:
            case ColumnKind::Bool:
            case ColumnKind::Custom:
            default:
                groupRows<std::int64_t>(
                    candidates, listedKeys, [](KeyColumn const& keys, std::uint32_t row) { return keys.ints[row]; },
                    groups, out);
                break;
        }
        auto const describe = [&](DistinctGroup const& group) {
            auto const column = listed[group.column];
            return describeValue(snapshot, column, input.columns[column], *listedKeys[group.column], group.first,
                                 validated.bucket, input.services.units.get());
        };
        if (!validated.search.empty()) {
            auto const& collator = collatorOf(input.services);
            auto const needle = collator.fold(validated.search);
            std::erase_if(groups, [&](DistinctGroup const& group) {
                if (info.kind == ColumnKind::Text) {
                    return !std::string_view{listedKeys[group.column]->folded[group.first]}.contains(needle);
                }
                return !collator.fold(describe(group).text).contains(needle);
            });
        }
        auto const count = static_cast<std::int64_t>(groups.size());
        out.distinct = std::min(count, input.limits.totalCap);
        out.distinctKind = count > input.limits.totalCap ? TotalKind::AtLeast : TotalKind::Exact;
        auto const begin = std::min(validated.offset, count);
        auto const end = validated.limit >= count - begin ? count : begin + validated.limit;
        // Only the values up to the page's end are put in order: a menu's first page of 50,000 values sorts 200.
        std::ranges::partial_sort(groups, groups.begin() + static_cast<std::ptrdiff_t>(end),
                                  [&listedKeys](DistinctGroup const& left, DistinctGroup const& right) {
                                      int const order = compareValues(*listedKeys[left.column], left.first,
                                                                      *listedKeys[right.column], right.first);
                                      return order != 0 ? order < 0 : left.first < right.first;
                                  });
        for (auto at = begin; at < end; ++at) {
            auto const& group = groups[static_cast<std::size_t>(at)];
            auto value = describe(group);
            value.count = group.count;
            out.values.push_back(std::move(value));
        }
        return out;
    } catch (std::exception const& error) {
        return std::unexpected(serviceFailed(error.what()));
    } catch (...) {
        return std::unexpected(serviceFailed("unknown exception"));
    }
}

void withoutOwnFilter(FilterSpec& filters, DistinctRequest const& request) {
    if (request.group.empty()) {
        filters.columns.erase(request.column);
        return;
    }
    std::erase_if(filters.groups, [&request](GroupFilter const& group) { return group.group == request.group; });
}

}  // namespace morph::table::detail

namespace morph::table {

std::expected<DistinctValues, TableError> distinctValues(DataSource const& source, TableQuery const& query,
                                                         DistinctRequest const& request, ApplyOptions const& options) {
    if (auto error = detail::checkGroups(source.columns(), source.groups())) {
        return std::unexpected(std::move(*error));
    }
    TableQuery asked = query;
    asked.distinct = request;
    auto checked = validate(std::move(asked), source.columns(), options.limits, options.mayRead, source.groups());
    if (!checked) {
        return std::unexpected(std::move(checked.error()));
    }
    detail::DistinctInput input{.snapshot = source.snapshot(),
                                .columns = source.columns(),
                                .keys = {},
                                .filters = std::move(checked->filters),
                                .services = options.services.forTask(),
                                .limits = options.limits,
                                .groups = source.groups()};
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): validate() accepted the request as a distinct-values one.
    detail::withoutOwnFilter(input.filters, *checked->distinct);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): validate() accepted the request as a distinct-values one.
    return detail::distinctOver(input, *checked->distinct);
}

std::expected<PageRows, TableError> apply(DataSource const& source, TableQuery const& query,
                                          ApplyOptions const& options) {
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
        return PageRows{.rows = {}, .distinct = std::move(*values)};
    }
    auto const view = detail::computeView(source, *checked, options);
    if (!view) {
        return std::unexpected(view.error());
    }
    // A source is read on its owner's thread, which this call is on, so this is the snapshot the view was
    // computed over.
    return detail::viewPage(*view, source, *source.snapshot(), *checked, options);
}

std::vector<glz::raw_json> changeKeys(std::span<RowId const> keys) {
    std::vector<glz::raw_json> out;
    out.reserve(keys.size());
    for (auto const& key : keys) {
        if (auto const* const integer = std::get_if<std::int64_t>(&key)) {
            out.emplace_back(std::to_string(*integer));
        } else {
            out.emplace_back(glz::write_json(std::get<std::string>(key)).value_or(std::string{"\"\""}));
        }
    }
    return out;
}

std::optional<RowId> rowIdOf(glz::raw_json const& key) {
    constexpr std::string_view kSpace = " \t\n\r";
    std::string_view text = key.str;
    auto const first = text.find_first_not_of(kSpace);
    text = first == std::string_view::npos ? std::string_view{}
                                           : text.substr(first, text.find_last_not_of(kSpace) - first + 1);
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == '"') {
        std::string value;
        std::string const buffer{text};
        if (glz::read_json(value, buffer)) {
            return std::nullopt;
        }
        return RowId{std::move(value)};
    }
    std::int64_t value = 0;
    auto const* const end = std::to_address(text.end());
    auto const [rest, error] = std::from_chars(std::to_address(text.begin()), end, value);
    if (error != std::errc{} || rest != end) {
        return std::nullopt;
    }
    return RowId{value};
}

void MeasureValue::setWire(Wire const& wire) {
    std::uint32_t places = math::kMaxDecimalPlaces;
    std::int64_t power = 1;
    for (std::uint32_t k = 0; k <= math::kMaxDecimalPlaces; ++k) {
        if (power == wire.den) {
            places = k;
            break;
        }
        if (power > std::numeric_limits<std::int64_t>::max() / 10) {
            break;
        }
        power *= 10;
    }
    amount = wire.den > 0
                 ? math::Rational{math::Numerator{wire.num}, math::Denominator{wire.den}, math::DecimalPlaces{places}}
                 : math::Rational{};
    unit = wire.unit;
}

}  // namespace morph::table

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
