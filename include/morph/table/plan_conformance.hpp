// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/plan_conformance.hpp
/// @brief Holds a model's translation of a `QueryPlan` to the answers of `apply`.
///
/// A model that pages its rows in a database translates `planQuery`'s plan into its own query. This header runs a
/// corpus of queries through `apply` over the rows (in the model's key order) and through the model's callback, and
/// reports every disagreement. It depends on neither a test framework nor a database: the caller asserts that the
/// returned list is empty. See `docs/spec/table/database_pages.md`.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <morph/table/query.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::table {

/// @brief What a model's translation of a plan returns.
struct ModelPage {
    /// @brief The keys of the page's rows, in order.
    std::vector<RowId> rows;
    /// @brief The reported total; absent when the model did not count.
    std::optional<std::int64_t> total;
    /// @brief Whether `total` is the count or a lower bound.
    TotalKind totalKind = TotalKind::Exact;
    /// @brief The view row of `rows[0]`.
    std::int64_t offset = 0;
};

/// @brief A model's translation of a plan into its own query and the page that query returns.
using PlanModel = std::function<std::expected<ModelPage, TableError>(QueryPlan const&)>;

/// @brief The inputs the oracle and the plan share.
struct ConformanceOptions {
    /// @brief The bounds of `validate`.
    QueryLimits limits;
    /// @brief The services the oracle and the plan compile with.
    Services services;
    /// @brief Whether the principal may read a column; empty allows every column.
    std::function<bool(std::string_view)> mayRead;
    /// @brief The columns and uses the model can translate; empty accepts all. A query this refuses is skipped: the
    ///        refusal runs after `validate` and before the filter compiles, so only the refusal itself is the model's.
    std::function<bool(std::string_view, PlanUse)> translatable;
};

namespace detail {

/// @brief The operators a filter entry can carry, as the entries the corpus builds.
/// @param op      The operator.
/// @param first   The first operand.
/// @param second  The second operand, for `Between`.
/// @return The entry.
[[nodiscard]] inline FilterEntry corpusEntry(CompiledFilter::Op op, std::string const& first,
                                             std::string const& second) {
    using Op = CompiledFilter::Op;
    FilterEntry entry;
    switch (op) {
        case Op::Eq:
            entry.eq = first;
            break;
        case Op::Ne:
            entry.ne = first;
            break;
        case Op::Lt:
            entry.lt = first;
            break;
        case Op::Le:
            entry.le = first;
            break;
        case Op::Gt:
            entry.gt = first;
            break;
        case Op::Ge:
            entry.ge = first;
            break;
        case Op::Between:
            entry.between = std::array<std::string, 2>{first, second};
            break;
        case Op::Contains:
            entry.contains = first;
            break;
        case Op::StartsWith:
            entry.startsWith = first;
            break;
        case Op::EndsWith:
            entry.endsWith = first;
            break;
        case Op::IsEmpty:
            entry.isEmpty = true;
            break;
        case Op::NotEmpty:
        default:
            entry.notEmpty = true;
            break;
    }
    return entry;
}

/// @brief The text a corpus query uses as an operand for a cell.
///
/// A date-time cell holds whole UTC seconds and a date cell holds days since the epoch; shown as bare numbers neither
/// parses as a date, so a query built from them is refused by `apply` and by the plan alike and compares nothing.
/// They are written as ISO 8601 (`2026-11-10T09:00:00Z`, `2026-11-10`), which the default date parser reads and which
/// lands exactly on the second or day the cell holds. Any other cell is shown as `displayText` shows it.
/// @param kind The column's kind.
/// @param cell The cell.
/// @return The operand text.
[[nodiscard]] inline std::string operandText(ColumnKind kind, Cell const& cell) {
    // 0001-01-01T00:00:00Z and 9999-12-31T23:59:59Z: the years four digits can write.
    constexpr std::int64_t kMinSeconds = -62'135'596'800;
    constexpr std::int64_t kMaxSeconds = 253'402'300'799;
    constexpr std::int64_t kSecondsPerDay = 86'400;
    if (auto const* whole = std::get_if<std::int64_t>(&cell)) {
        if (kind == ColumnKind::DateTime && *whole >= kMinSeconds && *whole <= kMaxSeconds) {
            return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::sys_seconds{std::chrono::seconds{*whole}});
        }
        if (kind == ColumnKind::Date && *whole >= kMinSeconds / kSecondsPerDay &&
            *whole <= kMaxSeconds / kSecondsPerDay) {
            return std::format("{:%F}", std::chrono::sys_days{std::chrono::days{*whole}});
        }
    }
    return displayText(cell);
}

/// @brief A description of a total for a failure message.
/// @param total The total.
/// @param kind  Its kind.
/// @return The text.
[[nodiscard]] inline std::string describeTotal(std::optional<std::int64_t> const& total, TotalKind kind) {
    if (!total) {
        return "absent";
    }
    return std::format("{}{}", kind == TotalKind::AtLeast ? ">=" : "=", *total);
}

}  // namespace detail

/// @brief Queries derived from a source's own cells: every operator each column's kind defines (as an include, as an
///        exclude and, for text, case-sensitively), both sort directions on every column and on every pair of
///        adjacent columns, a quick-search group over the text columns, and a window of pages around the start, the
///        middle and past the end, with a zero limit.
///
/// The operands are the cells' own text, so the corpus is only as discriminating as the data: duplicate sort keys,
/// empty cells, accents and case, sub-second instants and mixed units come from the rows the caller supplies.
/// @param source The rows the model also holds.
/// @return The queries.
[[nodiscard]] inline std::vector<TableQuery> standardCorpus(DataSource const& source) {
    using Op = CompiledFilter::Op;
    constexpr std::array<Op, 12> kOps{Op::Eq,         Op::Ne,       Op::Lt,      Op::Le,
                                      Op::Gt,         Op::Ge,       Op::Between, Op::Contains,
                                      Op::StartsWith, Op::EndsWith, Op::IsEmpty, Op::NotEmpty};
    auto const columns = source.columns();
    auto const snapshot = source.snapshot();
    auto const rowCount = static_cast<std::int64_t>(snapshot->rowCount());
    std::vector<TableQuery> corpus;
    auto const pageOf = [&](TableQuery base, std::int64_t offset, std::int64_t limit) {
        base.page = PageRequest{.offset = offset, .limit = limit};
        corpus.push_back(std::move(base));
    };
    auto const windows = [&](TableQuery const& base) {
        pageOf(base, 0, 7);
        pageOf(base, rowCount / 2, 7);
        pageOf(base, rowCount + 10, 7);
        pageOf(base, 0, 0);
    };
    for (std::size_t c = 0; c < columns.size(); ++c) {
        // Up to three distinct non-empty texts of the column, in row order.
        std::vector<std::string> samples;
        for (std::size_t row = 0; row < snapshot->rowCount() && samples.size() < 3; ++row) {
            auto text = detail::operandText(columns[c].kind, snapshot->cell(row, c));
            if (!text.empty() && std::ranges::find(samples, text) == samples.end()) {
                samples.push_back(std::move(text));
            }
        }
        // A half step off a whole number, so a translation that rounds or integer-binds an operand on an integer
        // column answers differently from `apply`.
        if ((columns[c].kind == ColumnKind::Integer || columns[c].kind == ColumnKind::Key) && !samples.empty()) {
            samples.push_back(samples.front() + ".5");
        }
        for (auto const dir : {SortDirection::Ascending, SortDirection::Descending}) {
            TableQuery sorted;
            sorted.sort = {SortKey{.column = columns[c].id, .dir = dir}};
            windows(sorted);
            if (c + 1 < columns.size()) {
                sorted.sort.push_back(SortKey{.column = columns[c + 1].id, .dir = SortDirection::Descending});
                windows(sorted);
            }
        }
        for (auto const op : kOps) {
            if (!detail::operatorDefined(columns[c].kind, op)) {
                continue;
            }
            for (std::size_t s = 0; s < std::max<std::size_t>(samples.size(), 1); ++s) {
                std::string const first = samples.empty() ? std::string{} : samples[s];
                std::string const second = samples.size() > 1 ? samples[(s + 1) % samples.size()] : first;
                for (bool const caseSensitive : {false, true}) {
                    if (caseSensitive && columns[c].kind != ColumnKind::Text) {
                        continue;
                    }
                    auto entry = detail::corpusEntry(op, first, second);
                    if (caseSensitive) {
                        entry.caseSensitive = true;
                    }
                    for (bool const asExclude : {false, true}) {
                        TableQuery query;
                        ColumnFilter filter;
                        (asExclude ? filter.exclude : filter.include).push_back(entry);
                        query.filters.columns.emplace(columns[c].id, std::move(filter));
                        query.sort = {SortKey{.column = columns[c].id, .dir = SortDirection::Descending}};
                        windows(query);
                    }
                }
            }
        }
    }
    GroupFilter group;
    for (auto const& column : columns) {
        if (column.kind == ColumnKind::Text) {
            group.columns.push_back(column.id);
        }
    }
    if (!group.columns.empty()) {
        for (auto const& needle : {std::string{"a"}, std::string{"E"}}) {
            TableQuery query;
            GroupFilter search = group;
            search.include.push_back(FilterEntry{.contains = needle});
            query.filters.groups.push_back(std::move(search));
            windows(query);
        }
    }
    return corpus;
}

/// @brief Runs @p corpus through `apply` over @p source and through @p model, and describes every disagreement and
/// every
///        column the corpus never compared.
///
/// @p source must hold the rows in the model's key order, which is the order `apply` breaks ties in. For each
/// query: when the plan is refused for a reason other than the model's `translatable`, `apply` must refuse with the
/// same code and column; when the plan is accepted, the model's rows (by key), offset and total must equal
/// `apply`'s. Above `QueryLimits::totalCap` `apply` omits the total; the oracle then counts the view itself, and
/// the model must report either that exact count or a lower bound equal to the cap.
///
/// Identical refusals count as agreement, so a corpus whose operands never parse would pass without comparing
/// anything. For each column that a corpus query filters with an operand, and for each column that a sort-only query
/// orders, the helper therefore also reports the column when at least one query on it was refused identically by
/// `apply` and the plan and none was compared with the model. Queries the model's `translatable` refuses are skipped
/// and count as neither, so a column the model declares untranslatable is not reported.
/// @param source  The rows, in key order.
/// @param model   The model's translation of a plan.
/// @param corpus  The queries.
/// @param options Limits, services and column access shared by the oracle and the plan.
/// @return One description per disagreement; empty when the model conforms.
[[nodiscard]] inline std::vector<std::string> conformanceFailures(DataSource const& source, PlanModel const& model,
                                                                  std::span<TableQuery const> corpus,
                                                                  ConformanceOptions const& options = {}) {
    std::vector<std::string> failures;
    auto const snapshot = source.snapshot();
    ApplyOptions applyOptions;
    applyOptions.limits = options.limits;
    applyOptions.services = options.services;
    applyOptions.mayRead = options.mayRead;
    // Per "filter <column>" and "sort <column>": queries compared with the model, and queries both sides refused.
    struct Tally {
        std::size_t compared = 0;
        std::size_t refusedBySame = 0;
    };
    std::map<std::string, Tally> tallies;
    auto const tallied = [&](TableQuery const& query) {
        std::vector<Tally*> out;
        if (!query.filters.columns.empty()) {
            for (auto const& [column, filter] : query.filters.columns) {
                // IsEmpty and NotEmpty take no operand, so they are compared whatever the operands do.
                auto const hasOperand = [](FilterEntry const& entry) {
                    return !entry.isEmpty.value_or(false) && !entry.notEmpty.value_or(false);
                };
                if (std::ranges::any_of(filter.include, hasOperand) ||
                    std::ranges::any_of(filter.exclude, hasOperand)) {
                    out.push_back(&tallies["filter " + column]);
                }
            }
        } else {
            for (auto const& key : query.sort) {
                out.push_back(&tallies["sort " + key.column]);
            }
        }
        return out;
    };
    for (std::size_t index = 0; index < corpus.size(); ++index) {
        auto const& query = corpus[index];
        auto const name = std::format("query {}", index);
        auto const planned = planQuery(query, source.columns(), options.limits, options.mayRead, options.services,
                                       options.translatable);
        auto const expected = apply(source, query, applyOptions);
        if (!planned) {
            if (planned.error().code == TableErrorCode::Unavailable) {
                continue;
            }
            if (expected || expected.error().code != planned.error().code ||
                expected.error().column != planned.error().column) {
                failures.push_back(name + ": the plan was refused (" + planned.error().message +
                                   ") but apply answered differently");
            } else {
                for (auto* tally : tallied(query)) {
                    ++tally->refusedBySame;
                }
            }
            continue;
        }
        if (!expected) {
            failures.push_back(name + ": apply refused (" + expected.error().message + ") but the plan was accepted");
            continue;
        }
        auto const got = model(*planned);
        if (!got) {
            failures.push_back(name + ": the model refused the plan: " + got.error().message);
            continue;
        }
        for (auto* tally : tallied(query)) {
            ++tally->compared;
        }
        std::vector<RowId> wanted;
        wanted.reserve(expected->rows.size());
        for (auto const row : expected->rows) {
            wanted.push_back(snapshot->rowId(row));
        }
        if (got->rows != wanted) {
            failures.push_back(
                std::format("{}: rows differ (apply {}, model {})", name, wanted.size(), got->rows.size()));
        }
        if (got->offset != expected->offset) {
            failures.push_back(std::format("{}: offset {} but apply {}", name, got->offset, expected->offset));
        }
        if (expected->total) {
            if (got->total != expected->total || got->totalKind != TotalKind::Exact) {
                failures.push_back(name + ": total " + detail::describeTotal(got->total, got->totalKind) +
                                   " but apply " + detail::describeTotal(expected->total, TotalKind::Exact));
            }
        } else {
            // Above the cap `apply` omits the total, so the oracle counts the filtered view itself: an Exact total
            // must equal that count, and a lower bound must equal the cap.
            auto const cap = options.limits.totalCap;
            auto counting = query;
            counting.page = PageRequest{.offset = 0, .limit = 0};
            auto countOptions = applyOptions;
            countOptions.limits.totalCap = std::numeric_limits<std::int64_t>::max();
            auto const counted = apply(source, counting, countOptions);
            if (!counted || !counted->total) {
                failures.push_back(name + ": the oracle could not count the view");
                continue;
            }
            bool const exactCount = got->total == counted->total && got->totalKind == TotalKind::Exact;
            bool const boundAtCap = got->total && got->totalKind == TotalKind::AtLeast && *got->total == cap;
            if (!exactCount && !boundAtCap) {
                failures.push_back(name + ": above the cap the model reported " +
                                   detail::describeTotal(got->total, got->totalKind) + " for a view of " +
                                   std::to_string(*counted->total));
            }
        }
    }
    for (auto const& [use, tally] : tallies) {
        if (tally.refusedBySame > 0 && tally.compared == 0) {
            failures.push_back(std::format(
                "{}: {} queries were refused identically by apply and the plan and none was compared with the model "
                "(an operand the source's own cells cannot supply?)",
                use, tally.refusedBySame));
        }
    }
    return failures;
}

}  // namespace morph::table
