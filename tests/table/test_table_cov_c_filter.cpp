// SPDX-License-Identifier: Apache-2.0
//
// morph::table filters and sorts over every column kind: a set of values (`oneOf`) on a column and on a group, a
// group's include and exclude entries, and the order each kind sorts in.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <morph/table/data_source.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <string>
#include <utility>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::col;
using tabletest::dec;

namespace {

using Rows = std::vector<std::size_t>;

// A kind, the three values A < B < C it holds and the text a filter names them by.
struct KindCase {
    ColumnKind kind;
    std::array<Cell, 3> cells;
    std::array<std::string, 3> operands;
};

std::vector<KindCase> kindCases() {
    using S = std::string;
    return {
        {ColumnKind::Integer, {std::int64_t{1}, std::int64_t{2}, std::int64_t{3}}, {S{"1"}, S{"2"}, S{"3"}}},
        {ColumnKind::Key, {std::int64_t{1}, std::int64_t{2}, std::int64_t{3}}, {S{"1"}, S{"2"}, S{"3"}}},
        {ColumnKind::Decimal, {dec("1.5"), dec("2.5"), dec("3.5")}, {S{"1.5"}, S{"2.5"}, S{"3.5"}}},
        {ColumnKind::Quantity, {dec("1.5"), dec("2.5"), dec("3.5")}, {S{"1.5"}, S{"2.5"}, S{"3.5"}}},
        {ColumnKind::Number, {1.5, 2.5, 3.5}, {S{"1.5"}, S{"2.5"}, S{"3.5"}}},
        {ColumnKind::Date,
         {std::int64_t{19723}, std::int64_t{19724}, std::int64_t{19725}},
         {S{"2024-01-01"}, S{"2024-01-02"}, S{"2024-01-03"}}},
        {ColumnKind::DateTime,
         {std::int64_t{1704067200}, std::int64_t{1704153600}, std::int64_t{1704240000}},
         {S{"2024-01-01T00:00:00Z"}, S{"2024-01-02T00:00:00Z"}, S{"2024-01-03T00:00:00Z"}}},
        {ColumnKind::Text, {S{"alpha"}, S{"beta"}, S{"gamma"}}, {S{"alpha"}, S{"beta"}, S{"gamma"}}},
    };
}

// Two columns, x and y, of one kind: x holds A B C A, y holds B A C C, over rows 0 to 3.
std::shared_ptr<VectorSource> twoColumns(KindCase const& kind) {
    constexpr std::array<std::size_t, 4> kX{0, 1, 2, 0};
    constexpr std::array<std::size_t, 4> kY{1, 0, 2, 2};
    std::vector<std::vector<Cell>> rows;
    for (std::size_t row = 0; row < kX.size(); ++row) {
        rows.push_back({kind.cells.at(kX.at(row)), kind.cells.at(kY.at(row))});
    }
    return tabletest::makeSource({col("x", kind.kind), col("y", kind.kind)}, std::move(rows));
}

Rows rowsOf(DataSource const& source, TableQuery const& query) {
    auto const page = apply(source, query);
    REQUIRE(page);
    return page->rows;
}

FilterEntry oneOf(std::vector<std::string> values) { return FilterEntry{.oneOf = std::move(values)}; }
FilterEntry eq(std::string value) { return FilterEntry{.eq = std::move(value)}; }

}  // namespace

TEST_CASE("a set of values filters a column and a group of every kind", "[covc][table][filter]") {
    for (auto const& kind : kindCases()) {
        CAPTURE(static_cast<int>(kind.kind));
        auto const source = twoColumns(kind);
        auto const& a = kind.operands.at(0);
        auto const& b = kind.operands.at(1);
        auto const& c = kind.operands.at(2);

        TableQuery column;
        column.filters.columns["x"].include = {oneOf({a, c})};
        CHECK(rowsOf(*source, column) == Rows{0, 2, 3});

        TableQuery columnEq;
        columnEq.filters.columns["x"].include = {eq(b)};
        CHECK(rowsOf(*source, columnEq) == Rows{1});

        TableQuery any;
        any.filters.groups = {GroupFilter{.columns = {"x", "y"}, .include = {oneOf({a})}}};
        CHECK(rowsOf(*source, any) == Rows{0, 1, 3});

        TableQuery anyEq;
        anyEq.filters.groups = {GroupFilter{.columns = {"x", "y"}, .include = {eq(a)}}};
        CHECK(rowsOf(*source, anyEq) == Rows{0, 1, 3});

        TableQuery all;
        all.filters.groups = {GroupFilter{
            .columns = {"x", "y"}, .match = ColumnCount{.kind = ColumnCount::Kind::All}, .include = {oneOf({a, c})}}};
        CHECK(rowsOf(*source, all) == Rows{2, 3});

        TableQuery exclude;
        exclude.filters.groups = {GroupFilter{.columns = {"x", "y"}, .exclude = {oneOf({b})}}};
        CHECK(rowsOf(*source, exclude) == Rows{2, 3});

        TableQuery together;
        together.filters.groups = {
            GroupFilter{.columns = {"x", "y"}, .mode = GroupMode::Together, .include = {oneOf({a}), oneOf({b})}}};
        CHECK(rowsOf(*source, together) == Rows{0, 1});

        TableQuery excluded;
        excluded.filters.columns["y"].exclude = {oneOf({a, b})};
        CHECK(rowsOf(*source, excluded) == Rows{2, 3});
    }
}

TEST_CASE("every kind sorts in its own order, ascending and descending", "[covc][table][filter][sort]") {
    for (auto const& kind : kindCases()) {
        CAPTURE(static_cast<int>(kind.kind));
        auto const source = twoColumns(kind);
        TableQuery ascending;
        ascending.sort = {SortKey{.column = "x"}};
        CHECK(rowsOf(*source, ascending) == Rows{0, 3, 1, 2});
        TableQuery descending;
        descending.sort = {SortKey{.column = "x", .dir = SortDirection::Descending}};
        CHECK(rowsOf(*source, descending) == Rows{2, 1, 0, 3});
        TableQuery byY;
        byY.sort = {SortKey{.column = "y"}, SortKey{.column = "x", .dir = SortDirection::Descending}};
        CHECK(rowsOf(*source, byY) == Rows{1, 0, 2, 3});
    }
}

TEST_CASE("a number's zero matches whichever sign it is written with", "[covc][table][filter]") {
    auto const source =
        tabletest::makeSource({col("n", ColumnKind::Number), col("m", ColumnKind::Number)},
                              {{0.0, 1.0}, {-0.0, 0.0}, {1.0, -0.0}, {std::numeric_limits<double>::infinity(), 2.0}});
    TableQuery column;
    column.filters.columns["n"].include = {oneOf({"0", "5"})};
    CHECK(rowsOf(*source, column) == Rows{0, 1});
    TableQuery group;
    group.filters.groups = {GroupFilter{.columns = {"n", "m"}, .include = {oneOf({"0"})}}};
    CHECK(rowsOf(*source, group) == Rows{0, 1, 2});
    TableQuery exclude;
    exclude.filters.groups = {GroupFilter{.columns = {"n", "m"}, .exclude = {oneOf({"0"})}}};
    CHECK(rowsOf(*source, exclude) == Rows{3});
    TableQuery sorted;
    sorted.sort = {SortKey{.column = "n"}};
    CHECK(rowsOf(*source, sorted) == Rows{1, 0, 2, 3});
}

TEST_CASE("a group's operator that its column kind does not define applies to no column", "[covc][table][filter]") {
    auto const source = tabletest::makeSource({col("x", ColumnKind::Integer), col("y", ColumnKind::Integer)},
                                              {{std::int64_t{1}, std::int64_t{2}}});
    TableQuery query;
    query.filters.groups = {GroupFilter{.columns = {"x", "y"}, .include = {FilterEntry{.contains = "1"}}}};
    auto const page = apply(*source, query);
    REQUIRE_FALSE(page);
    CHECK(page.error().code == TableErrorCode::UnsupportedOperator);
    TableQuery custom;
    custom.filters.columns["x"].include = {FilterEntry{.isEmpty = true}};
    CHECK(rowsOf(*source, custom).empty());
}
