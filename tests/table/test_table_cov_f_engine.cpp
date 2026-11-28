// SPDX-License-Identifier: Apache-2.0
//
// What an engine keeps of its sort and filter when the source's columns or groups change, and what it refuses
// before it computes a view.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/table/engine.hpp>
#include <string>
#include <utility>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::col;

namespace {

using Rows = std::vector<std::vector<Cell>>;

Rows threeColumnRows() {
    return {{std::string{"x"}, std::string{"p"}, std::string{"q"}},
            {std::string{"y"}, std::string{"p"}, std::string{"r"}}};
}

}  // namespace

TEST_CASE("a source that drops columns and groups takes their sort keys and filters with them",
          "[covf][table][engine]") {
    morph::exec::MainThreadExecutor owner;
    auto const source = tabletest::makeSource(
        {col("a", ColumnKind::Text), col("b", ColumnKind::Text), col("c", ColumnKind::Text)}, threeColumnRows());
    source->setGroups({ColumnGroupInfo{.id = "ab", .label = "AB", .columns = {"a", "b"}}});
    Engine engine{source, EngineOptions{.owner = &owner, .worker = nullptr}};
    REQUIRE(engine.setSort({{"a", SortDirection::Ascending}, {"c", SortDirection::Descending}}).has_value());

    FilterSpec spec;
    spec.columns["c"].include = {FilterEntry{.eq = "q"}};
    spec.columns["a"].include = {FilterEntry{.eq = "x"}};
    spec.groups.push_back(GroupFilter{.group = "ab", .include = {FilterEntry{.contains = "p"}}});
    spec.groups.push_back(GroupFilter{.columns = {"b", "c"}, .include = {FilterEntry{.contains = "p"}}});
    spec.groups.push_back(GroupFilter{.columns = {"a", "c"}, .include = {FilterEntry{.contains = "x"}}});
    REQUIRE(engine.setFilter(spec).has_value());
    REQUIRE(tabletest::pumpUntil(owner, [&] { return !engine.pending(); }));

    source->setGroups({});
    source->setRows({RowId{std::int64_t{0}}, RowId{std::int64_t{1}}}, Rows{{std::string{"x"}}, {std::string{"y"}}},
                    std::vector<ColumnInfo>{col("a", ColumnKind::Text)});
    REQUIRE(tabletest::pumpUntil(owner, [&] { return engine.columns().size() == 1; }));
    REQUIRE(tabletest::pumpUntil(owner, [&] { return !engine.pending(); }));

    // The sort key of the dropped column and the filters on dropped columns are gone; the declared group is no
    // longer declared; a group of columns keeps the ones that remain.
    REQUIRE(engine.sort().size() == 1);
    CHECK(engine.sort()[0].column == "a");
    CHECK(engine.filter().columns.size() == 1);
    CHECK(engine.filter().columns.contains("a"));
    REQUIRE(engine.filter().groups.size() == 1);
    CHECK(engine.filter().groups[0].columns == std::vector<std::string>{"a"});
    CHECK(engine.viewRowCount() == 1);
}

TEST_CASE("a filter that no longer compiles for a column's new kind is dropped", "[covf][table][engine]") {
    morph::exec::MainThreadExecutor owner;
    auto const source =
        tabletest::makeSource({col("a", ColumnKind::Text)}, Rows{{std::string{"x"}}, {std::string{"7"}}});
    Engine engine{source, EngineOptions{.owner = &owner, .worker = nullptr}};
    FilterSpec spec;
    spec.columns["a"].include = {FilterEntry{.eq = "x"}};
    REQUIRE(engine.setFilter(spec).has_value());
    REQUIRE(tabletest::pumpUntil(owner, [&] { return !engine.pending(); }));
    CHECK(engine.viewRowCount() == 1);

    source->setRows({RowId{std::int64_t{0}}, RowId{std::int64_t{1}}}, Rows{{std::int64_t{3}}, {std::int64_t{7}}},
                    std::vector<ColumnInfo>{col("a", ColumnKind::Integer)});
    REQUIRE(tabletest::pumpUntil(owner, [&] { return engine.columns().front().kind == ColumnKind::Integer; }));
    REQUIRE(tabletest::pumpUntil(owner, [&] { return !engine.pending(); }));
    CHECK(engine.filter().columns.empty());
    CHECK(engine.viewRowCount() == 2);
}

TEST_CASE("a sort over a measure column is refused while the engine has no units", "[covf][table][engine]") {
    auto measure = col("m", ColumnKind::Measure);
    measure.dimensions = {"mass"};
    auto const source = tabletest::makeSource({measure}, Rows{{Cell{}}});
    Engine engine{source};
    auto const refused = engine.setSort({{.column = "m", .dir = SortDirection::Ascending}});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::SourceContract);
    CHECK(engine.sort().empty());
    auto const unknown = engine.setSort({{.column = "zz", .dir = SortDirection::Ascending}});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == TableErrorCode::UnknownColumn);
}

TEST_CASE("a filter is refused while the source's declared groups are malformed", "[covf][table][engine]") {
    auto const source = tabletest::makeSource({col("a", ColumnKind::Text)}, Rows{{std::string{"x"}}});
    source->setGroups({ColumnGroupInfo{.id = "g", .label = "G", .columns = {"nothing"}}});
    Engine engine{source};
    FilterSpec spec;
    spec.columns["a"].include = {FilterEntry{.eq = "x"}};
    auto const refused = engine.setFilter(spec);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::SourceContract);
    CHECK(engine.filter().columns.empty());
}

TEST_CASE("retaining the rows already retained changes nothing", "[covf][table][engine]") {
    auto const source = tabletest::makeSource({col("a", ColumnKind::Text)},
                                              Rows{{std::string{"x"}}, {std::string{"y"}}, {std::string{"z"}}});
    Engine engine{source};
    FilterSpec spec;
    spec.columns["a"].include = {FilterEntry{.eq = "x"}};
    REQUIRE(engine.setFilter(spec).has_value());
    REQUIRE(engine.retain({RowId{std::int64_t{2}}}).has_value());
    CHECK(engine.viewRowCount() == 2);
    CHECK(engine.isUnmatched(1));
    CHECK_FALSE(engine.isUnmatched(0));
    CHECK_FALSE(engine.isUnmatched(7));
    REQUIRE(engine.retain({RowId{std::int64_t{2}}}).has_value());
    CHECK(engine.viewRowCount() == 2);
    REQUIRE(engine.retain({}).has_value());
    CHECK(engine.viewRowCount() == 1);
    CHECK(engine.retained().empty());
}
