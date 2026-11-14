// SPDX-License-Identifier: Apache-2.0
//
// morph::table database pages: planQuery, TotalKind, and the conformance helper (docs/spec/table/database_pages.md).
//
// Mutations these tests were seen to fail on (each applied to the library, not to the reference model):
//  - planQuery projecting exclude entries into include: "a model that follows the plan answers as apply does" fails.
//  - planQuery dropping caseSensitive from a condition: the same case fails.
//  - planQuery swapping Between's operands: the same case fails.
//  - planQuery taking the sort direction from the wrong field: the same case fails.
//  - planQuery skipping validate: "a plan is refused as apply refuses" fails.
//  - compileFilterGuarded without its catch: "a throwing collator is a ServiceFailed error" fails.
//  - PageWindow stopping at an AtLeast total: "a lower-bound total does not end the window" fails.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/table/plan_conformance.hpp>
#include <morph/table/query.hpp>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using morph::math::DecimalPlaces;
using morph::math::Rational;
using tabletest::col;
using tabletest::dec;

namespace {

// The columns of the corpus table, one of every kind a plan carries an operand for, except Quantity and Custom.
std::vector<ColumnInfo> pageColumns() {
    return {col("id", ColumnKind::Key),    col("n", ColumnKind::Integer),   col("price", ColumnKind::Decimal),
            col("name", ColumnKind::Text), col("on", ColumnKind::Date),     col("at", ColumnKind::DateTime),
            col("flag", ColumnKind::Bool), col("ratio", ColumnKind::Number)};
}

// Rows with duplicate sort keys in every column, empty cells in every column, and text that differs by case.
std::vector<std::vector<Cell>> pageRows(std::size_t count) {
    static constexpr std::array<std::string_view, 7> kNames{"alpha", "Alpha", "beta", "Beta",
                                                            "gamma", "ALPHA", "delta"};
    std::vector<std::vector<Cell>> rows;
    for (std::size_t i = 0; i < count; ++i) {
        auto const n = static_cast<std::int64_t>(i);
        rows.push_back({
            Cell{n},
            i % 9 == 4 ? Cell{std::monostate{}} : Cell{n % 6},
            i % 8 == 3 ? Cell{std::monostate{}} : Cell{dec(std::to_string(n % 5) + "." + std::to_string(n % 4) + "5")},
            i % 7 == 2 ? Cell{std::monostate{}} : Cell{std::string{kNames[i % kNames.size()]}},
            i % 11 == 5 ? Cell{std::monostate{}} : Cell{19'000 + (n % 4)},
            i % 10 == 6 ? Cell{std::monostate{}} : Cell{1'700'000'000 + (n % 3)},
            i % 13 == 7 ? Cell{std::monostate{}} : Cell{n % 2 == 0},
            i % 12 == 1 ? Cell{std::monostate{}} : Cell{static_cast<double>(n % 5) / 2.0},
        });
    }
    return rows;
}

// The corpus table keyed 0..n-1, in key order.
std::shared_ptr<VectorSource> pageSource(std::size_t count) {
    return tabletest::makeSource(pageColumns(), pageRows(count));
}

// A value as the reference model holds it: empty, exact, floating point, integer, or text.
using Value = std::variant<std::monostate, Rational, double, std::int64_t, std::string>;

Value valueOf(Cell const& cell, ColumnKind kind) {
    if (std::holds_alternative<std::monostate>(cell)) {
        return {};
    }
    if (kind == ColumnKind::Integer || kind == ColumnKind::Key || kind == ColumnKind::Decimal) {
        if (auto const* whole = std::get_if<std::int64_t>(&cell)) {
            return Rational{*whole, DecimalPlaces{0}};
        }
        return std::get<Rational>(cell);
    }
    if (kind == ColumnKind::Number) {
        return std::get<double>(cell);
    }
    if (kind == ColumnKind::Date || kind == ColumnKind::DateTime) {
        return std::get<std::int64_t>(cell);
    }
    if (kind == ColumnKind::Bool) {
        return static_cast<std::int64_t>(std::get<bool>(cell) ? 1 : 0);
    }
    return std::get<std::string>(cell);
}

// Three-way compare of a cell's value with an operand of the same type; text compares the strings given.
template <typename T>
int three(T const& left, T const& right) {
    return left < right ? -1 : (right < left ? 1 : 0);
}

int sign(std::strong_ordering order) { return std::is_lt(order) ? -1 : (std::is_gt(order) ? 1 : 0); }

int compareValue(Value const& cell, PlannedOperand const& operand) {
    if (auto const* exact = std::get_if<Rational>(&cell)) {
        return sign(*exact <=> std::get<Rational>(operand));
    }
    if (auto const* real = std::get_if<double>(&cell)) {
        return three(*real, std::get<double>(operand));
    }
    if (auto const* whole = std::get_if<std::int64_t>(&cell)) {
        return three(*whole, std::get<std::int64_t>(operand));
    }
    return three(std::get<std::string>(cell), std::get<std::string>(operand));
}

// A reference store: a plan walker written from the plan alone. Text is folded with the default collator, which
// is what a stored fold column written through that collator holds.
class ReferenceModel {
public:
    ReferenceModel(std::vector<ColumnInfo> columns, std::vector<std::vector<Cell>> rows)
        : _columns{std::move(columns)}, _rows{std::move(rows)} {}

    std::expected<ModelPage, TableError> run(QueryPlan const& plan) const {
        std::vector<std::size_t> hits;
        for (std::size_t row = 0; row < _rows.size(); ++row) {
            if (accepts(plan, row)) {
                hits.push_back(row);
            }
        }
        // The order is the sort chain, then the row key, which is the row's index here.
        std::ranges::stable_sort(hits, [&](std::size_t left, std::size_t right) {
            for (auto const& key : plan.sort) {
                int const order = compareCells(key, left, right);
                if (order != 0) {
                    return order < 0;
                }
            }
            return false;
        });
        ModelPage page;
        page.offset = plan.offset;
        auto const count = static_cast<std::int64_t>(hits.size());
        page.total = std::min(count, plan.totalCap);
        page.totalKind = count > plan.totalCap ? TotalKind::AtLeast : TotalKind::Exact;
        auto const begin = std::min(plan.offset, count);
        auto const end = plan.limit >= count - begin ? count : begin + plan.limit;
        for (auto at = begin; at < end; ++at) {
            page.rows.emplace_back(static_cast<std::int64_t>(hits[static_cast<std::size_t>(at)]));
        }
        return page;
    }

private:
    std::size_t index(std::string const& column) const {
        return static_cast<std::size_t>(std::ranges::find(_columns, column, &ColumnInfo::id) - _columns.begin());
    }

    // Empty cells sort after valid ones in either direction.
    int compareCells(PlannedSortKey const& key, std::size_t left, std::size_t right) const {
        auto const c = index(key.column);
        auto const a = valueOf(_rows[left][c], key.kind);
        auto const b = valueOf(_rows[right][c], key.kind);
        bool const aEmpty = std::holds_alternative<std::monostate>(a);
        bool const bEmpty = std::holds_alternative<std::monostate>(b);
        if (aEmpty || bEmpty) {
            return aEmpty == bEmpty ? 0 : (aEmpty ? 1 : -1);
        }
        int order = 0;
        if (key.kind == ColumnKind::Text) {
            order = three(foldOf(std::get<std::string>(a)), foldOf(std::get<std::string>(b)));
        } else if (auto const* exact = std::get_if<Rational>(&a)) {
            order = sign(*exact <=> std::get<Rational>(b));
        } else if (auto const* real = std::get_if<double>(&a)) {
            order = three(*real, std::get<double>(b));
        } else {
            order = three(std::get<std::int64_t>(a), std::get<std::int64_t>(b));
        }
        return key.dir == SortDirection::Ascending ? order : -order;
    }

    static std::string foldOf(std::string const& text) { return CodePointCollator{}.fold(text); }

    bool matches(PlannedCondition const& condition, std::size_t row) const {
        auto const value = valueOf(_rows[row][index(condition.column)], condition.kind);
        bool const empty = std::holds_alternative<std::monostate>(value);
        if (condition.op == FilterOp::IsEmpty) {
            return empty;
        }
        if (condition.op == FilterOp::NotEmpty) {
            return !empty;
        }
        if (empty) {
            return false;
        }
        if (condition.kind == ColumnKind::Text) {
            auto const& cell = std::get<std::string>(value);
            auto const text = condition.caseSensitive ? cell : foldOf(cell);
            auto const& operand = std::get<std::string>(condition.value);
            auto const op = condition.op;
            if (op == FilterOp::Eq) {
                return text == operand;
            }
            if (op == FilterOp::Ne) {
                return text != operand;
            }
            if (op == FilterOp::Contains) {
                return text.find(operand) != std::string::npos;
            }
            if (op == FilterOp::StartsWith) {
                return text.starts_with(operand);
            }
            return op == FilterOp::EndsWith && text.ends_with(operand);
        }
        int const order = compareValue(value, condition.value);
        auto const op = condition.op;
        if (op == FilterOp::Between) {
            return order >= 0 && compareValue(value, condition.upper) <= 0;
        }
        if (op == FilterOp::Eq) {
            return order == 0;
        }
        if (op == FilterOp::Ne) {
            return order != 0;
        }
        if (op == FilterOp::Lt) {
            return order < 0;
        }
        if (op == FilterOp::Le) {
            return order <= 0;
        }
        if (op == FilterOp::Gt) {
            return order > 0;
        }
        return op == FilterOp::Ge && order >= 0;
    }

    // Include entries combine; an exclude entry rejects a row only when the cell is present and meets it.
    bool passes(PlannedColumnFilter const& filter, std::size_t row) const {
        if (!filter.include.empty()) {
            bool const all = filter.combine == Combine::All;
            bool result = all;
            for (auto const& condition : filter.include) {
                if (matches(condition, row) != all) {
                    result = !all;
                    break;
                }
            }
            if (!result) {
                return false;
            }
        }
        return std::ranges::none_of(filter.exclude, [&](PlannedCondition const& c) { return matches(c, row); });
    }

    bool accepts(QueryPlan const& plan, std::size_t row) const {
        return std::ranges::all_of(plan.filters, [&](auto const& filter) { return passes(filter, row); }) &&
               std::ranges::all_of(plan.groups, [&](PlannedGroup const& group) {
                   return std::ranges::any_of(group.columns, [&](auto const& filter) { return passes(filter, row); });
               });
    }

    std::vector<ColumnInfo> _columns;
    std::vector<std::vector<Cell>> _rows;
};

ConformanceOptions smallCap() {
    ConformanceOptions options;
    options.limits.totalCap = 20;
    return options;
}

}  // namespace

TEST_CASE("table: a model that follows the plan answers as apply does", "[table][database_pages]") {
    auto const source = pageSource(60);
    ReferenceModel const model{pageColumns(), pageRows(60)};
    auto const corpus = standardCorpus(*source);
    REQUIRE(corpus.size() > 500);
    auto const failures =
        conformanceFailures(*source, [&](QueryPlan const& plan) { return model.run(plan); }, corpus, smallCap());
    INFO("first failure: " << (failures.empty() ? std::string{} : failures.front()));
    CHECK(failures.empty());
}

TEST_CASE("table: the conformance helper reports a model that disagrees with apply", "[table][database_pages]") {
    auto const source = pageSource(60);
    ReferenceModel const model{pageColumns(), pageRows(60)};
    auto const corpus = standardCorpus(*source);
    auto const ascendingOnly = [&](QueryPlan plan) {
        for (auto& key : plan.sort) {
            key.dir = SortDirection::Ascending;
        }
        return model.run(plan);
    };
    CHECK_FALSE(conformanceFailures(*source, ascendingOnly, corpus, smallCap()).empty());
    auto const noExcludes = [&](QueryPlan plan) {
        for (auto& filter : plan.filters) {
            filter.exclude.clear();
        }
        return model.run(plan);
    };
    CHECK_FALSE(conformanceFailures(*source, noExcludes, corpus, smallCap()).empty());
    auto const wrongTotal = [&](QueryPlan const& plan) {
        auto page = model.run(plan);
        if (page && page->total) {
            page->total = *page->total + 1;
        }
        return page;
    };
    CHECK_FALSE(conformanceFailures(*source, wrongTotal, corpus, smallCap()).empty());
    // Above the cap the exact count is also an answer, and an absent total is not.
    auto const exactAboveCap = [&](QueryPlan plan) {
        plan.totalCap = 1'000'000;
        return model.run(plan);
    };
    CHECK(conformanceFailures(*source, exactAboveCap, corpus, smallCap()).empty());
    auto const uncounted = [&](QueryPlan const& plan) {
        auto page = model.run(plan);
        if (page) {
            page->total.reset();
        }
        return page;
    };
    CHECK_FALSE(conformanceFailures(*source, uncounted, corpus, smallCap()).empty());
}

namespace {

TableQuery withFilter(std::string column, FilterEntry entry) {
    TableQuery query;
    query.filters.columns.emplace(std::move(column),
                                  ColumnFilter{.combine = Combine::Any, .include = {std::move(entry)}, .exclude = {}});
    return query;
}

}  // namespace

TEST_CASE("table: a plan is refused as apply refuses", "[table][database_pages]") {
    auto const source = pageSource(10);
    ApplyOptions options;
    options.mayRead = [](std::string_view column) { return column != "ratio"; };
    options.limits.maxSortKeys = 2;
    options.limits.maxFilterColumns = 1;
    options.limits.maxTextLength = 4;

    std::vector<TableQuery> bad;
    bad.push_back([] {
        TableQuery q;
        q.page.offset = -1;
        return q;
    }());
    bad.push_back([] {
        TableQuery q;
        q.page.offset = 10'000'001;
        return q;
    }());
    bad.push_back([] {
        TableQuery q;
        q.page.limit = -1;
        return q;
    }());
    bad.push_back([] {
        TableQuery q;
        q.sort = {SortKey{.column = "n"}, SortKey{.column = "id"}, SortKey{.column = "name"}};
        return q;
    }());
    bad.push_back([] {
        TableQuery q;
        q.sort = {SortKey{.column = "nope"}};
        return q;
    }());
    bad.push_back([] {
        TableQuery q;
        q.sort = {SortKey{.column = "ratio"}};
        return q;
    }());
    bad.push_back(withFilter("n", FilterEntry{.contains = "1"}));                  // operator not defined for Integer
    bad.push_back(withFilter("n", FilterEntry{.eq = "not a number"}));             // operand does not parse
    bad.push_back(withFilter("n", FilterEntry{.eq = "1", .ne = "2"}));             // two operators
    bad.push_back(withFilter("name", FilterEntry{.eq = "far too long a value"}));  // over-long operand
    bad.push_back(withFilter("ratio", FilterEntry{.eq = "1"}));                    // hidden column
    bad.push_back([] {
        TableQuery q = withFilter("n", FilterEntry{.eq = "1"});
        q.filters.columns.emplace(
            "id", ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.eq = "1"}}, .exclude = {}});
        return q;
    }());  // too many filtered columns
    bad.push_back([] {
        TableQuery q;
        q.filters.groups.push_back(
            GroupFilter{.columns = {"n"}, .combine = Combine::Any, .include = {FilterEntry{.contains = "x"}}});
        return q;
    }());  // a group entry that applies nowhere

    for (auto const& query : bad) {
        auto const expected = apply(*source, query, options);
        auto const planned = planQuery(query, source->columns(), options.limits, options.mayRead);
        REQUIRE_FALSE(expected.has_value());
        REQUIRE_FALSE(planned.has_value());
        CHECK(planned.error().code == expected.error().code);
        CHECK(planned.error().column == expected.error().column);
    }
}

TEST_CASE("table: a throwing collator is a ServiceFailed error from planQuery", "[table][database_pages]") {
    struct Throwing final : TextCollator {
        [[nodiscard]] std::string sortKey(std::string_view text) const override { return std::string{text}; }
        [[nodiscard]] std::string fold(std::string_view) const override { throw std::runtime_error{"down"}; }
    };
    auto const source = pageSource(10);
    Services services;
    services.collator = std::make_shared<Throwing const>();
    auto const planned =
        planQuery(withFilter("name", FilterEntry{.contains = "a"}), source->columns(), {}, {}, services);
    REQUIRE_FALSE(planned.has_value());
    CHECK(planned.error().code == TableErrorCode::ServiceFailed);
}

TEST_CASE("table: a model refuses what it cannot translate with Unavailable", "[table][database_pages]") {
    auto const source = pageSource(10);
    auto const translatable = [](std::string_view column, PlanUse use) {
        return !(column == "name" && use == PlanUse::Sort) && !(column == "ratio" && use == PlanUse::Filter);
    };
    TableQuery sorted;
    sorted.sort = {SortKey{.column = "name"}};
    auto const sort = planQuery(sorted, source->columns(), {}, {}, {}, translatable);
    REQUIRE_FALSE(sort.has_value());
    CHECK(sort.error().code == TableErrorCode::Unavailable);
    CHECK(sort.error().column == "name");
    auto const filter =
        planQuery(withFilter("ratio", FilterEntry{.gt = "1"}), source->columns(), {}, {}, {}, translatable);
    REQUIRE_FALSE(filter.has_value());
    CHECK(filter.error().code == TableErrorCode::Unavailable);
    TableQuery group;
    group.filters.groups.push_back(
        GroupFilter{.columns = {"name", "ratio"}, .combine = Combine::Any, .include = {FilterEntry{.contains = "a"}}});
    CHECK_FALSE(planQuery(group, source->columns(), {}, {}, {}, translatable).has_value());
    // What the predicate accepts is planned, and a refused column that is only sorted on is a filter use of none.
    TableQuery fine = withFilter("name", FilterEntry{.contains = "a"});
    CHECK(planQuery(fine, source->columns(), {}, {}, {}, translatable).has_value());
    // Validation comes first: an unknown column is the engine's refusal, not Unavailable.
    TableQuery unknown;
    unknown.sort = {SortKey{.column = "missing"}};
    CHECK(planQuery(unknown, source->columns(), {}, {}, {}, translatable).error().code ==
          TableErrorCode::UnknownColumn);
}

TEST_CASE("table: a plan carries the clamped page, the cap and operands typed by their column",
          "[table][database_pages]") {
    auto const source = pageSource(10);
    TableQuery query;
    query.page = PageRequest{.offset = 3, .limit = 5'000};
    query.sort = {SortKey{.column = "price", .dir = SortDirection::Descending}};
    ColumnFilter name;
    name.include.push_back(FilterEntry{.contains = "AlPh"});
    name.include.push_back(FilterEntry{.eq = "Beta", .caseSensitive = true});
    query.filters.columns.emplace("name", name);
    ColumnFilter at;
    at.include.push_back(
        FilterEntry{.between = std::array<std::string, 2>{"2023-11-14T22:13:20Z", "2023-11-15T00:00:00Z"}});
    query.filters.columns.emplace("at", at);
    ColumnFilter n;
    n.exclude.push_back(FilterEntry{.lt = "1.5"});
    n.include.push_back(FilterEntry{.notEmpty = true});
    query.filters.columns.emplace("n", n);
    QueryLimits limits;
    limits.totalCap = 77;

    auto const plan = planQuery(query, source->columns(), limits);
    REQUIRE(plan.has_value());
    CHECK(plan->offset == 3);
    CHECK(plan->limit == limits.maxLimit);
    CHECK(plan->totalCap == 77);
    REQUIRE(plan->sort.size() == 1);
    CHECK(plan->sort[0].column == "price");
    CHECK(plan->sort[0].kind == ColumnKind::Decimal);
    CHECK(plan->sort[0].dir == SortDirection::Descending);
    REQUIRE(plan->filters.size() == 3);
    auto const find = [&](std::string_view column) -> PlannedColumnFilter const& {
        return *std::ranges::find(plan->filters, column, &PlannedColumnFilter::column);
    };
    auto const& names = find("name");
    REQUIRE(names.include.size() == 2);
    CHECK(std::get<std::string>(names.include[0].value) == "alph");  // folded
    CHECK(names.include[0].text == "AlPh");                          // as given
    CHECK_FALSE(names.include[0].caseSensitive);
    CHECK(std::get<std::string>(names.include[1].value) == "Beta");  // raw, because case-sensitive
    CHECK(names.include[1].caseSensitive);
    auto const& stamps = find("at").include.at(0);
    CHECK(stamps.op == FilterOp::Between);
    CHECK(std::get<std::int64_t>(stamps.value) == 1'700'000'000);
    CHECK(std::get<std::int64_t>(stamps.upper) == 1'700'006'400);
    CHECK(stamps.upperText == "2023-11-15T00:00:00Z");
    auto const& ints = find("n");
    REQUIRE(ints.exclude.size() == 1);
    CHECK(std::get<Rational>(ints.exclude[0].value) == dec("1.5"));  // not rounded
    CHECK(ints.include.at(0).op == FilterOp::NotEmpty);
    CHECK(std::holds_alternative<std::monostate>(ints.include[0].value));
}

TEST_CASE("table: a total is exact or a lower bound, and a lower bound does not end the window",
          "[table][database_pages]") {
    Page<int> page{.rows = {1, 2}, .total = 100, .offset = 0, .totalKind = TotalKind::AtLeast};
    auto const json = glz::write_json(page);
    REQUIRE(json.has_value());
    Page<int> back;
    REQUIRE_FALSE(glz::read_json(back, *json));
    CHECK(back.totalKind == TotalKind::AtLeast);
    CHECK(back.total == 100);
    Page<int> without;
    REQUIRE_FALSE(glz::read_json(without, R"({"rows":[1],"total":1,"offset":0})"));
    CHECK(without.totalKind == TotalKind::Exact);

    PageWindow<int> bounded{10, 8};
    auto const first = bounded.visible(0, 5);
    REQUIRE_FALSE(first.empty());
    CHECK(bounded.accept(
        first[0],
        Page<int>{.rows = std::vector<int>(10, 1), .total = 10, .offset = 0, .totalKind = TotalKind::AtLeast}));
    CHECK(bounded.totalKind() == TotalKind::AtLeast);
    // Rows 10.. are fetched although the bound is 10: a lower bound says there are at least that many.
    auto const next = bounded.visible(10, 5);
    CHECK_FALSE(next.empty());
    bounded.reset();
    CHECK(bounded.totalKind() == TotalKind::Exact);

    PageWindow<int> exact{10, 8};
    auto const start = exact.visible(0, 5);
    REQUIRE_FALSE(start.empty());
    CHECK(exact.accept(start[0], Page<int>{.rows = std::vector<int>(10, 1), .total = 10, .offset = 0}));
    CHECK(exact.visible(10, 5).empty());
}

TEST_CASE("table: the standard corpus filters every column kind with operands that parse", "[table][database_pages]") {
    auto const source = pageSource(60);
    auto const corpus = standardCorpus(*source);
    std::map<ColumnKind, std::size_t> accepted;
    for (auto const& query : corpus) {
        if (!query.filters.columns.empty() && planQuery(query, source->columns()).has_value()) {
            ++accepted[std::ranges::find(source->columns(), query.filters.columns.begin()->first, &ColumnInfo::id)
                           ->kind];
        }
    }
    for (auto const kind : {ColumnKind::Key, ColumnKind::Integer, ColumnKind::Decimal, ColumnKind::Text,
                            ColumnKind::Date, ColumnKind::DateTime, ColumnKind::Bool, ColumnKind::Number}) {
        INFO("kind " << static_cast<int>(kind));
        CHECK(accepted[kind] > 20);
    }
}

TEST_CASE("table: an exact total above the cap must equal the true count", "[table][database_pages]") {
    auto const source = pageSource(60);
    ReferenceModel const model{pageColumns(), pageRows(60)};
    auto const corpus = standardCorpus(*source);
    // Exact above the cap, with the true count: accepted.
    auto const exactCount = [&](QueryPlan plan) {
        plan.totalCap = 1'000'000;
        return model.run(plan);
    };
    CHECK(conformanceFailures(*source, exactCount, corpus, smallCap()).empty());
    // Exact above the cap, but cap + 1 or an off-by-one count: reported.
    auto const capPlusOne = [&](QueryPlan const& plan) {
        auto page = model.run(plan);
        if (page && page->totalKind == TotalKind::AtLeast) {
            page->totalKind = TotalKind::Exact;
            page->total = plan.totalCap + 1;
        }
        return page;
    };
    CHECK_FALSE(conformanceFailures(*source, capPlusOne, corpus, smallCap()).empty());
    auto const offByOne = [&](QueryPlan plan) {
        plan.totalCap = 1'000'000;
        auto page = model.run(plan);
        if (page && page->total && *page->total > 20) {
            page->total = *page->total - 1;
        }
        return page;
    };
    CHECK_FALSE(conformanceFailures(*source, offByOne, corpus, smallCap()).empty());
}

TEST_CASE("table: the corpus holds a translation to non-integral operands on integer columns",
          "[table][database_pages]") {
    auto const source = pageSource(60);
    ReferenceModel const model{pageColumns(), pageRows(60)};
    auto const corpus = standardCorpus(*source);
    // A translation that truncates the operand to a whole number, as an integer-bound parameter would.
    auto const truncating = [&](QueryPlan plan) {
        auto truncate = [](PlannedCondition& condition) {
            if (condition.kind != ColumnKind::Integer && condition.kind != ColumnKind::Key) {
                return;
            }
            for (auto* operand : {&condition.value, &condition.upper}) {
                if (auto* exact = std::get_if<Rational>(operand)) {
                    *exact = Rational{exact->numerator / exact->denominator, DecimalPlaces{0}};
                }
            }
        };
        for (auto& filter : plan.filters) {
            std::ranges::for_each(filter.include, truncate);
            std::ranges::for_each(filter.exclude, truncate);
        }
        return model.run(plan);
    };
    CHECK_FALSE(conformanceFailures(*source, truncating, corpus, smallCap()).empty());
    CHECK(conformanceFailures(
              *source, [&](QueryPlan const& plan) { return model.run(plan); }, corpus, smallCap())
              .empty());
}

namespace {

// A date parser that reads nothing, so every date operand is refused by apply and by the plan alike.
class RefusingDates final : public DateParser {
public:
    [[nodiscard]] std::optional<std::int64_t> parseDate(std::string_view /*text*/,
                                                        std::string_view /*format*/) const override {
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::int64_t> parseDateTime(std::string_view /*text*/,
                                                            std::string_view /*format*/) const override {
        return std::nullopt;
    }
};

// Instants in milliseconds around zero, one empty.
std::vector<std::optional<std::int64_t>> instantsMs() {
    return {-1500, -1001, -1000, -999, -1, 0, 1, 999, 1000, 1500, std::nullopt};
}

// The rows of an id and a date-time in whole seconds, with each instant floored or truncated toward zero.
std::vector<std::vector<Cell>> instantRows(bool floored) {
    std::vector<std::vector<Cell>> rows;
    std::int64_t id = 0;
    for (auto const& instant : instantsMs()) {
        Cell at = std::monostate{};
        if (instant) {
            std::int64_t seconds = *instant / 1000;
            if (floored && *instant % 1000 < 0) {
                --seconds;
            }
            at = seconds;
        }
        rows.push_back({Cell{id++}, at});
    }
    return rows;
}

std::vector<ColumnInfo> instantColumns() { return {col("id", ColumnKind::Key), col("at", ColumnKind::DateTime)}; }

}  // namespace

TEST_CASE("table: the corpus writes date operands the date parser reads", "[table][database_pages]") {
    auto const source = pageSource(60);
    std::map<std::string, std::size_t> withOperand;
    for (auto const& query : standardCorpus(*source)) {
        for (auto const& [column, filter] : query.filters.columns) {
            for (auto const& entry : filter.include) {
                if ((column == "on" || column == "at") && entry.eq &&
                    planQuery(query, source->columns()).has_value()) {
                    ++withOperand[column];
                }
            }
        }
    }
    CHECK(withOperand["on"] > 0);
    CHECK(withOperand["at"] > 0);
}

TEST_CASE("table: the corpus holds a translation to the floor of a negative instant", "[table][database_pages]") {
    // The source holds each instant floored to its UTC second, as the engine does.
    auto const source = tabletest::makeSource(instantColumns(), instantRows(true));
    auto const corpus = standardCorpus(*source);
    ReferenceModel const flooring{instantColumns(), instantRows(true)};
    ReferenceModel const truncating{instantColumns(), instantRows(false)};
    CHECK(conformanceFailures(
              *source, [&](QueryPlan const& plan) { return flooring.run(plan); }, corpus, smallCap())
              .empty());
    // -1500 ms is second -2, not -1: a model that truncates toward zero places it a second late.
    CHECK_FALSE(conformanceFailures(
                    *source, [&](QueryPlan const& plan) { return truncating.run(plan); }, corpus, smallCap())
                    .empty());
}

TEST_CASE("table: the helper reports a column whose queries every side refused", "[table][database_pages]") {
    auto const source = pageSource(60);
    ReferenceModel const model{pageColumns(), pageRows(60)};
    auto const corpus = standardCorpus(*source);
    auto options = smallCap();
    options.services.dates = std::make_shared<RefusingDates const>();
    auto const failures =
        conformanceFailures(*source, [&](QueryPlan const& plan) { return model.run(plan); }, corpus, options);
    auto const reported = [&](std::string_view use) {
        return std::ranges::any_of(failures, [&](std::string const& failure) { return failure.starts_with(use); });
    };
    CHECK(reported("filter at:"));
    CHECK(reported("filter on:"));
    CHECK_FALSE(reported("filter n:"));
    CHECK_FALSE(reported("sort at:"));
}

TEST_CASE("table: a total kind goes over the wire by name", "[table][database_pages]") {
    Page<int> page{.rows = {1}, .total = 5, .offset = 0, .totalKind = TotalKind::AtLeast};
    auto const json = glz::write_json(page);
    REQUIRE(json.has_value());
    CHECK(json->find("\"totalKind\":\"atLeast\"") != std::string::npos);
    page.totalKind = TotalKind::Exact;
    CHECK(glz::write_json(page).value().find("\"totalKind\":\"exact\"") != std::string::npos);
    Page<int> back;
    REQUIRE_FALSE(glz::read_json(back, R"({"rows":[],"total":3,"offset":0,"totalKind":"atLeast"})"));
    CHECK(back.totalKind == TotalKind::AtLeast);
    CHECK(glz::read_json(back, R"({"rows":[],"total":3,"offset":0,"totalKind":1})"));  // a number is refused
}
