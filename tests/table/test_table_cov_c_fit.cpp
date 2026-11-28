// SPDX-License-Identifier: Apache-2.0
//
// An engine whose source changes its columns keeps the sort keys and filters that still apply to the new ones.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/table/engine.hpp>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::col;

namespace {

std::shared_ptr<VectorSource> threeColumns() {
    auto source =
        tabletest::makeSource({col("a", ColumnKind::Text), col("b", ColumnKind::Text), col("c", ColumnKind::Text)},
                              {{Cell{std::string{"x"}}, Cell{std::string{"y"}}, Cell{std::string{"z"}}},
                               {Cell{std::string{"p"}}, Cell{std::string{"q"}}, Cell{std::string{"r"}}}});
    source->setGroups({ColumnGroupInfo{.id = "g", .label = "G", .columns = {"a", "b"}}});
    return source;
}

void replaceColumns(VectorSource& source, std::vector<ColumnInfo> columns) {
    std::vector<std::vector<Cell>> rows;
    rows.reserve(2);
    for (std::int64_t at = 0; at < 2; ++at) {
        std::vector<Cell> row;
        row.reserve(columns.size());
        for (auto const& column : columns) {
            row.push_back(column.kind == ColumnKind::Integer ? Cell{at} : Cell{std::string{"x"}});
        }
        rows.push_back(std::move(row));
    }
    source.setRows({RowId{std::int64_t{0}}, RowId{std::int64_t{1}}}, std::move(rows), std::move(columns));
}

}  // namespace

TEST_CASE("a source that drops columns takes their sort keys and filters with them", "[covc][table][fit]") {
    auto const source = threeColumns();
    Engine engine{source};
    REQUIRE(engine.setSort({{"a", SortDirection::Ascending}, {"b", SortDirection::Descending}}));
    FilterSpec spec;
    spec.columns["a"].include = {FilterEntry{.contains = "x"}};
    spec.columns["b"].include = {FilterEntry{.contains = "y"}};
    spec.groups = {GroupFilter{.group = "g", .include = {FilterEntry{.contains = "x"}}},
                   GroupFilter{.columns = {"b", "c"}, .include = {FilterEntry{.contains = "x"}}},
                   GroupFilter{.columns = {"a", "b"}, .include = {FilterEntry{.contains = "x"}}}};
    REQUIRE(engine.setFilter(spec));

    source->setGroups({});
    replaceColumns(*source, {col("a", ColumnKind::Text), col("c", ColumnKind::Text)});
    CHECK(engine.viewRowCount() >= 0);
    CHECK(engine.sort() == SortChain{{.column = "a", .dir = SortDirection::Ascending}});
    CHECK(engine.filter().columns.size() == 1);
    CHECK(engine.filter().columns.contains("a"));
    REQUIRE(engine.filter().groups.size() == 2);
    CHECK(engine.filter().groups[0].columns == std::vector<std::string>{"c"});
    CHECK(engine.filter().groups[1].columns == std::vector<std::string>{"a"});
}

TEST_CASE("a filter that no longer compiles for a column's new kind is dropped whole", "[covc][table][fit]") {
    auto const source = threeColumns();
    Engine engine{source};
    FilterSpec spec;
    spec.columns["a"].include = {FilterEntry{.contains = "x"}};
    REQUIRE(engine.setFilter(spec));
    replaceColumns(*source, {col("a", ColumnKind::Integer), col("b", ColumnKind::Text)});
    CHECK(engine.viewRowCount() >= 0);
    CHECK(engine.filter().empty());
}

TEST_CASE("a source that replaces its rows under the same columns keeps the engine's state", "[covc][table][fit]") {
    auto const source = threeColumns();
    Engine engine{source};
    REQUIRE(engine.setSort({{"c", SortDirection::Descending}}));
    source->setRows({RowId{std::int64_t{5}}},
                    {{Cell{std::string{"k"}}, Cell{std::string{"l"}}, Cell{std::string{"m"}}}});
    CHECK(engine.viewRowCount() == 1);
    CHECK(engine.sort().size() == 1);
}
