// SPDX-License-Identifier: Apache-2.0
//
// morph::table query validation, one refusal per bound, and the values a header lists for every column kind.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/table/data_source.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "table/table_fixtures.hpp"

using namespace morph::table;
using tabletest::col;
using tabletest::dec;

namespace {

std::vector<ColumnInfo> validationColumns() {
    return {col("a", ColumnKind::Text), col("b", ColumnKind::Integer), col("c", ColumnKind::Text),
            col("cu", ColumnKind::Custom, "cmp"), col("d", ColumnKind::Date)};
}

std::vector<ColumnGroupInfo> validationGroups() {
    return {ColumnGroupInfo{.id = "ab", .label = "AB", .columns = {"a", "c"}},
            ColumnGroupInfo{.id = "none", .label = "None", .columns = {}}};
}

FilterEntry text(std::string value) { return FilterEntry{.eq = std::move(value)}; }

FilterEntry oneOfValues(std::vector<std::string> values) { return FilterEntry{.oneOf = std::move(values)}; }

struct Refusal {
    std::string name;
    std::function<void(TableQuery&, QueryLimits&)> shape;
    TableErrorCode code;
    std::string column;
    std::string message;
};

std::vector<Refusal> refusals() {
    using Code = TableErrorCode;
    return {
        {.name = "offset below zero",
         .shape = [](TableQuery& q, QueryLimits&) { q.page.offset = -1; },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "page.offset out of range"},
        {.name = "offset past the cap",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxOffset = 5;
                 q.page.offset = 6;
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "page.offset out of range"},
        {.name = "negative limit",
         .shape = [](TableQuery& q, QueryLimits&) { q.page.limit = -1; },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "page.limit is negative"},
        {.name = "too many sort keys",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxSortKeys = 1;
                 q.sort = {SortKey{.column = "a"}, SortKey{.column = "b"}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "too many sort keys"},
        {.name = "unknown sort column",
         .shape = [](TableQuery& q, QueryLimits&) { q.sort = {SortKey{.column = "zz"}}; },
         .code = Code::UnknownColumn,
         .column = "zz",
         .message = ""},
        {.name = "too many filtered columns",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxFilterColumns = 1;
                 q.filters.columns["a"] = {};
                 q.filters.columns["b"] = {};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "too many filtered columns"},
        {.name = "unknown filter column",
         .shape = [](TableQuery& q, QueryLimits&) { q.filters.columns["zz"] = {}; },
         .code = Code::UnknownColumn,
         .column = "zz",
         .message = ""},
        {.name = "too many entries in a column filter",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxEntries = 1;
                 q.filters.columns["a"].include = {text("x")};
                 q.filters.columns["a"].exclude = {text("y")};
             },
         .code = Code::LimitExceeded,
         .column = "a",
         .message = "too many filter entries"},
        {.name = "a column include value too long",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxTextLength = 3;
                 q.filters.columns["a"].include = {text("long")};
             },
         .code = Code::LimitExceeded,
         .column = "a",
         .message = "filter value too long"},
        {.name = "a column exclude value too long",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxTextLength = 3;
                 q.filters.columns["a"].exclude = {text("long")};
             },
         .code = Code::LimitExceeded,
         .column = "a",
         .message = "filter value too long"},
        {.name = "a column include set too large",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxSetValues = 1;
                 q.filters.columns["a"].include = {oneOfValues({"x", "y"})};
             },
         .code = Code::LimitExceeded,
         .column = "a",
         .message = "too many values in one set"},
        {.name = "a column exclude set too large",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxSetValues = 1;
                 q.filters.columns["a"].exclude = {oneOfValues({"x", "y"})};
             },
         .code = Code::LimitExceeded,
         .column = "a",
         .message = "too many values in one set"},
        {.name = "too many filter groups",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxGroups = 1;
                 q.filters.groups = {GroupFilter{.columns = {"a"}}, GroupFilter{.columns = {"b"}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "too many filter groups"},
        {.name = "a filter group with too many columns",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxGroupColumns = 1;
                 q.filters.groups = {GroupFilter{.columns = {"a", "b"}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "filter group too large"},
        {.name = "a filter group with too many entries",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxEntries = 1;
                 q.filters.groups = {GroupFilter{.columns = {"a"}, .include = {text("x")}, .exclude = {text("y")}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "filter group too large"},
        {.name = "a filter group naming a group nobody declared",
         .shape = [](TableQuery& q, QueryLimits&) { q.filters.groups = {GroupFilter{.group = "nope"}}; },
         .code = Code::UnknownColumn,
         .column = "nope",
         .message = "no column group nope"},
        {.name = "a filter group naming an unknown column",
         .shape = [](TableQuery& q, QueryLimits&) { q.filters.groups = {GroupFilter{.columns = {"a", "zz"}}}; },
         .code = Code::UnknownColumn,
         .column = "zz",
         .message = ""},
        {.name = "a filter group's include value too long",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxTextLength = 3;
                 q.filters.groups = {GroupFilter{.columns = {"a"}, .include = {text("long")}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "filter value too long"},
        {.name = "a filter group's exclude value too long",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxTextLength = 3;
                 q.filters.groups = {GroupFilter{.columns = {"a"}, .exclude = {text("long")}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "filter value too long"},
        {.name = "a filter group's include set too large",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxSetValues = 1;
                 q.filters.groups = {GroupFilter{.columns = {"a"}, .include = {oneOfValues({"x", "y"})}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "too many values in one set"},
        {.name = "a filter group's exclude set too large",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxSetValues = 1;
                 q.filters.groups = {GroupFilter{.columns = {"a"}, .exclude = {oneOfValues({"x", "y"})}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "too many values in one set"},
        {.name = "too many set values in one query",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxQuerySetValues = 3;
                 q.filters.columns["a"].include = {oneOfValues({"x", "y"})};
                 q.filters.columns["c"].include = {oneOfValues({"x", "y"})};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "too many set values in one query"},
        {.name = "set values too long in one query",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxQuerySetBytes = 3;
                 q.filters.columns["a"].include = {oneOfValues({"xx", "yy"})};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "set values too long in one query"},
        {.name = "too many columns",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxColumns = 1;
                 q.columns = std::vector<std::string>{"a", "b"};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "too many columns"},
        {.name = "an unknown asked column",
         .shape = [](TableQuery& q, QueryLimits&) { q.columns = std::vector<std::string>{"zz"}; },
         .code = Code::UnknownColumn,
         .column = "zz",
         .message = ""},
        {.name = "a keyed query with an offset",
         .shape =
             [](TableQuery& q, QueryLimits&) {
                 q.keys = std::vector<RowId>{RowId{std::int64_t{1}}};
                 q.page.offset = 1;
             },
         .code = Code::InvalidSpec,
         .column = "",
         .message = "a keyed query has no page"},
        {.name = "too many keys",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxKeys = 1;
                 q.keys = std::vector<RowId>{RowId{std::int64_t{1}}, RowId{std::int64_t{2}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "too many keys"},
        {.name = "a key too long",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxTextLength = 3;
                 q.keys = std::vector<RowId>{RowId{std::int64_t{1}}, RowId{std::string{"long"}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "key too long"},
        {.name = "too many retained rows",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxRetained = 1;
                 q.retain = {RowId{std::int64_t{1}}, RowId{std::int64_t{2}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "too many retained rows"},
        {.name = "a retained key too long",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxTextLength = 3;
                 q.retain = {RowId{std::int64_t{1}}, RowId{std::string{"long"}}};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "retained key too long"},
        {.name = "keys and distinct together",
         .shape =
             [](TableQuery& q, QueryLimits&) {
                 q.keys = std::vector<RowId>{};
                 q.distinct = DistinctRequest{.column = "a"};
             },
         .code = Code::InvalidSpec,
         .column = "",
         .message = "a query is keyed or distinct"},
        {.name = "distinct naming neither column nor group",
         .shape = [](TableQuery& q, QueryLimits&) { q.distinct = DistinctRequest{}; },
         .code = Code::InvalidSpec,
         .column = "",
         .message = "a distinct request names a column or a group"},
        {.name = "distinct naming both",
         .shape = [](TableQuery& q, QueryLimits&) { q.distinct = DistinctRequest{.column = "a", .group = "ab"}; },
         .code = Code::InvalidSpec,
         .column = "",
         .message = "a distinct request names a column or a group"},
        {.name = "distinct naming a group nobody declared",
         .shape = [](TableQuery& q, QueryLimits&) { q.distinct = DistinctRequest{.group = "nope"}; },
         .code = Code::UnknownColumn,
         .column = "nope",
         .message = "no column group nope"},
        {.name = "distinct naming a group with no columns",
         .shape = [](TableQuery& q, QueryLimits&) { q.distinct = DistinctRequest{.group = "none"}; },
         .code = Code::InvalidSpec,
         .column = "",
         .message = "column group none has no columns"},
        {.name = "distinct on an unknown column",
         .shape = [](TableQuery& q, QueryLimits&) { q.distinct = DistinctRequest{.column = "zz"}; },
         .code = Code::UnknownColumn,
         .column = "zz",
         .message = ""},
        {.name = "distinct search too long",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxTextLength = 3;
                 q.distinct = DistinctRequest{.column = "a", .search = "long"};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "distinct search too long"},
        {.name = "distinct offset below zero",
         .shape = [](TableQuery& q, QueryLimits&) { q.distinct = DistinctRequest{.column = "a", .offset = -1}; },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "distinct offset out of range"},
        {.name = "distinct offset past the cap",
         .shape =
             [](TableQuery& q, QueryLimits& l) {
                 l.maxOffset = 5;
                 q.distinct = DistinctRequest{.column = "a", .offset = 6};
             },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "distinct offset out of range"},
        {.name = "distinct limit below zero",
         .shape = [](TableQuery& q, QueryLimits&) { q.distinct = DistinctRequest{.column = "a", .limit = -1}; },
         .code = Code::LimitExceeded,
         .column = "",
         .message = "distinct limit is negative"},
        {.name = "distinct on a custom column",
         .shape = [](TableQuery& q, QueryLimits&) { q.distinct = DistinctRequest{.column = "cu"}; },
         .code = Code::UnsupportedOperator,
         .column = "cu",
         .message = "column cu has no values to list"},
        {.name = "a date bucket on text",
         .shape = [](TableQuery& q,
                     QueryLimits&) { q.distinct = DistinctRequest{.column = "a", .bucket = DateBucket::Month}; },
         .code = Code::InvalidSpec,
         .column = "",
         .message = "a date bucket on column a, which holds no dates"},
    };
}

}  // namespace

TEST_CASE("validate refuses a query over each bound with its typed error", "[covc][table][query]") {
    auto const columns = validationColumns();
    auto const groups = validationGroups();
    for (auto const& refusal : refusals()) {
        CAPTURE(refusal.name);
        TableQuery query;
        QueryLimits limits;
        refusal.shape(query, limits);
        auto const result = validate(query, columns, limits, {}, groups);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == refusal.code);
        CHECK(result.error().column == refusal.column);
        CHECK(result.error().message.contains(refusal.message));
    }
}

TEST_CASE("validate refuses a column the principal may not read, in every place a query names one",
          "[covc][table][query]") {
    auto const columns = validationColumns();
    auto const groups = validationGroups();
    auto const mayRead = [](std::string_view id) { return id != "b"; };
    auto const refused = [&](auto const& shape) {
        TableQuery query;
        shape(query);
        auto const result = validate(query, columns, QueryLimits{}, mayRead, groups);
        return result ? std::optional<TableErrorCode>{} : std::optional{result.error().code};
    };
    CHECK(refused([](TableQuery& q) { q.sort = {SortKey{.column = "b"}}; }) == TableErrorCode::ColumnNotReadable);
    CHECK(refused([](TableQuery& q) { q.filters.columns["b"] = {}; }) == TableErrorCode::ColumnNotReadable);
    CHECK(refused([](TableQuery& q) { q.filters.groups = {GroupFilter{.columns = {"b"}}}; }) ==
          TableErrorCode::ColumnNotReadable);
    CHECK(refused([](TableQuery& q) { q.columns = std::vector<std::string>{"b"}; }) ==
          TableErrorCode::ColumnNotReadable);
    CHECK(refused([](TableQuery& q) { q.distinct = DistinctRequest{.column = "b"}; }) ==
          TableErrorCode::ColumnNotReadable);
    CHECK_FALSE(refused([](TableQuery& q) { q.distinct = DistinctRequest{.group = "ab"}; }));
}

TEST_CASE("validate clamps the limits and answers the asked columns in the table's order", "[covc][table][query]") {
    auto const columns = validationColumns();
    TableQuery query;
    query.page.limit = 5000;
    query.columns = std::vector<std::string>{"c", "a"};
    query.distinct = DistinctRequest{.column = "a", .limit = 5000};
    auto const result = validate(query, columns, QueryLimits{}, {}, validationGroups());
    REQUIRE(result);
    CHECK(result->page.limit == 1000);
    CHECK(result->distinct->limit == 1000);
    CHECK(*result->columns == std::vector<std::string>{"a", "c"});
}

TEST_CASE("a filter value too long is refused in every operator of an entry", "[covc][table][query]") {
    auto const columns = validationColumns();
    QueryLimits limits;
    limits.maxTextLength = 3;
    std::vector<std::pair<std::string, std::function<void(FilterEntry&)>>> const operators{
        {"eq", [](FilterEntry& e) { e.eq = "long"; }},
        {"ne", [](FilterEntry& e) { e.ne = "long"; }},
        {"lt", [](FilterEntry& e) { e.lt = "long"; }},
        {"le", [](FilterEntry& e) { e.le = "long"; }},
        {"gt", [](FilterEntry& e) { e.gt = "long"; }},
        {"ge", [](FilterEntry& e) { e.ge = "long"; }},
        {"contains", [](FilterEntry& e) { e.contains = "long"; }},
        {"startsWith", [](FilterEntry& e) { e.startsWith = "long"; }},
        {"endsWith", [](FilterEntry& e) { e.endsWith = "long"; }},
        {"between low", [](FilterEntry& e) { e.between = std::array<std::string, 2>{"long", "a"}; }},
        {"between high", [](FilterEntry& e) { e.between = std::array<std::string, 2>{"a", "long"}; }},
        {"oneOf member", [](FilterEntry& e) { e.oneOf = std::vector<std::string>{"a", "long"}; }},
    };
    for (auto const& [name, set] : operators) {
        CAPTURE(name);
        FilterEntry entry;
        set(entry);
        TableQuery query;
        query.filters.columns["a"].include = {entry};
        auto const result = validate(query, columns, limits, {}, {});
        REQUIRE_FALSE(result);
        CHECK(result.error().message == "filter value too long");
        FilterEntry shortEntry;
        shortEntry.eq = "abc";
        shortEntry.between = std::array<std::string, 2>{"a", "b"};
        shortEntry.oneOf = std::vector<std::string>{"a", "bcd"};
        TableQuery fine;
        fine.filters.columns["a"].include = {shortEntry};
        CHECK(validate(fine, columns, limits, {}, {}));
    }
}

namespace {

using Texts = std::vector<std::pair<std::string, std::int64_t>>;

Texts listed(std::expected<DistinctValues, TableError> const& result) {
    Texts out;
    REQUIRE(result);
    for (auto const& value : result->values) {
        out.emplace_back(value.text, value.count);
    }
    return out;
}

// Nine columns of three rows, each kind once; the rows are in no column's order.
std::shared_ptr<VectorSource> everyKindSource() {
    using S = std::string;
    return tabletest::makeSource(
        {col("i", ColumnKind::Integer), col("k", ColumnKind::Key), col("n", ColumnKind::Number),
         col("f", ColumnKind::Bool), col("m", ColumnKind::Decimal), col("q", ColumnKind::Quantity),
         col("t", ColumnKind::Text), col("ts", ColumnKind::DateTime), col("dd", ColumnKind::Date)},
        {{std::int64_t{30}, std::int64_t{3}, 2.5, true, dec("1.5"), dec("2"), S{"pear"}, std::int64_t{90061},
          std::int64_t{19000}},
         {std::int64_t{10}, std::int64_t{1}, 0.0, false, dec("0.25"), dec("1"), S{"apple"}, std::int64_t{0},
          std::int64_t{18000}},
         {std::int64_t{20}, std::int64_t{2}, -0.0, true, dec("1.5"), dec("2"), S{"fig"}, std::int64_t{-1},
          std::int64_t{19000}}});
}

}  // namespace

TEST_CASE("distinct values are listed in each kind's own order and spelling", "[covc][table][query][distinct]") {
    auto const source = everyKindSource();
    auto const values = [&](std::string column, DateBucket bucket = DateBucket::Value) {
        return listed(
            distinctValues(*source, TableQuery{}, DistinctRequest{.column = std::move(column), .bucket = bucket}));
    };
    CHECK(values("i") == Texts{{"10", 1}, {"20", 1}, {"30", 1}});
    CHECK(values("k") == Texts{{"1", 1}, {"2", 1}, {"3", 1}});
    CHECK(values("n") == Texts{{"0", 2}, {"2.5", 1}});
    CHECK(values("f") == Texts{{"false", 1}, {"true", 2}});
    CHECK(values("m") == Texts{{"0.25", 1}, {"1.5", 2}});
    CHECK(values("q") == Texts{{"1", 1}, {"2", 2}});
    CHECK(values("t") == Texts{{"apple", 1}, {"fig", 1}, {"pear", 1}});
    CHECK(values("ts") ==
          Texts{{"1969-12-31T23:59:59Z", 1}, {"1970-01-01T00:00:00Z", 1}, {"1970-01-02T01:01:01Z", 1}});
    CHECK(values("ts", DateBucket::Month) == Texts{{"1969-12", 1}, {"1970-01", 2}});
    CHECK(values("ts", DateBucket::Year) == Texts{{"1969", 1}, {"1970", 2}});
    CHECK(values("ts", DateBucket::Day) == Texts{{"1969-12-31", 1}, {"1970-01-01", 1}, {"1970-01-02", 1}});
    CHECK(values("dd") == Texts{{"2019-04-14", 1}, {"2022-01-08", 2}});
    CHECK(values("dd", DateBucket::Year) == Texts{{"2019", 1}, {"2022", 2}});
    CHECK(values("dd", DateBucket::Month) == Texts{{"2019-04", 1}, {"2022-01", 2}});
}

TEST_CASE("a distinct value carries the operand a filter takes back", "[covc][table][query][distinct]") {
    auto const source = everyKindSource();
    auto const operands = [&](std::string column, DateBucket bucket) {
        auto const result =
            distinctValues(*source, TableQuery{}, DistinctRequest{.column = std::move(column), .bucket = bucket});
        REQUIRE(result);
        std::vector<std::string> out;
        for (auto const& value : result->values) {
            out.push_back(value.operand);
        }
        return out;
    };
    CHECK(operands("ts", DateBucket::Year) == std::vector<std::string>{"1969-01-01T00:00:00Z..1969-12-31T23:59:59Z",
                                                                       "1970-01-01T00:00:00Z..1970-12-31T23:59:59Z"});
    CHECK(operands("dd", DateBucket::Month) ==
          std::vector<std::string>{"2019-04-01..2019-04-30", "2022-01-01..2022-01-31"});
    CHECK(operands("f", DateBucket::Value) == std::vector<std::string>{"false", "true"});
}

TEST_CASE("a date column holding text lists the text as written", "[covc][table][query][distinct]") {
    auto const source = tabletest::column(
        ColumnKind::Date, {std::string{"2024-02-29"}, std::string{"2024-01-05"}, std::string{"2024-02-29"}});
    auto const result = distinctValues(*source, TableQuery{}, DistinctRequest{.column = "c"});
    CHECK(listed(result) == Texts{{"2024-01-05", 1}, {"2024-02-29", 2}});
}

TEST_CASE("a measure column lists its amounts with their units, and needs a registry",
          "[covc][table][query][distinct]") {
    UnitRegistryBuilder builder;
    REQUIRE(builder.add(UnitInfo{.id = "kg", .symbol = "kg", .dimension = "mass", .toBase = dec("1"), .decimals = 3}));
    REQUIRE(
        builder.add(UnitInfo{.id = "g", .symbol = "", .dimension = "mass", .toBase = dec("0.001"), .decimals = 0}));
    auto const registry = std::move(builder).build();
    auto measureColumn = col("w", ColumnKind::Measure);
    measureColumn.dimensions = {"mass"};
    auto const kg = *registry->find("kg");
    auto const g = *registry->find("g");
    auto const source = tabletest::makeSource({measureColumn}, {{Cell{Measure{.amount = dec("2"), .unit = kg}}},
                                                                {Cell{Measure{.amount = dec("500"), .unit = g}}},
                                                                {Cell{Measure{.amount = dec("2"), .unit = kg}}}});
    ApplyOptions options;
    options.services.units = registry;
    auto const result = distinctValues(*source, TableQuery{}, DistinctRequest{.column = "w"}, options);
    REQUIRE(result);
    REQUIRE(result->values.size() == 2);
    CHECK(result->values[0].text == "500");
    CHECK(result->values[0].operand == "500 g");
    CHECK(result->values[0].unit == "g");
    CHECK(result->values[0].dimension == "mass");
    CHECK(result->values[0].count == 1);
    CHECK(result->values[1].text == "2 kg");
    CHECK(result->values[1].operand == "2 kg");
    CHECK(result->values[1].count == 2);

    auto const unregistered = distinctValues(*source, TableQuery{}, DistinctRequest{.column = "w"});
    REQUIRE_FALSE(unregistered);
    CHECK(unregistered.error().code == TableErrorCode::SourceContract);
}

TEST_CASE("a pooled group lists the values of its columns once", "[covc][table][query][distinct]") {
    auto source = tabletest::makeSource(
        {col("a", ColumnKind::Integer), col("b", ColumnKind::Integer)},
        {{std::int64_t{1}, std::int64_t{2}}, {std::int64_t{2}, std::int64_t{3}}, {std::int64_t{3}, std::int64_t{3}}});
    source->setGroups({ColumnGroupInfo{.id = "ab", .label = "AB", .columns = {"a", "b"}}});
    auto const result = distinctValues(*source, TableQuery{}, DistinctRequest{.group = "ab"});
    CHECK(listed(result) == Texts{{"1", 1}, {"2", 2}, {"3", 2}});
    auto searched = distinctValues(*source, TableQuery{}, DistinctRequest{.group = "ab", .search = "3"});
    CHECK(listed(searched) == Texts{{"3", 2}});
}

TEST_CASE("a measure column reads an integer, a rational and a text with its unit", "[covc][table][query][distinct]") {
    UnitRegistryBuilder builder;
    REQUIRE(builder.add(UnitInfo{.id = "kg", .symbol = "kg", .dimension = "mass", .toBase = dec("1"), .decimals = 3}));
    auto const registry = std::move(builder).build();
    auto measureColumn = col("w", ColumnKind::Measure);
    measureColumn.dimensions = {"mass"};
    using S = std::string;
    auto const source = tabletest::makeSource({measureColumn}, {{Cell{S{"3 kg"}}},
                                                                {Cell{S{"3 kg"}}},
                                                                {Cell{S{"3 parsecs"}}},
                                                                {Cell{S{"abc"}}},
                                                                {Cell{std::int64_t{7}}},
                                                                {Cell{dec("1.5")}},
                                                                {Cell{}}});
    ApplyOptions options;
    options.services.units = registry;
    auto const result = distinctValues(*source, TableQuery{}, DistinctRequest{.column = "w"}, options);
    REQUIRE(result);
    std::int64_t counted = result->blanks + result->invalid;
    bool sawThree = false;
    for (auto const& value : result->values) {
        counted += value.count;
        if (value.text == "3 kg") {
            sawThree = true;
            CHECK(value.count == 2);
            CHECK(value.operand == "3 kg");
        }
    }
    CHECK(sawThree);
    CHECK(counted == 7);
    CHECK(result->blanks == 1);
    CHECK(result->invalid >= 2);
}

TEST_CASE("a row key is read from its JSON text, number or string, or refused", "[covc][table][query]") {
    auto const read = [](std::string text) { return rowIdOf(glz::raw_json{std::move(text)}); };
    CHECK(read("42") == RowId{std::int64_t{42}});
    CHECK(read("  -7\n") == RowId{std::int64_t{-7}});
    CHECK(read("\"abc\"") == RowId{std::string{"abc"}});
    CHECK(read(" \"a b\" ") == RowId{std::string{"a b"}});
    CHECK_FALSE(read(""));
    CHECK_FALSE(read("   "));
    CHECK_FALSE(read("\"open"));
    CHECK_FALSE(read("4.5"));
    CHECK_FALSE(read("x"));
    CHECK_FALSE(read("99999999999999999999"));
}

TEST_CASE("row keys are written as JSON and read back", "[covc][table][query]") {
    std::vector<RowId> const keys{RowId{std::int64_t{3}}, RowId{std::string{"q\"x"}}};
    auto const written = changeKeys(keys);
    REQUIRE(written.size() == 2);
    CHECK(written[0].str == "3");
    CHECK(written[1].str == "\"q\\\"x\"");
    CHECK(rowIdOf(written[0]) == keys[0]);
    CHECK(rowIdOf(written[1]) == keys[1]);
}

TEST_CASE("a measure's decimal places come from a power-of-ten denominator", "[covc][table][query]") {
    auto const read = [](std::int64_t num, std::int64_t den) {
        MeasureValue value;
        value.setWire(MeasureValue::Wire{.num = num, .den = den, .unit = "kg"});
        return value;
    };
    CHECK(read(12, 1).amount.decimalPlaces.value == 0);
    CHECK(read(125, 100).amount.decimalPlaces.value == 2);
    CHECK(read(1, 1'000'000'000'000'000'000LL).amount.decimalPlaces.value == 18);
    CHECK(read(1, 3).amount.decimalPlaces.value == morph::math::kMaxDecimalPlaces);
    CHECK(read(1, 7).amount.denominator == 7);
    CHECK(read(5, 0).amount == morph::math::Rational{});
    CHECK(read(5, -2).amount == morph::math::Rational{});
    CHECK(read(5, 10).unit == "kg");
}
