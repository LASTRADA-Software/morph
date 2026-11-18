// SPDX-License-Identifier: Apache-2.0
//
// morph::table descriptions, the generic page, and a query's columns (docs/spec/table/table.md, "A table is a
// description", "Rows on the wire", "Data levels").
//
// Mutations these tests were seen to fail on:
//  - describe hashing with the old revision in: "equal descriptions have equal revisions" fails.
//  - describe hashing the columns' ids only: "equal descriptions have equal revisions" fails.
//  - a Rational written as glaze's {"num","den","dp"} object: "every cell kind round-trips" fails.
//  - [1250, 100] read with DecimalPlaces{kMaxDecimalPlaces}: "every cell kind round-trips" fails.
//  - cellFromJson parsing strings for the column's kind: "a cell that does not read as its column's kind is
//    invalid" fails.
//  - a double written as glaze writes it (2.0 becomes 2): "a cell that does not read as its column's kind is
//    invalid" fails.
//  - a non-finite double written with glaze's default: "numbers at the edges cross the wire unchanged" fails.
//  - readGenericPage padding a short row with empty cells: "a malformed reply is refused" fails.
//  - validate ignoring `columns`: "columns round-trip and are bounded" fails.
//  - validate returning `columns` as received: "a columns list is answered in the description's order" fails.
//  - writeGenericPage ignoring `columns`: "a page names only the asked columns" fails.
//  - writeGenericPage writing the first 40 columns whatever it is given: "a whole page fits an envelope" fails.
//  - a glz::meta for TableDescription that leaves out `groups`: "a 300-column description round-trips" fails.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <limits>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"
#include "table/wide_table_model.hpp"

using namespace morph::table;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
using tabletest::dec;
using tabletest::sameCell;

namespace {

constexpr auto kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr auto kInt64Min = std::numeric_limits<std::int64_t>::min();

TableColumn column(std::string id, ColumnKind kind) {
    return TableColumn{.info = {.id = id, .kind = kind, .comparator = {}, .format = {}}, .label = id, .group = {}};
}

TableDescription describedOf(std::vector<TableColumn> columns) {
    return describe(TableDescription{.id = "t", .label = "T", .columns = std::move(columns), .key = "id"});
}

std::vector<RowId> intKeys(std::size_t count, std::int64_t first = 1) {
    std::vector<RowId> keys;
    for (std::size_t i = 0; i < count; ++i) {
        keys.emplace_back(first + static_cast<std::int64_t>(i));
    }
    return keys;
}

PageRows pageOf(std::size_t rows, std::optional<std::int64_t> total = std::nullopt) {
    PageRows page;
    page.rows.resize(rows);
    std::iota(page.rows.begin(), page.rows.end(), std::size_t{0});
    page.total = total;
    return page;
}

std::string toJson(GenericPage const& page) {
    auto json = glz::write_json(page);
    REQUIRE(json.has_value());
    return std::move(*json);
}

// The states buildKeys gives a single column of `kind` holding `cells`, one per row.
std::vector<CellState> statesOf(ColumnKind kind, std::vector<Cell> cells) {
    ColumnInfo const info{.id = "v", .kind = kind, .comparator = {}, .format = {}};
    VectorSource source{{info}};
    std::vector<std::vector<Cell>> rows;
    for (auto& cell : cells) {
        rows.push_back({std::move(cell)});
    }
    auto const count = rows.size();
    source.setRows(intKeys(count), std::move(rows));
    auto keys = KeyColumn::forColumn(info, Services{}, source.snapshot()->rowCount());
    buildKeys(*source.snapshot(), 0, info, Services{}, 0, source.snapshot()->rowCount(), keys);
    return keys.states;
}

std::vector<Cell> columnOf(GenericPage const& page, std::size_t column) {
    std::vector<Cell> out;
    for (auto const& row : page.rows) {
        out.push_back(row.at(column));
    }
    return out;
}

// One column "v" of `kind` holding `cells`: the page's JSON, written from a VectorSource, and that JSON read back.
struct OneColumn {
    std::string json;
    GenericPage read;
};

OneColumn throughTheWire(ColumnKind kind, std::vector<Cell> cells) {
    VectorSource source{{ColumnInfo{.id = "v", .kind = kind, .comparator = {}, .format = {}}}};
    std::vector<std::vector<Cell>> rows;
    for (auto& cell : cells) {
        rows.push_back({std::move(cell)});
    }
    auto const count = rows.size();
    source.setRows(intKeys(count), std::move(rows));
    std::vector<std::string> const columns{"v"};
    auto json = toJson(writeGenericPage(pageOf(count), source, columns));
    auto read = readGenericPage(json, describedOf({column("v", kind)}));
    REQUIRE(read.has_value());
    return OneColumn{.json = std::move(json), .read = std::move(*read)};
}

TableDescription eightColumns() {
    return describedOf({column("id", ColumnKind::Integer), column("name", ColumnKind::Text),
                        column("net", ColumnKind::Decimal), column("share", ColumnKind::Decimal),
                        column("dose", ColumnKind::Quantity), column("ratio", ColumnKind::Number),
                        column("flag", ColumnKind::Bool), column("on", ColumnKind::Date)});
}

std::shared_ptr<VectorSource> eightColumnRows() {
    auto source = std::make_shared<VectorSource>(columnInfos(eightColumns()));
    std::vector<std::vector<Cell>> rows{
        {Cell{std::int64_t{101}}, Cell{std::string{"Alpha"}}, Cell{dec("12.50")}, Cell{dec("0.003")},
         Cell{QuantityCell{.amount = Rational{5, DecimalPlaces{0}},
                           .toCanonical = Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}}},
         Cell{0.1}, Cell{true}, Cell{std::int64_t{19'000}}},
        {Cell{std::int64_t{102}}, Cell{std::string{"Beta"}}, Cell{}, Cell{}, Cell{Rational{250, DecimalPlaces{0}}},
         Cell{2.5}, Cell{false}, Cell{}}};
    source->setRows({RowId{std::int64_t{101}}, RowId{std::int64_t{102}}}, std::move(rows));
    return source;
}

}  // namespace

TEST_CASE("description: equal descriptions have equal revisions", "[table][description]") {
    TableDescription base{.id = "designs",
                          .label = "Designs",
                          .columns = {TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}, .label = "Id"},
                                      TableColumn{.info = {.id = "name", .kind = ColumnKind::Text}, .label = "Name"}},
                          .key = "id"};
    auto const a = describe(base);
    auto const b = describe(base);
    CHECK(a.revision.size() == 16);
    CHECK(a.revision == b.revision);
    CHECK(describe(a).revision == a.revision);  // Mutation: hashing with the old revision in
    auto relabelled = base;
    relabelled.columns[1].label = "Title";
    CHECK(describe(relabelled).revision != a.revision);  // Mutation: hashing the columns' ids only
    auto hidden = base;
    hidden.columns[1].shown = false;
    CHECK(describe(hidden).revision != a.revision);
}

TEST_CASE("generic page: every cell kind round-trips", "[table][description]") {
    auto const description = eightColumns();
    auto const source = eightColumnRows();
    auto const snapshot = source->snapshot();

    std::vector<std::string> const five{"id", "name", "net", "share", "dose"};
    auto page = writeGenericPage(pageOf(2, 9093), *source, five);
    page.revision = "9f2c";
    auto const json = toJson(page);
    CHECK(json == R"({"revision":"9f2c","columns":["id","name","net","share","dose"],"keys":[101,102],)"
                  R"("rows":[[101,"Alpha",[1250,100],[3,1000],[5,1,[1,1000]]],[102,"Beta",null,null,[250,1]]],)"
                  R"("total":9093,"totalKind":"exact","offset":0})");
    auto const read = readGenericPage(json, description);
    REQUIRE(read.has_value());
    CHECK(read->keys == page.keys);
    CHECK(read->columns == page.columns);
    CHECK(read->total == page.total);
    CHECK(read->offset == page.offset);
    CHECK(read->revision == "9f2c");
    REQUIRE(read->rows.size() == 2);
    for (std::size_t row = 0; row < 2; ++row) {
        REQUIRE(read->rows[row].size() == five.size());
        for (std::size_t column = 0; column < five.size(); ++column) {
            INFO("row " << row << ", column " << five[column]);
            CHECK(sameCell(read->rows[row][column], page.rows[row][column]));
        }
    }

    std::vector<std::string> all;
    for (auto const& info : source->columns()) {
        all.push_back(info.id);
    }
    auto const wide = writeGenericPage(pageOf(2), *source, all);
    auto const wideRead = readGenericPage(toJson(wide), description);
    REQUIRE(wideRead.has_value());
    for (std::size_t row = 0; row < 2; ++row) {
        for (std::size_t column = 0; column < all.size(); ++column) {
            INFO("row " << row << ", column " << all[column]);
            CHECK(sameCell(wideRead->rows[row][column], snapshot->cell(row, column)));
        }
    }
    CHECK(sameCell(wideRead->rows[0][5], Cell{0.1}));
    CHECK(sameCell(wideRead->rows[1][5], Cell{2.5}));
    CHECK(sameCell(wideRead->rows[0][6], Cell{true}));
    CHECK(sameCell(wideRead->rows[0][7], Cell{std::int64_t{19'000}}));
    // Mutation: writing a Rational as glaze's {"num","den","dp"} object.
    // Mutation: reading [1250, 100] with DecimalPlaces{kMaxDecimalPlaces} (operator== still holds; sameCell fails).
}

TEST_CASE("generic page: a cell that does not read as its column's kind is invalid", "[table][description]") {
    auto const json =
        std::string{R"({"revision":"","columns":["v"],"keys":[1,2,3,4,5,6],)"
                    R"("rows":[["abc"],[2.5],[[1,0]],[{"x":1}],[2.0],[7]],"totalKind":"exact","offset":0})"};
    auto const integer = readGenericPage(json, describedOf({column("v", ColumnKind::Integer)}));
    REQUIRE(integer.has_value());
    using enum CellState;
    CHECK(statesOf(ColumnKind::Integer, columnOf(*integer, 0)) ==
          std::vector<CellState>{Invalid, Invalid, Invalid, Invalid, Invalid, Valid});
    auto const text = readGenericPage(json, describedOf({column("v", ColumnKind::Text)}));
    REQUIRE(text.has_value());
    CHECK(statesOf(ColumnKind::Text, columnOf(*text, 0)) ==
          std::vector<CellState>{Valid, Valid, Valid, Valid, Valid, Valid});

    // Server and client agree on a double the server holds in an integer or exact column.
    for (auto const kind : {ColumnKind::Integer, ColumnKind::Decimal}) {
        INFO("kind " << static_cast<int>(kind));
        CHECK(statesOf(kind, {Cell{2.0}}) == std::vector<CellState>{Invalid});
        auto const wire = throughTheWire(kind, {Cell{2.0}});
        CHECK(wire.json.find("[[2.0]]") != std::string::npos);
        CHECK(std::holds_alternative<double>(wire.read.rows.at(0).at(0)));
        CHECK(statesOf(kind, columnOf(wire.read, 0)) == std::vector<CellState>{Invalid});
    }
    auto const number = throughTheWire(ColumnKind::Number, {Cell{2.0}});
    CHECK(number.json.find("[[2.0]]") != std::string::npos);
    CHECK(sameCell(number.read.rows.at(0).at(0), Cell{2.0}));
    CHECK(statesOf(ColumnKind::Number, columnOf(number.read, 0)) == std::vector<CellState>{Valid});
    // Mutation: cellFromJson parsing "abc"-like strings for the column's kind on read (2.5 read as Integer 2).
    // Mutation: writing a double as glaze does (2.0 becomes 2, an int64 the client keys Valid).
}

TEST_CASE("generic page: numbers at the edges cross the wire unchanged", "[table][description]") {
    auto const huge = glz::write_json(1e308).value();
    auto const number =
        throughTheWire(ColumnKind::Number,
                       {Cell{std::numeric_limits<double>::quiet_NaN()}, Cell{std::numeric_limits<double>::infinity()},
                        Cell{-std::numeric_limits<double>::infinity()}, Cell{-0.0}, Cell{1e308}});
    CHECK(number.json.find(R"([["nan"],["inf"],["-inf"],[-0.0],[)" + huge + "]]") != std::string::npos);
    auto const reals = columnOf(number.read, 0);
    REQUIRE(reals.size() == 5);
    REQUIRE(std::ranges::all_of(reals, [](Cell const& cell) { return std::holds_alternative<double>(cell); }));
    CHECK(std::isnan(std::get<double>(reals[0])));
    CHECK(std::get<double>(reals[1]) == std::numeric_limits<double>::infinity());
    CHECK(std::get<double>(reals[2]) == -std::numeric_limits<double>::infinity());
    CHECK(std::get<double>(reals[3]) == 0.0);
    CHECK(std::signbit(std::get<double>(reals[3])));
    CHECK(std::get<double>(reals[4]) == 1e308);

    auto const integer = throughTheWire(ColumnKind::Integer, {Cell{kInt64Min}, Cell{kInt64Max}});
    CHECK(sameCell(integer.read.rows.at(0).at(0), Cell{kInt64Min}));
    CHECK(sameCell(integer.read.rows.at(1).at(0), Cell{kInt64Max}));

    Rational const thirds{Numerator{kInt64Max}, Denominator{3}, DecimalPlaces{2}};
    Rational const scaled{Numerator{kInt64Max / 10}, Denominator{1}, DecimalPlaces{18}};
    auto const exact = throughTheWire(ColumnKind::Decimal, {Cell{thirds}, Cell{scaled}});
    CHECK(exact.json.find("[[9223372036854775807,3]]") != std::string::npos);
    CHECK(exact.json.find("[[922337203685477580,1]]") != std::string::npos);
    CHECK(exact.read.rows.at(0).at(0) == Cell{thirds});
    CHECK(exact.read.rows.at(1).at(0) == Cell{scaled});
    // Mutation: writing a non-finite double with glaze's default (the page fails to write, or reads back empty).
}

TEST_CASE("generic page: a malformed reply is refused and a malformed cell is invalid", "[table][description]") {
    auto const description = describedOf(
        {column("a", ColumnKind::Integer), column("b", ColumnKind::Integer), column("c", ColumnKind::Integer)});
    auto const refused = [&](std::string const& json) {
        auto const read = readGenericPage(json, description);
        REQUIRE_FALSE(read.has_value());
        return read.error();
    };
    CHECK(refused(R"({"columns":["a","b","c"],"keys":[1],"rows":[[1,2]]})").code == TableErrorCode::InvalidValue);
    CHECK(refused(R"({"columns":["a"],"keys":[1,2],"rows":[[1]]})").code == TableErrorCode::InvalidValue);
    auto const unknown = refused(R"({"columns":["nope"],"keys":[],"rows":[]})");
    CHECK(unknown.code == TableErrorCode::UnknownColumn);
    CHECK(unknown.column == "nope");
    CHECK(refused("not json").code == TableErrorCode::InvalidValue);

    auto const kept = readGenericPage(R"({"columns":["a","b"],"keys":[1,2],"rows":[[1,[1,0]],[2,3]]})", description);
    REQUIRE(kept.has_value());
    using enum CellState;
    CHECK(statesOf(ColumnKind::Integer, columnOf(*kept, 0)) == std::vector<CellState>{Valid, Valid});
    CHECK(statesOf(ColumnKind::Integer, columnOf(*kept, 1)) == std::vector<CellState>{Invalid, Valid});
    // Mutation: readGenericPage padding a short row with empty cells.
}

TEST_CASE("table query: columns round-trip and are bounded", "[table][query]") {
    TableQuery const query{.columns = std::vector<std::string>{"a", "b"}};
    auto const json = glz::write_json(query);
    REQUIRE(json.has_value());
    CHECK(json->find(R"("columns":["a","b"])") != std::string::npos);
    TableQuery back;
    REQUIRE_FALSE(glz::read_json(back, *json));
    CHECK(back.columns == query.columns);
    TableQuery bare;
    REQUIRE_FALSE(glz::read_json(bare, R"({"sort":[],"filters":{},"page":{"offset":0,"limit":10}})"));
    CHECK(bare.columns == std::nullopt);

    std::vector<ColumnInfo> const columns{tabletest::col("id", ColumnKind::Integer),
                                          tabletest::col("a", ColumnKind::Text),
                                          tabletest::col("b", ColumnKind::Text)};
    auto const mayRead = [](std::string_view id) { return id != "b"; };
    auto const check = [&](std::vector<std::string> asked) {
        return validate(TableQuery{.columns = std::move(asked)}, columns, QueryLimits{}, mayRead);
    };
    auto const zz = check({"zz"});
    REQUIRE_FALSE(zz.has_value());
    CHECK(zz.error().code == TableErrorCode::UnknownColumn);
    CHECK(zz.error().column == "zz");
    auto const b = check({"b"});
    REQUIRE_FALSE(b.has_value());
    CHECK(b.error().code == TableErrorCode::ColumnNotReadable);
    CHECK(b.error().column == "b");
    std::vector<std::string> many;
    for (int i = 0; i < 513; ++i) {
        many.push_back("c" + std::to_string(i));
    }
    auto const tooMany = check(many);
    REQUIRE_FALSE(tooMany.has_value());
    CHECK(tooMany.error().code == TableErrorCode::LimitExceeded);
    auto const copies = check(std::vector<std::string>(513, "a"));
    REQUIRE_FALSE(copies.has_value());
    CHECK(copies.error().code == TableErrorCode::LimitExceeded);
    auto const none = check({});
    REQUIRE(none.has_value());
    REQUIRE(none->columns.has_value());
    CHECK(none->columns->empty());
    // Mutation: validate ignoring `columns`.
}

TEST_CASE("table query: a columns list is answered in the description's order, once each", "[table][query]") {
    auto const infos = columnInfos(bench::wideDescription("small", 30));
    TableQuery const asked{.columns = std::vector<std::string>{"t000", "id", "t000", "i001"}};
    auto const ordered = validate(asked, infos);
    REQUIRE(ordered.has_value());
    CHECK(ordered->columns == std::vector<std::string>{"id", "i001", "t000"});
    auto const every = validate(TableQuery{}, infos);
    REQUIRE(every.has_value());
    CHECK(every->columns == std::nullopt);
    auto const unknown = validate(TableQuery{.columns = std::vector<std::string>{"t000", "zz"}}, infos);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == TableErrorCode::UnknownColumn);
    CHECK(unknown.error().column == "zz");
    bench::WideTableSource const source{{.rows = 1'000, .columns = 30, .seed = 7}};
    CHECK(apply(source, asked).has_value());
    CHECK(detail::orderedColumns(infos, {"i001", "nope", "id"}) == std::vector<std::string>{"id", "i001"});

    auto const keysOnly = writeGenericPage(pageOf(200), source, std::vector<std::string>{});
    CHECK(keysOnly.keys.size() == 200);
    REQUIRE(keysOnly.rows.size() == 200);
    CHECK(std::ranges::all_of(keysOnly.rows, [](GenericRow const& row) { return row.empty(); }));
    // Mutation: validate returning `columns` as received (the query's order, the duplicate kept).
}

TEST_CASE("generic page: a page names only the asked columns", "[table][description]") {
    bench::WideTableSource const source{{.rows = 1'000, .columns = 300, .seed = 7}};
    std::vector<std::string> asked;
    for (std::size_t i = 100; i < 140; ++i) {
        asked.push_back(source.columns()[i].id);
    }
    auto const json = toJson(writeGenericPage(pageOf(200), source, asked));
    auto const read = readGenericPage(json, bench::wideDescription("wide", 300));
    REQUIRE(read.has_value());
    CHECK(read->columns == asked);
    REQUIRE(read->rows.size() == 200);
    CHECK(std::ranges::all_of(read->rows, [](GenericRow const& row) { return row.size() == 40; }));
    // Mutation: writeGenericPage ignoring `columns` (writes all 300).
}

TEST_CASE("generic page: a whole page fits an envelope", "[table][description]") {
    bench::WideTableSource const source{{.rows = 1'000, .columns = 300, .seed = 7}};
    std::vector<std::string> all;
    for (auto const& info : source.columns()) {
        all.push_back(info.id);
    }
    auto const page = writeGenericPage(pageOf(200), source, all);
    REQUIRE(page.rows.size() == 200);
    std::size_t cells = 0;
    for (auto const& row : page.rows) {
        CHECK(row.size() == 300);
        cells += row.size();
    }
    CHECK(cells == 60'000);
    auto const bytes = toJson(page).size();
    CHECK(bytes + 4'096 < morph::wire::kMaxEnvelopeBytes);
    CHECK(bytes > 600'000);  // ten bytes a cell: a floor no 300-column page is under
    // Mutation: writeGenericPage writing the first 40 columns whatever it is given (the row length and the floor
    // fail).
}

TEST_CASE("description: a 300-column description round-trips", "[table][description]") {
    auto d = bench::wideDescription("wide", 300);
    d.levels = {"recipe"};
    auto const described = describe(d);
    auto const json = glz::write_json(described);
    REQUIRE(json.has_value());
    TableDescription back;
    REQUIRE_FALSE(glz::read_json(back, *json));
    CHECK(back == described);
    CHECK(back.columns.size() == 300);
    REQUIRE(back.groups.size() == 2);
    CHECK(back.groups[0].id == "g3");
    CHECK(back.groups[0].columns.size() == 3);
    CHECK(back.groups[1].id == "g16");
    CHECK(back.groups[1].columns.size() == 16);
    CHECK(back.revision.size() == 16);
    // Mutation: a glz::meta for TableDescription that leaves out `groups` (the read description has none).
}
