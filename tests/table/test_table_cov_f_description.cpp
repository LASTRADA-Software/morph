// SPDX-License-Identifier: Apache-2.0
//
// The column an order level is read for, the rows a scope picks out of a shared source, and the cells a page's
// JSON holds that are not well formed.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <memory>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <string>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::col;
using tabletest::levelCol;

namespace {

std::vector<ColumnInfo> orderColumns() {
    return {col("a", ColumnKind::Text), levelCol("s", ColumnKind::Text, "slow"),
            levelCol("t", ColumnKind::Text, "slow"), levelCol("u", ColumnKind::Text, "other")};
}

std::vector<ColumnGroupInfo> orderGroups() {
    return {ColumnGroupInfo{.id = "at", .label = "AT", .columns = {"a", "t"}},
            ColumnGroupInfo{.id = "au", .label = "AU", .columns = {"a", "u"}}};
}

TableQuery sorted(std::vector<std::string> const& columns) {
    TableQuery query;
    for (auto const& column : columns) {
        query.sort.push_back(SortKey{.column = column, .dir = {}});
    }
    return query;
}

TableQuery filtered(std::string const& column) {
    TableQuery query;
    query.filters.columns[column] = ColumnFilter{.include = {FilterEntry{.eq = "x"}}};
    return query;
}

TableQuery listed(DistinctRequest request) {
    TableQuery query;
    query.distinct = std::move(request);
    return query;
}

}  // namespace

TEST_CASE("the column an order level is read for is the first one a sort, a filter or a values list names there",
          "[covf][table][description]") {
    struct Case {
        char const* name;
        TableQuery query;
        std::string expected;
    };
    auto const shape = GENERATE(Catch::Generators::values<Case>({
        {"nothing asked", TableQuery{}, ""},
        {"a sort key at the level", sorted({"s"}), "s"},
        {"a sort key at another level is passed over", sorted({"a", "u", "t"}), "t"},
        {"a sort key no column has is passed over", sorted({"zz", "s"}), "s"},
        {"a filter when no sort key is at the level",
         [] {
             auto query = filtered("t");
             query.sort = {SortKey{.column = "a", .dir = {}}};
             return query;
         }(),
         "t"},
        {"a filter on a column at another level", filtered("u"), ""},
        {"a filter on the base level only", filtered("a"), ""},
        {"a distinct column at the level", listed(DistinctRequest{.column = "s"}), "s"},
        {"a distinct column at another level", listed(DistinctRequest{.column = "u"}), ""},
        {"a distinct column the table lacks", listed(DistinctRequest{.column = "zz"}), ""},
        {"a distinct group lists its columns at the level", listed(DistinctRequest{.group = "at"}), "t"},
        {"a distinct group with none at the level", listed(DistinctRequest{.group = "au"}), ""},
        {"a distinct group the table lacks", listed(DistinctRequest{.group = "none"}), ""},
    }));
    CAPTURE(shape.name);
    auto const columns = orderColumns();
    auto const groups = orderGroups();
    CHECK(detail::orderColumnOf(shape.query, columns, "slow", Services{}, groups) == shape.expected);
}

TEST_CASE("a scope keeps its rows of the shared source and follows the source's next snapshot",
          "[covf][table][description]") {
    using Rows = std::vector<std::vector<Cell>>;
    auto const source =
        tabletest::makeSource({col("a", ColumnKind::Integer)},
                              Rows{{Cell{std::int64_t{10}}}, {Cell{std::int64_t{11}}}, {Cell{std::int64_t{12}}}});
    auto const scope = scoped(source, {RowId{std::int64_t{2}}, RowId{std::int64_t{0}}, RowId{std::int64_t{99}}});
    auto const first = scope->snapshot();
    REQUIRE(first->rowCount() == 2);
    CHECK(first->rowId(0) == RowId{std::int64_t{0}});
    CHECK(first->rowId(1) == RowId{std::int64_t{2}});
    // The shared snapshot is unchanged: the scope hands back the snapshot it already built.
    CHECK(scope->snapshot() == first);

    source->setRows({RowId{std::int64_t{2}}, RowId{std::int64_t{5}}},
                    Rows{{Cell{std::int64_t{1}}}, {Cell{std::int64_t{2}}}});
    auto const next = scope->snapshot();
    REQUIRE(next != first);
    REQUIRE(next->rowCount() == 1);
    CHECK(next->rowId(0) == RowId{std::int64_t{2}});
    CHECK(scope->columns().size() == 1);
    CHECK(scope->groups().empty());
}

TEST_CASE("a cell that is an array but not a quantity's pair of pairs is kept as the text it was written as",
          "[covf][table][description]") {
    auto const bad = GENERATE("[99999999999999999999,1]", "[1,10,[1,0]]", "[1,10,[1,10]", "[1,10,[1,10]]x",
                              "[1,10,[1]]", "[1,0]", "[1,-5]", "[1.5,10]");
    CAPTURE(bad);
    auto const cell = detail::cellFromJson(bad, std::nullopt);
    REQUIRE(std::holds_alternative<std::string>(cell));
    CHECK(std::get<std::string>(cell) == bad);
}

TEST_CASE("a quantity's cell is its amount and the factor to the canonical unit", "[covf][table][description]") {
    auto const cell = detail::cellFromJson("[3,10,[1,1000]]", std::nullopt);
    REQUIRE(std::holds_alternative<QuantityCell>(cell));
    CHECK(std::get<QuantityCell>(cell).amount == tabletest::dec("0.3"));
    CHECK(std::get<QuantityCell>(cell).toCanonical == tabletest::dec("0.001"));
}

TEST_CASE("a rest-of-page reply drops the cells of a column the page does not hold", "[covf][table][description]") {
    PageWindow<GenericRow> window{4, 3};
    std::vector<std::string> const columns{"a"};
    auto const fetches = window.visible(0, 4, columns);
    REQUIRE(fetches.size() == 2);
    auto const reply = [](std::int64_t first, std::int64_t count, std::vector<std::string> shown) {
        GenericPage page;
        page.columns = std::move(shown);
        for (std::int64_t at = 0; at < count; ++at) {
            page.keys.emplace_back(first + at);
            page.rows.emplace_back(page.columns.size(), Cell{first + at});
        }
        return page;
    };
    REQUIRE(window.accept(fetches[0], reply(0, 2, {"a"})));
    auto const rest = window.visible(0, 4, columns);
    REQUIRE(rest.size() == 1);
    REQUIRE(window.accept(rest[0], reply(2, 2, {"a", "z"})));
    CHECK(window.cell(3, "z") == nullptr);
    REQUIRE(window.cell(3, "a") != nullptr);
    CHECK(std::get<std::int64_t>(*window.cell(3, "a")) == 3);
}
