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
//  The table registration cases name their own mutations at their ends.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <glaze/glaze.hpp>
#include <limits>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/changes.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/records.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_cache.hpp>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

#include "table/table_fixtures.hpp"
#include "table/wide_table_model.hpp"
#include "test_support.hpp"

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

TableColumn column(std::string const& id, ColumnKind kind) {
    return TableColumn{
        .info = {.id = id, .kind = kind, .comparator = {}, .format = {}, .level = {}}, .label = id, .group = {}};
}

TableDescription describedOf(std::vector<TableColumn> columns) {
    return describe(TableDescription{.id = "t", .label = "T", .columns = std::move(columns), .key = "id"});
}

std::vector<RowId> intKeys(std::size_t count, std::int64_t first = 1) {
    std::vector<RowId> keys;
    for (std::size_t i = 0; i < count; ++i) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `keys` is a fixture list built once; sizing it first would not change what the case checks
        keys.emplace_back(first + static_cast<std::int64_t>(i));
    }
    return keys;
}

PageRows pageOf(std::size_t rows, std::optional<std::int64_t> total = std::nullopt) {
    PageRows page;
    page.rows.resize(rows);
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
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
    ColumnInfo const info{.id = "v", .kind = kind, .comparator = {}, .format = {}, .level = {}};
    VectorSource source{{info}};
    std::vector<std::vector<Cell>> rows;
    for (auto& cell : cells) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
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
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
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
    VectorSource source{{ColumnInfo{.id = "v", .kind = kind, .comparator = {}, .format = {}, .level = {}}}};
    std::vector<std::vector<Cell>> rows;
    for (auto& cell : cells) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
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
    TableDescription const base{
        .id = "designs",
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
                  R"("total":9093,"totalKind":"exact","offset":0,"unloadedRows":[],"undecided":0,"awaiting":[]})");
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
        CHECK(wire.json.contains("[[2.0]]"));
        CHECK(std::holds_alternative<double>(wire.read.rows.at(0).at(0)));
        CHECK(statesOf(kind, columnOf(wire.read, 0)) == std::vector<CellState>{Invalid});
    }
    auto const number = throughTheWire(ColumnKind::Number, {Cell{2.0}});
    CHECK(number.json.contains("[[2.0]]"));
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
    CHECK(number.json.contains(R"([["nan"],["inf"],["-inf"],[-0.0],[)" + huge + "]]"));
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
    CHECK(exact.json.contains("[[9223372036854775807,3]]"));
    CHECK(exact.json.contains("[[922337203685477580,1]]"));
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

// Mutation: readGenericPage not checking `unloadedRows` and `indices` against the rows (each refused page reads).
TEST_CASE("generic page: unloaded rows and indices are bounded by the page's rows", "[table][description]") {
    auto const description = describedOf({column("a", ColumnKind::Integer)});
    auto const refused = [&](std::string const& members) {
        auto const read =
            readGenericPage(R"({"columns":["a"],"keys":[1,2],"rows":[[1],[2]],)" + members + "}", description);
        REQUIRE_FALSE(read.has_value());
        return read.error().code;
    };
    CHECK(refused(R"("unloadedRows":[{"index":2,"levels":["r"]}])") == TableErrorCode::InvalidValue);
    CHECK(refused(R"("unloadedRows":[{"index":-1,"levels":["r"]}])") == TableErrorCode::InvalidValue);
    CHECK(refused(R"("unloadedRows":[{"index":1,"levels":["r"]},{"index":0,"levels":["r"]}])") ==
          TableErrorCode::InvalidValue);
    CHECK(refused(R"("indices":[4])") == TableErrorCode::InvalidValue);
    CHECK(refused(R"("indices":[4,-1])") == TableErrorCode::InvalidValue);
    auto const kept =
        readGenericPage(R"({"columns":["a"],"keys":[1,2],"rows":[[1],[2]],"unloadedRows":[{"index":0,"levels":["r"]},)"
                        R"({"index":1,"levels":["r"]}],"indices":[4,9]})",
                        description);
    REQUIRE(kept.has_value());
    CHECK(kept->indices == std::vector<std::int64_t>{4, 9});
}

TEST_CASE("table query: columns round-trip and are bounded", "[table][query]") {
    TableQuery const query{.columns = std::vector<std::string>{"a", "b"}};
    auto const json = glz::write_json(query);
    REQUIRE(json.has_value());
    CHECK(json->contains(R"("columns":["a","b"])"));
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
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `many` is a fixture list built once; sizing it first would not change what the case checks
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
    ApplyOptions withUnits;
    withUnits.services.units = source.units();
    CHECK(apply(source, asked, withUnits).has_value());
    CHECK(detail::orderedColumns(infos, {"i001", "nope", "id"}) == std::vector<std::string>{"id", "i001"});

    auto const keysOnly = writeGenericPage(pageOf(200), source, std::vector<std::string>{}, source.units().get());
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
    auto const json = toJson(writeGenericPage(pageOf(200), source, asked, source.units().get()));
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
    auto const page = writeGenericPage(pageOf(200), source, all, source.units().get());
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

// ── Tables of a model on the bridge ─────────────────────────────────────────

namespace tabledesc {

// A record a table is derived from, and an action that changes it.
struct RtSource {
    std::int64_t id = 0;
};

struct RtTouch {};

class RtModel {
public:
    morph::table::ServedTable mine(morph::table::TableContext const& context);
    morph::table::ServedTable theirs(morph::table::TableContext const& context);
    bool execute(RtTouch const& action);
};

// Two models with a table function of the same name; neither is registered (see the uniqueness case).
class UqA {
public:
    morph::table::ServedTable rows(morph::table::TableContext const& context);
};

class UqB {
public:
    morph::table::ServedTable rows(morph::table::TableContext const& context);
};

// A typed table's rows.
struct TypedRow {
    std::int64_t id = 0;
    std::string name;
    morph::math::Rational price;
};

class TypedModel {
public:
    std::shared_ptr<std::vector<TypedRow> const> rows(morph::table::TableContext const& context);
};

// A typed table whose row type declares a level on one column.
class LevelModel {
public:
    std::shared_ptr<std::vector<tabletest::LevelRow> const> rows(morph::table::TableContext const& context);
};

// A table that throws on its first call, names a column its source lacks on its second, and is well formed after.
class FlakyModel {
public:
    morph::table::ServedTable flaky(morph::table::TableContext const& context);

private:
    int _calls = 0;
};

// A result that holds no ChangedRows.
struct NoTableResult {
    std::vector<int> ids;
};

// The threads a recorded snapshot was read on, and the thread the table's function last ran on.
struct WorkThreads {
    std::mutex mutex;
    std::set<std::thread::id> reads;
    std::thread::id served;

    void noteRead() {
        std::scoped_lock const lock{mutex};
        reads.insert(std::this_thread::get_id());
    }
};

inline WorkThreads& workThreads() {
    static WorkThreads shared;
    return shared;
}

// A snapshot that records the thread of every read of a cell.
class RecordingSnapshot final : public morph::table::RowSnapshot {
public:
    explicit RecordingSnapshot(std::shared_ptr<morph::table::RowSnapshot const> inner) : _inner{std::move(inner)} {}

    [[nodiscard]] std::size_t rowCount() const override { return _inner->rowCount(); }
    [[nodiscard]] RowId rowId(std::size_t row) const override { return _inner->rowId(row); }
    [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override {
        workThreads().noteRead();
        return _inner->cell(row, column);
    }
    void readColumn(std::size_t column, morph::table::ColumnSink& sink, std::size_t first,
                    std::size_t last) const override {
        workThreads().noteRead();
        _inner->readColumn(column, sink, first, last);
    }

private:
    std::shared_ptr<morph::table::RowSnapshot const> _inner;
};

// A source whose snapshots record their reads.
class RecordingSource final : public DataSource {
public:
    explicit RecordingSource(std::shared_ptr<DataSource const> inner) : _inner{std::move(inner)} {}

    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _inner->columns(); }
    [[nodiscard]] std::shared_ptr<morph::table::RowSnapshot const> snapshot() const override {
        return std::make_shared<RecordingSnapshot const>(_inner->snapshot());
    }
    void subscribe(morph::table::ChangeListener& /*listener*/) override {}
    void unsubscribe(morph::table::ChangeListener& /*listener*/) override {}

private:
    std::shared_ptr<DataSource const> _inner;
};

// A table read through a recording source.
class WorkModel {
public:
    morph::table::ServedTable work(morph::table::TableContext const& context);
};

}  // namespace tabledesc

BRIDGE_REGISTER_RECORD(tabledesc::RtSource, "RtSource")
BRIDGE_REGISTER_RECORD(tabledesc::TypedRow, "TypedRow")
BRIDGE_REGISTER_RECORD(tabletest::LevelRow, "LevelRow")
BRIDGE_REGISTER_MODEL(tabledesc::RtModel, "RtModel")
BRIDGE_REGISTER_TABLE(tabledesc::RtModel, "mine", &tabledesc::RtModel::mine, morph::table::rowScoped)
BRIDGE_REGISTER_TABLE(tabledesc::RtModel, "theirs", &tabledesc::RtModel::theirs,
                      morph::table::derivedFrom<tabledesc::RtSource>)
BRIDGE_REGISTER_ACTION(tabledesc::RtModel, tabledesc::RtTouch, "RtTouch")
BRIDGE_REGISTER_EFFECTS(tabledesc::RtTouch, morph::effects::changes<tabledesc::RtSource>)
BRIDGE_REGISTER_MODEL(tabledesc::TypedModel, "TypedModel")
BRIDGE_REGISTER_TABLE(tabledesc::TypedModel, "typedRows",
                      morph::table::typed<tabledesc::TypedRow>(&tabledesc::TypedModel::rows))
BRIDGE_REGISTER_MODEL(tabledesc::LevelModel, "LevelModel")
BRIDGE_REGISTER_TABLE(tabledesc::LevelModel, "levelRows",
                      morph::table::typed<tabletest::LevelRow>(&tabledesc::LevelModel::rows))
BRIDGE_REGISTER_MODEL(tabledesc::FlakyModel, "FlakyModel")
BRIDGE_REGISTER_TABLE(tabledesc::FlakyModel, "flaky", &tabledesc::FlakyModel::flaky)
BRIDGE_REGISTER_MODEL(tabledesc::WorkModel, "WorkModel")
BRIDGE_REGISTER_TABLE(tabledesc::WorkModel, "work", &tabledesc::WorkModel::work)

namespace tabledesc {

namespace {

// A one-column table over `rows` integer rows keyed 1..rows.
morph::table::ServedTable oneColumnTable(std::string id, std::int64_t rows) {
    morph::table::ServedTable served;
    served.description =
        describe(TableDescription{.id = std::move(id), .label = "T", .columns = {column("v", ColumnKind::Integer)}});
    auto source = std::make_shared<VectorSource>(columnInfos(served.description));
    std::vector<std::vector<Cell>> cells;
    for (std::int64_t row = 1; row <= rows; ++row) {
        cells.push_back({Cell{row * 10}});
    }
    source->setRows(intKeys(static_cast<std::size_t>(rows)), std::move(cells));
    served.source = std::move(source);
    return served;
}

std::vector<TypedRow> typedRows() {
    return {TypedRow{.id = 1, .name = "Alpha", .price = dec("12.50")},
            TypedRow{.id = 2, .name = "Beta", .price = dec("3.25")},
            TypedRow{.id = 3, .name = "Gamma", .price = dec("40.00")}};
}

}  // namespace

morph::table::ServedTable RtModel::mine(morph::table::TableContext const& context) {
    static_cast<void>(context);
    return oneColumnTable("mine", 3);
}

morph::table::ServedTable RtModel::theirs(morph::table::TableContext const& context) {
    static_cast<void>(context);
    return oneColumnTable("theirs", 3);
}

bool RtModel::execute(RtTouch const& action) {
    static_cast<void>(action);
    return true;
}

morph::table::ServedTable UqA::rows(morph::table::TableContext const& context) {
    static_cast<void>(context);
    return oneColumnTable("rows", 1);
}

morph::table::ServedTable UqB::rows(morph::table::TableContext const& context) {
    static_cast<void>(context);
    return oneColumnTable("rows", 2);
}

std::shared_ptr<std::vector<TypedRow> const> TypedModel::rows(morph::table::TableContext const& context) {
    static_cast<void>(context);
    return std::make_shared<std::vector<TypedRow> const>(typedRows());
}

std::shared_ptr<std::vector<tabletest::LevelRow> const> LevelModel::rows(morph::table::TableContext const& context) {
    static_cast<void>(context);
    return std::make_shared<std::vector<tabletest::LevelRow> const>(tabletest::levelRows(3));
}

morph::table::ServedTable FlakyModel::flaky(morph::table::TableContext const& context) {
    static_cast<void>(context);
    ++_calls;
    if (_calls == 1) {
        throw std::runtime_error{"the flaky table failed"};
    }
    auto served = oneColumnTable("flaky", 3);
    if (_calls == 2) {
        served.description.columns.push_back(column("ghost", ColumnKind::Text));
    }
    return served;
}

morph::table::ServedTable WorkModel::work(morph::table::TableContext const& context) {
    static_cast<void>(context);
    {
        std::scoped_lock const lock{workThreads().mutex};
        workThreads().served = std::this_thread::get_id();
    }
    auto served = oneColumnTable("work", 50);
    served.source = std::make_shared<RecordingSource const>(std::move(served.source));
    return served;
}

}  // namespace tabledesc

namespace {

// A pool that records the threads its tasks run on.
class RecordingPool final : public morph::exec::IExecutor {
public:
    void post(std::function<void()> task) override {
        _pool.post([this, task = std::move(task)] {
            {
                std::scoped_lock const lock{_mutex};
                _threads.insert(std::this_thread::get_id());
            }
            task();
        });
    }

    [[nodiscard]] bool isSerial() const noexcept override { return false; }

    std::set<std::thread::id> threads() {
        std::scoped_lock const lock{_mutex};
        return _threads;
    }

private:
    std::mutex _mutex;
    std::set<std::thread::id> _threads;
    morph::exec::ThreadPoolExecutor _pool{2};
};

std::vector<RowId> keysOfApply(DataSource const& source, TableQuery const& query) {
    ApplyOptions options;
    options.services.units = bench::wideUnits();
    auto const page = apply(source, query, options);
    REQUIRE(page.has_value());
    auto const snapshot = source.snapshot();
    std::vector<RowId> out;
    for (auto const row : page->rows) {
        out.push_back(snapshot->rowId(row));
    }
    return out;
}

std::vector<std::string> idsOf(TableDescription const& description) {
    std::vector<std::string> out;
    for (auto const& entry : description.columns) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
        out.push_back(entry.info.id);
    }
    return out;
}

morph::effects::detail::DeclaredEffect updatesOf(std::type_index record) {
    return {.kind = morph::effects::Kind::Updates, .record = record};
}

bool holdsProblem(std::function<void()> const& resolve, std::string const& problem) {
    try {
        resolve();
    } catch (morph::model::EffectsRegistrationError const& error) {
        return std::ranges::find(error.problems(), problem) != error.problems().end();
    }
    return false;
}

std::vector<std::string> keyTexts(morph::effects::RecordChange const& change) {
    std::vector<std::string> out;
    if (change.keys) {
        for (auto const& key : *change.keys) {
            out.push_back(key.str);
        }
    }
    return out;
}

morph::effects::RecordChange const* changeOf(morph::effects::Changes const& changes, std::string_view record) {
    auto const found = std::ranges::find(changes.records, record, &morph::effects::RecordChange::record);
    return found == changes.records.end() ? nullptr : &*found;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization, cert-err58-cpp): `kRanOn` is a test constant built from literals; if building it threw, the run would fail at load, which is what a broken fixture should do
morph::effects::InstanceChange const kRanOn{.modelType = "WideTableModel", .instance = 1, .primary = {}};

}  // namespace

TEST_CASE("table registration: a model with no row type serves a table", "[table][registration]") {
    bench::WideTableModel::configure({.rows = 2'000, .columns = 300, .seed = 7, .loadDelay = {}},
                                     {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}});
    morph::session::Context const session{};
    TableContext const context{.session = session};
    bench::WideTableModel model;
    auto const served = detail::pinned(model.small(context));
    TableQuery query;
    query.sort = {SortKey{.column = "i001", .dir = SortDirection::Ascending}};
    query.filters.columns["t001"].include.push_back(FilterEntry{.contains = "a"});
    query.page = PageRequest{.offset = 0, .limit = 10};

    auto const page = detail::answerQuery(served, query);
    REQUIRE(page.has_value());
    bench::WideTableSource const fresh{{.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}}};
    auto const expected = keysOfApply(fresh, query);
    REQUIRE(expected.size() == 10);
    CHECK(page->keys == expected);
    std::vector<std::string> layout;
    for (auto const& entry : bench::wideColumns(30)) {
        layout.push_back(entry.info.id);
    }
    CHECK(page->columns == layout);
    CHECK(page->revision == describe(served.description).revision);
    CHECK_FALSE(page->revision.empty());
    // Spec row "A table from a description", server half. Mutation: answerQuery ignoring query.sort.
}

TEST_CASE("table registration: two tables of one model are two records and two actions", "[table][registration]") {
    for (std::string_view const name : {"WideTableModel.wide", "WideTableModel.small", "WideTableModel.scoped"}) {
        INFO(name);
        auto const* const record = morph::effects::recordNamed(name);
        REQUIRE(record != nullptr);
        CHECK(record->keyField == std::optional<std::string>{"key"});
        CHECK(record->table);
    }
    using morph::effects::ActionRecord;
    CHECK(morph::effects::recordsOf("Table.wide") ==
          std::vector<ActionRecord>{
              ActionRecord{.record = "WideTableModel.wide", .keyField = "key", .paths = {}, .opaque = true}});
    CHECK(morph::effects::actionsHolding("WideTableModel.wide") ==
          std::vector<std::string>{"EditWideRow", "Table.wide"});
    CHECK(morph::effects::actionsHolding("WideTableModel.small") ==
          std::vector<std::string>{"EditWideRow", "Table.small"});
    using morph::effects::Effect;
    using morph::effects::Kind;
    CHECK(morph::effects::effectsOf("EditWideRow") ==
          std::vector<Effect>{{.kind = Kind::Updates, .record = "WideTableModel.wide"},
                              {.kind = Kind::Updates, .record = "WideTableModel.small"}});
    CHECK(morph::effects::effectsOf("ReplaceWideRows") ==
          std::vector<Effect>{{.kind = Kind::Changes, .record = "WideTableModel.wide"},
                              {.kind = Kind::Changes, .record = "WideTableModel.small"}});
    // Mutation: one record for the model (the record named by the model alone): the second registration conflicts.
}

TEST_CASE("changed rows: the walk reports the table at its rows member", "[table][registration]") {
    using morph::model::detail::ResultSlot;
    auto const slots = morph::model::detail::walkResult<bench::EditWideRowResult>();
    auto const holds = [&](std::type_index type, std::string_view path) {
        return std::ranges::any_of(slots,
                                   [&](ResultSlot const& slot) { return slot.type == type && slot.path == path; });
    };
    CHECK(holds(typeid(TableOf<&bench::WideTableModel::wide>), "wide.rows"));
    CHECK(holds(typeid(TableOf<&bench::WideTableModel::small>), "small.rows"));

    auto const changes = morph::effects::deriveChanges(
        "EditWideRow", R"({"wide":{"rows":[{"key":5}]},"small":{"rows":[{"key":5}]}})", kRanOn);
    auto const* const wide = changeOf(changes, "WideTableModel.wide");
    auto const* const small = changeOf(changes, "WideTableModel.small");
    REQUIRE(wide != nullptr);
    REQUIRE(small != nullptr);
    CHECK(keyTexts(*wide) == std::vector<std::string>{"5"});
    CHECK(keyTexts(*small) == std::vector<std::string>{"5"});
    // Mutation: the walker without the heldRecord rule (the registry then refuses EditWideRow's updates).
}

TEST_CASE("changed rows: a changed key names both keys, and a failed row neither", "[table][registration][keys]") {
    auto const* const record = morph::effects::recordNamed("WideTableModel.wide");
    REQUIRE(record != nullptr);
    CHECK(record->previousKeyField == std::optional<std::string>{"from"});
    auto const changes = morph::effects::deriveChanges(
        "EditWideRow",
        R"({"wide":{"rows":[{"key":10,"from":2},{"key":7}],"failed":[{"key":4,"message":"Locked"}]},)"
        R"("small":{"rows":[]}})",
        kRanOn);
    auto const* const wide = changeOf(changes, "WideTableModel.wide");
    REQUIRE(wide != nullptr);
    auto keys = keyTexts(*wide);
    std::ranges::sort(keys);
    CHECK(keys == std::vector<std::string>{"10", "2", "7"});
    morph::table::ChangedRows<&bench::WideTableModel::wide> read;
    REQUIRE_FALSE(
        glz::read_json(read, R"({"rows":[{"key":10,"from":2},{"key":7}],"failed":[{"key":4,"message":"Locked"}]})"));
    REQUIRE(read.rows.size() == 2);
    CHECK(read.rows[0].from == std::optional<morph::table::RowId>{std::int64_t{2}});
    CHECK_FALSE(read.rows[1].from.has_value());
    CHECK(read.failed == std::vector<morph::table::FailedRow>{{.key = std::int64_t{4}, .message = "Locked"}});
    CHECK(glz::write_json(read.rows[1]).value_or("") == R"({"key":7})");
}
// Mutation: the walk ignoring `from`, which leaves the old row's watchers untold (keys {10, 7}).

TEST_CASE("changed rows: updatesTable without its ChangedRows is refused when the registries resolve",
          "[table][registration]") {
    using morph::model::detail::ActionResult;
    using morph::model::detail::walkResult;
    auto const* const record = morph::effects::recordNamed("WideTableModel.wide");
    REQUIRE(record != nullptr);
    morph::effects::detail::EffectsMap declared;
    morph::effects::detail::addEffects(declared, "Bad", {updatesOf(typeid(TableOf<&bench::WideTableModel::wide>))});
    std::string const problem =
        "action `Bad` declares updates on record `WideTableModel.wide`, but its result holds no `WideTableModel.wide`";

    morph::model::detail::ActionResultSlotMap bad;
    bad.emplace("Bad", ActionResult{.action = typeid(tabledesc::NoTableResult),
                                    .resultSlots = walkResult<tabledesc::NoTableResult>()});
    CHECK(holdsProblem([&] { static_cast<void>(morph::effects::detail::resolveForTesting({*record}, bad, declared)); },
                       problem));

    morph::model::detail::ActionResultSlotMap good;
    good.emplace("Bad", ActionResult{.action = typeid(bench::EditWideRowResult),
                                     .resultSlots = walkResult<bench::EditWideRowResult>()});
    auto const resolved = morph::effects::detail::resolveForTesting({*record}, good, declared);
    auto const held = resolved.recordsOf("Bad");
    REQUIRE(held.size() == 1);
    CHECK(held[0].record == "WideTableModel.wide");
    CHECK(held[0].paths == std::vector<std::string>{"wide.rows"});
    // Mutation: the walker without the heldRecord rule (the second call throws the same problem).
}

TEST_CASE("table registration: one table id in two models is refused when the registries resolve",
          "[table][registration]") {
    // Two query actions under one id are two action types: what registerActionResultOnce lists as a conflict.
    static_assert(!std::is_same_v<QueryTable<&tabledesc::UqA::rows>, QueryTable<&tabledesc::UqB::rows>>);
    morph::model::detail::ActionResultSlotMap slots;
    slots.emplace("Table.rows", morph::model::detail::ActionResult{.action = typeid(QueryTable<&tabledesc::UqA::rows>),
                                                                   .resultSlots = {}});
    CHECK(holdsProblem(
        [&] { static_cast<void>(morph::effects::detail::resolveForTesting({}, slots, {}, {"Table.rows"})); },
        "action `Table.rows` is registered by two action types; give each its own id"));
    // This executable's tables all have different ids, so its process registry holds no conflict.
    CHECK(morph::model::detail::actionResultConflicts().empty());
    // Mutation: QueryTable keyed on the table's id text instead of its function (the static_assert fails: two
    // models' tables would share one action type and a second table "rows" would answer with the first's rows).
}

TEST_CASE("table registration: rowScoped and derivedFrom are the record's", "[table][registration]") {
    auto const* const mine = morph::effects::recordNamed("RtModel.mine");
    REQUIRE(mine != nullptr);
    CHECK(mine->privateKeys);
    auto const* const theirs = morph::effects::recordNamed("RtModel.theirs");
    REQUIRE(theirs != nullptr);
    CHECK(theirs->derivedFrom == std::vector<std::type_index>{typeid(tabledesc::RtSource)});
    CHECK(theirs->keyField == std::optional<std::string>{"key"});
    CHECK_FALSE(theirs->privateKeys);
    auto const changes = morph::effects::deriveChanges("RtTouch", "{}", kRanOn);
    auto const* const change = changeOf(changes, "RtModel.theirs");
    REQUIRE(change != nullptr);
    CHECK_FALSE(change->keys.has_value());
    // Mutation: keeping "a derived record is keyless" for tables (the process registry fails to resolve).
}

TEST_CASE("columns by session: two principals get two descriptions", "[table][registration]") {
    bench::WideTableModel::configure({.rows = 2'000, .columns = 300, .seed = 7, .loadDelay = {}},
                                     {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}});
    bench::WideTableModel model;
    morph::session::Context const alice{.principal = "alice"};
    morph::session::Context const extended{.principal = "extended"};
    morph::session::Context const restricted{.principal = "restricted"};
    auto const aliceTable = model.scoped(TableContext{.session = alice});
    auto const extendedTable = model.scoped(TableContext{.session = extended});
    auto const restrictedTable = model.scoped(TableContext{.session = restricted});
    auto const aliceDescription = describe(aliceTable.description);
    auto const extendedDescription = describe(extendedTable.description);
    auto const restrictedDescription = describe(restrictedTable.description);
    CHECK(aliceDescription.columns.size() == 24);
    CHECK(extendedDescription.columns.size() == 30);
    CHECK(restrictedDescription.columns.size() == 17);
    CHECK(aliceDescription.revision != extendedDescription.revision);
    CHECK(aliceDescription.revision != restrictedDescription.revision);
    CHECK(extendedDescription.revision != restrictedDescription.revision);

    TableQuery query;
    query.page = PageRequest{.offset = 0, .limit = 5};
    auto const restrictedPage = detail::answerQuery(detail::pinned(restrictedTable), query);
    REQUIRE(restrictedPage.has_value());
    CHECK(restrictedPage->columns == idsOf(restrictedDescription));
    CHECK(restrictedPage->columns.size() == 17);

    query.columns = std::vector<std::string>{"d000"};
    auto const refused = detail::answerQuery(detail::pinned(restrictedTable), query);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::UnknownColumn);
    CHECK(refused.error().column == "d000");

    query.columns.reset();
    auto const alicePage = detail::answerQuery(detail::pinned(aliceTable), query);
    REQUIRE(alicePage.has_value());
    CHECK(alicePage->revision == aliceDescription.revision);

    // A description that lists i001 under a mayRead that refuses it: the page neither names nor carries it.
    auto narrowed = model.small(TableContext{.session = alice});
    narrowed.mayRead = [](std::string_view id) { return id != "i001"; };
    auto const narrowedPage = detail::answerQuery(detail::pinned(narrowed), query);
    REQUIRE(narrowedPage.has_value());
    CHECK(std::ranges::find(narrowedPage->columns, "i001") == narrowedPage->columns.end());
    CHECK(narrowedPage->columns.size() == 29);
    CHECK(std::ranges::all_of(narrowedPage->rows, [](GenericRow const& row) { return row.size() == 29; }));
    // Spec row "Columns by session", server half.
    // Mutation: answerQuery not filtering the absent-columns list by mayRead.
    // Mutation: answerQuery validating against the source's columns instead of the description's.
}

TEST_CASE("typed: a typed table writes the page the generic source writes", "[table][registration][threads]") {
    morph::session::Context const session{};
    TableContext const context{.session = session};
    tabledesc::TypedModel model;
    auto const typedServed = detail::serve<&tabledesc::TypedModel::rows>(model, context);
    auto const& description = typedServed.description;
    REQUIRE(description.columns.size() == 3);
    CHECK(description.columns[0].info.id == "id");
    CHECK(description.columns[0].info.kind == ColumnKind::Integer);
    CHECK(description.columns[1].info.id == "name");
    CHECK(description.columns[1].info.kind == ColumnKind::Text);
    CHECK(description.columns[2].info.id == "price");
    CHECK(description.columns[2].info.kind == ColumnKind::Decimal);
    CHECK(description.columns[0].label == "Id");
    CHECK(description.columns[1].label == "Name");
    CHECK(description.columns[2].label == "Price");
    CHECK(description.key == "id");

    ServedTable generic;
    generic.description = description;
    auto source = std::make_shared<VectorSource>(columnInfos(description));
    std::vector<std::vector<Cell>> cells;
    std::vector<RowId> keys;
    for (auto const& row : tabledesc::typedRows()) {
        keys.emplace_back(row.id);
        cells.push_back({Cell{row.id}, Cell{row.name}, Cell{row.price}});
    }
    source->setRows(std::move(keys), std::move(cells));
    generic.source = std::move(source);

    TableQuery query;
    query.sort = {SortKey{.column = "price", .dir = SortDirection::Descending}};
    query.page = PageRequest{.offset = 0, .limit = 2};
    auto const typedPage = detail::answerQuery(detail::pinned(typedServed), query);
    auto const genericPage = detail::answerQuery(detail::pinned(generic), query);
    REQUIRE(typedPage.has_value());
    REQUIRE(genericPage.has_value());
    CHECK(typedPage->keys == std::vector<RowId>{RowId{std::int64_t{3}}, RowId{std::int64_t{1}}});
    CHECK(toJson(*typedPage) == toJson(*genericPage));

    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<tabledesc::TypedModel> handler{bridge, &owner};
    auto const described =
        morph::testing::awaitValueOn(owner, handler.execute(DescribeTable<&tabledesc::TypedModel::rows>{}));
    CHECK(described == describe(description));
    // Spec row "The generic page", typed half.
    // Mutation: DescribeTable building the typed description its own way and dropping the labels.
    // Mutation: the typed layer reading cells in glaze's key order while the description lists reflected order
    // differently.
}

// Mutation: columnLevels() applied in RowsSource only, not in reflectedColumns.
TEST_CASE("typed: a row's column levels reach the description", "[table][registration][levels]") {
    morph::session::Context const session{};
    TableContext const context{.session = session};
    tabledesc::LevelModel model;
    auto const served = detail::serve<&tabledesc::LevelModel::rows>(model, context);
    auto const& description = served.description;
    auto const qty = std::ranges::find(description.columns, std::string{"qty"},
                                       [](TableColumn const& column) { return column.info.id; });
    REQUIRE(qty != description.columns.end());
    CHECK(qty->info.level == "stock");
    CHECK(description.levels == std::vector<std::string>{"stock"});
    for (auto const& column : description.columns) {
        CHECK((column.info.level.empty() || column.info.id == "qty"));
    }
    auto const sourceColumns = served.source->columns();
    auto const sourceQty = std::ranges::find(sourceColumns, std::string{"qty"}, &ColumnInfo::id);
    REQUIRE(sourceQty != sourceColumns.end());
    CHECK(sourceQty->level == "stock");
}

TEST_CASE("scoped: a scope's source reads the shared rows of its keys", "[table][registration]") {
    auto const base =
        std::make_shared<VectorSource>(std::vector<ColumnInfo>{tabletest::col("v", ColumnKind::Integer)});
    std::vector<std::vector<Cell>> cells;
    for (std::int64_t key = 1; key <= 5; ++key) {
        cells.push_back({Cell{key * 10}});
    }
    base->setRows(intKeys(5), std::move(cells));
    auto const scope = scoped(base, {RowId{std::int64_t{4}}, RowId{std::int64_t{2}}, RowId{std::int64_t{9}}});
    auto const before = scope->snapshot();
    REQUIRE(before->rowCount() == 2);
    CHECK(before->rowId(0) == RowId{std::int64_t{2}});
    CHECK(before->rowId(1) == RowId{std::int64_t{4}});
    CHECK(sameCell(before->cell(1, 0), Cell{std::int64_t{40}}));

    base->updateRow(3, {Cell{std::int64_t{99}}});
    auto const after = scope->snapshot();
    REQUIRE(after->rowCount() == 2);
    CHECK(sameCell(after->cell(1, 0), Cell{std::int64_t{99}}));
    // Mutation: scoped copying the rows when built (the update is not seen).
}

TEST_CASE("table action: a failing table function is an error reply and the model stays usable",
          "[table][registration][threads]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<tabledesc::FlakyModel> handler{bridge, &owner};
    using Query = QueryTable<&tabledesc::FlakyModel::flaky>;

    std::string thrown;
    try {
        static_cast<void>(morph::testing::awaitValueOn(owner, handler.execute(Query{})));
    } catch (std::exception const& error) {
        thrown = error.what();
    }
    CHECK(thrown == "the flaky table failed");

    std::optional<morph::model::ActionRefusal> refusal;
    try {
        static_cast<void>(morph::testing::awaitValueOn(owner, handler.execute(Query{})));
    } catch (morph::model::ActionRefusal const& error) {
        refusal = error;
    }
    REQUIRE(refusal.has_value());
    CHECK(refusal->code() == "table.unknownColumn");
    REQUIRE(refusal->args().size() == 1);
    CHECK(refusal->args()[0].name == "column");
    CHECK(refusal->args()[0].value == "ghost");

    auto const page = morph::testing::awaitValueOn(owner, handler.execute(Query{}));
    CHECK(page.keys.size() == 3);
    CHECK(page.columns == std::vector<std::string>{"v"});
    // Mutation: answerQuery writing the page column by column without checking the source first (a partial page).
}

TEST_CASE("table action: a server builds a table's reply on tableWork", "[table][registration][threads]") {
    auto const tableWork = std::make_shared<RecordingPool>();
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::ServerConfig config;
    config.tableWork = tableWork;
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool, std::move(config));
    auto const registered =
        morph::wire::decode(server->handleInline(morph::wire::encode(morph::wire::makeRegister("WorkModel"))));
    REQUIRE(registered.kind == "ok");
    {
        std::scoped_lock const lock{tabledesc::workThreads().mutex};
        tabledesc::workThreads().reads.clear();
    }

    morph::wire::Envelope execute;
    execute.kind = "execute";
    execute.callId = 7;
    execute.modelId = registered.modelId;
    execute.modelType = "WorkModel";
    execute.actionType = "Table.work";
    execute.body = R"({"sort":[{"column":"v","dir":"desc"}],"page":{"offset":0,"limit":4}})";
    auto const waiter = std::make_shared<morph::testing::WaitReply>();
    server->handle(morph::wire::encode(execute), [waiter](std::string const& reply) { (*waiter)(reply); });
    REQUIRE(waiter->await(std::chrono::seconds{30}));
    REQUIRE(waiter->env.kind == "ok");
    GenericPage page;
    REQUIRE_FALSE(glz::read_json(page, waiter->env.body));
    CHECK(page.keys == std::vector<RowId>{RowId{std::int64_t{50}}, RowId{std::int64_t{49}}, RowId{std::int64_t{48}},
                                          RowId{std::int64_t{47}}});

    std::scoped_lock const lock{tabledesc::workThreads().mutex};
    auto const& reads = tabledesc::workThreads().reads;
    auto const work = tableWork->threads();
    REQUIRE_FALSE(reads.empty());
    CHECK(std::ranges::all_of(reads, [&](std::thread::id id) { return work.contains(id); }));
    CHECK_FALSE(reads.contains(tabledesc::workThreads().served));
    // The in-process twin of the socket case "tables: a table's reply is built on tableWork".
    // Mutation: RemoteServer not installing the TableRuntimeScope (answerQuery runs inline on the strand).
}

namespace {

// A served table of three rows whose key column "id" holds each row's key, 0, 1 and 2; `keyed` is its
// capability for keyed queries.
ServedTable threeRowTable(bool keyed) {
    std::vector<std::vector<Cell>> cells;
    for (std::int64_t row = 0; row < 3; ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `cells` is a fixture list built once; sizing it first would not change what the case checks
        cells.push_back({Cell{row}});
    }
    auto description =
        TableDescription{.id = "t", .label = "T", .columns = {column("id", ColumnKind::Integer)}, .key = "id"};
    description.capabilities.keys = keyed;
    return detail::pinned(
        ServedTable{.description = describe(std::move(description)),
                    .source = tabletest::makeSource({tabletest::col("id", ColumnKind::Integer)}, std::move(cells)),
                    .levels = {},
                    .mayRead = {},
                    .scope = {},
                    .services = {},
                    .limits = {}});
}

}  // namespace

// Mutation: answerQuery ignoring capabilities.keys.
TEST_CASE("table: a keyed query to a table without keyed queries is unavailable", "[table][query][keyed]") {
    auto const served = threeRowTable(false);
    TableQuery keyed;
    keyed.keys = std::vector<RowId>{RowId{std::int64_t{1}}};
    auto const refused = detail::answerQuery(served, keyed);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::Unavailable);
    auto const page = detail::answerQuery(served, TableQuery{});
    REQUIRE(page.has_value());
    CHECK(page->keys.size() == 3);
    auto const answered = detail::answerQuery(threeRowTable(true), keyed);
    REQUIRE(answered.has_value());
    CHECK(answered->keys == std::vector<RowId>{RowId{std::int64_t{1}}});
    CHECK(answered->indices == std::vector<std::int64_t>{1});
}

// Mutation: an empty columns list read as absent.
TEST_CASE("table: an empty columns list gives a page of keys", "[table][query]") {
    TableQuery query;
    query.columns = std::vector<std::string>{};
    auto const page = detail::answerQuery(threeRowTable(true), query);
    REQUIRE(page.has_value());
    CHECK(page->columns.empty());
    CHECK(page->keys == std::vector<RowId>{RowId{std::int64_t{0}}, RowId{std::int64_t{1}}, RowId{std::int64_t{2}}});
    REQUIRE(page->rows.size() == 3);
    CHECK(std::ranges::all_of(page->rows, [](GenericRow const& row) { return row.empty(); }));
}

// Mutation: the reader requiring unloadedRows.
TEST_CASE("generic page: unloaded rows, undecided and indices round-trip", "[table][description]") {
    GenericPage page;
    page.revision = "r";
    page.keys = {RowId{std::int64_t{5}}, RowId{std::int64_t{9}}};
    page.rows = {GenericRow{}, GenericRow{}};
    page.unloadedRows = {UnloadedRow{.index = 1, .levels = {"recipe"}, .failed = {"recipe"}}};
    page.undecided = 40;
    page.awaiting = {"stock"};
    page.failed = {"recipe"};
    page.indices = {5, 9};
    auto const description = describedOf({column("id", ColumnKind::Integer)});
    auto const back = readGenericPage(toJson(page), description);
    REQUIRE(back.has_value());
    CHECK(back->unloadedRows == page.unloadedRows);
    CHECK(back->undecided == 40);
    CHECK(back->awaiting == page.awaiting);
    CHECK(back->failed == page.failed);
    CHECK(back->indices == page.indices);

    auto const loaded = readGenericPage(
        R"({"revision":"r","columns":[],"keys":[5],"rows":[[]],"total":1,"totalKind":"exact","offset":0})",
        description);
    REQUIRE(loaded.has_value());
    CHECK(loaded->unloadedRows.empty());
    CHECK(loaded->undecided == 0);
    CHECK(loaded->awaiting.empty());
    CHECK(loaded->failed.empty());
    CHECK(loaded->indices.empty());
}

// Mutation: the writer emitting a sentinel string for Unloaded.
TEST_CASE("generic page: an unloaded cell is null and named per level", "[table][description]") {
    using tabletest::levelCol;
    auto const source =
        tabletest::makeSource({tabletest::col("id", ColumnKind::Integer), levelCol("r", ColumnKind::Text, "recipe"),
                               levelCol("s", ColumnKind::Text, "stock")},
                              {{Cell{std::int64_t{10}}, Cell{std::string{"a"}}, Cell{std::string{"x"}}},
                               {Cell{std::int64_t{11}}, Cell{Unloaded{}}, Cell{Unloaded{}}}});
    std::vector<std::string> const named{"id", "r"};
    auto const page = writeGenericPage(pageOf(2), *source, named);
    CHECK(page.unloadedRows ==
          std::vector<UnloadedRow>{UnloadedRow{.index = 1, .levels = {"recipe"}, .failed = {}}});  // not "stock"
    auto const json = toJson(page);
    CHECK(json.contains(R"("rows":[[10,"a"],[11,null]])"));
    auto const back =
        readGenericPage(json, describedOf({column("id", ColumnKind::Integer), column("r", ColumnKind::Text)}));
    REQUIRE(back.has_value());
    REQUIRE(back->rows.size() == 2);
    CHECK(std::holds_alternative<std::monostate>(back->rows[1][1]));
    CHECK(back->unloadedRows == page.unloadedRows);
}

// Mutation: the page writers leaving nothing out (skip_if answering false: an empty failed and indices written).
TEST_CASE("generic page: an empty failed or indices is left out, and read either way", "[table][description]") {
    std::vector<UnloadedRow> const unloaded{UnloadedRow{.index = 0, .levels = {"recipe"}, .failed = {}}};
    GenericPage page;
    page.revision = "r";
    page.keys = {RowId{std::int64_t{5}}};
    page.rows = {GenericRow{}};
    page.unloadedRows = unloaded;
    CHECK(toJson(page) == R"({"revision":"r","columns":[],"keys":[5],"rows":[[]],"totalKind":"exact","offset":0,)"
                          R"("unloadedRows":[{"index":0,"levels":["recipe"]}],"undecided":0,"awaiting":[]})");
    Page<int> typed{.rows = {1}};
    typed.unloadedRows = unloaded;
    CHECK(glz::write_json(typed).value_or("") ==
          R"({"rows":[1],"offset":0,"totalKind":"exact","unloadedRows":[{"index":0,"levels":["recipe"]}],)"
          R"("undecided":0,"awaiting":[]})");
    typed.failed = {"recipe"};
    typed.indices = {3};
    CHECK(glz::write_json(typed).value_or("").contains(R"("failed":["recipe"],"indices":[3])"));

    auto const present = readGenericPage(
        R"({"revision":"r","columns":[],"keys":[5],"rows":[[]],"totalKind":"exact","offset":0,)"
        R"("unloadedRows":[{"index":0,"levels":["recipe"],"failed":[]}],"undecided":0,"awaiting":[],"failed":[],)"
        R"("indices":[]})",
        describedOf({column("id", ColumnKind::Integer)}));
    REQUIRE(present.has_value());
    CHECK(present->unloadedRows == unloaded);
    CHECK(present->failed.empty());
    CHECK(present->indices.empty());
    Page<int> back;
    REQUIRE_FALSE(glz::read_json(back, R"({"rows":[1],"offset":0,"totalKind":"exact","failed":[],"indices":[]})"));
    CHECK(back.rows == std::vector<int>{1});
}

namespace {

// A level that records what it is asked and holds nothing.
class RecordingLevel final : public LevelSource {
public:
    explicit RecordingLevel(std::string level) : _level{std::move(level)} {}
    [[nodiscard]] std::string_view level() const override { return _level; }
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return {}; }
    [[nodiscard]] std::shared_ptr<LevelSnapshot const> snapshot() const override {
        struct Empty final : LevelSnapshot {
            [[nodiscard]] LevelRead read(RowId const&, std::span<Cell>) const override { return LevelRead::Unloaded; }
            [[nodiscard]] std::uint64_t generation() const override { return 0; }
        };
        return std::make_shared<Empty const>();
    }
    std::expected<void, TableError> want(std::span<RowId const> keys, LevelUse, bool) override {
        wanted += keys.size();
        ++calls;
        return {};
    }
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): `wanted` and `calls` record what the source was asked, which the case compares
    std::size_t wanted = 0;
    std::size_t calls = 0;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    std::string _level;
};

}  // namespace

// Mutation: a level without a source read from the base source (every row matches and none is undecided).
TEST_CASE("level cache: a level no cache serves is unloaded and awaited", "[table][levels]") {
    auto stock = std::make_shared<RecordingLevel>("stock");
    auto description = describedOf({column("id", ColumnKind::Integer), column("r", ColumnKind::Text)});
    description.columns[1].info.level = "recipe";
    description.levels = {"recipe"};
    description = describe(std::move(description));
    std::vector<std::vector<Cell>> rows;
    for (std::int64_t row = 0; row < 5; ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
        rows.push_back({Cell{row}, Cell{std::string{"abc"}}});
    }
    auto const served = detail::pinned(ServedTable{
        .description = description,
        .source = tabletest::makeSource(
            {tabletest::col("id", ColumnKind::Integer), tabletest::col("r", ColumnKind::Text)}, std::move(rows)),
        .levels = {stock},
        .mayRead = {},
        .scope = {},
        .services = {},
        .limits = {}});
    TableQuery query;
    query.filters.columns.emplace(
        "r", ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.contains = "a"}}, .exclude = {}});
    auto const page = detail::answerQuery(served, query);
    REQUIRE(page.has_value());
    CHECK(page->rows.empty());
    CHECK(page->undecided == 5);
    CHECK(page->awaiting == std::vector<std::string>{"recipe"});
    CHECK(stock->calls == 0);
}

namespace tabledesc {

// A typed table whose function returns the same rows vector until `replace` makes a new one.
class CachedRowsModel {
public:
    std::shared_ptr<std::vector<TypedRow> const> rows(morph::table::TableContext const& context) {
        static_cast<void>(context);
        return _rows;
    }
    void replace() { _rows = std::make_shared<std::vector<TypedRow> const>(typedRows()); }

private:
    std::shared_ptr<std::vector<TypedRow> const> _rows = std::make_shared<std::vector<TypedRow> const>(typedRows());
};

}  // namespace tabledesc

BRIDGE_REGISTER_MODEL(tabledesc::CachedRowsModel, "CachedRowsModel")
BRIDGE_REGISTER_TABLE(tabledesc::CachedRowsModel, "cachedRows",
                      morph::table::typed<tabledesc::TypedRow>(&tabledesc::CachedRowsModel::rows))

// Mutation: find comparing snapshot pointers instead of identity() (three computations).
TEST_CASE("view cache: a typed table's view is cached across calls", "[table][views]") {
    tabledesc::CachedRowsModel model;
    morph::session::Context const session{};
    TableContext const context{.session = session};
    ViewCache views;
    detail::AnswerContext const answer{.views = &views, .record = "CachedRowsModel.cachedRows", .rowScoped = false};
    TableQuery query;
    query.sort = {SortKey{.column = "name", .dir = SortDirection::Ascending}};
    auto const ask = [&] {
        auto const page = detail::answerQuery(
            detail::pinned(detail::serve<&tabledesc::CachedRowsModel::rows>(model, context)), query, answer);
        REQUIRE(page.has_value());
    };
    ask();
    ask();
    CHECK(views.stats().computations == 1);
    CHECK(views.stats().hits == 1);
    model.replace();
    ask();
    CHECK(views.stats().computations == 2);
}

// Mutation: the check reading served.scope alone (the third case is refused).
TEST_CASE("view cache: a row-scoped table needs a scope", "[table][views]") {
    auto served = threeRowTable(true);
    detail::AnswerContext const scoped{.views = nullptr, .record = {}, .rowScoped = true};
    auto const refused = detail::answerQuery(served, TableQuery{}, scoped);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::SourceContract);
    served.scope = "p1";
    CHECK(detail::answerQuery(served, TableQuery{}, scoped).has_value());
    served.scope.clear();
    CHECK(detail::answerQuery(served, TableQuery{}, detail::AnswerContext{}).has_value());
}

namespace {

// A level whose rows are scripted: keys 0-3 loaded (`a`, `b`, `a`, `b`), 4-6 unloaded, 7-9 failed. It records every
// want and can refuse an order use.
class ScriptedLevel final : public LevelSource {
public:
    struct Wanted {
        std::vector<RowId> keys;
        LevelUse use = LevelUse::Display;
        bool retry = false;
    };

    [[nodiscard]] std::string_view level() const override { return "recipe"; }
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _columns; }
    [[nodiscard]] std::shared_ptr<LevelSnapshot const> snapshot() const override {
        struct Scripted final : LevelSnapshot {
            [[nodiscard]] LevelRead read(RowId const& key, std::span<Cell> out) const override {
                auto const at = std::get<std::int64_t>(key);
                if (at >= 7) {
                    return LevelRead::Failed;
                }
                if (at >= 4) {
                    return LevelRead::Unloaded;
                }
                out[0] = Cell{std::string{at % 2 == 0 ? "a" : "b"}};
                return LevelRead::Loaded;
            }
            [[nodiscard]] std::uint64_t generation() const override { return 1; }
        };
        return std::make_shared<Scripted const>();
    }
    std::expected<void, TableError> want(std::span<RowId const> keys, LevelUse use, bool retry) override {
        wanted.push_back(Wanted{.keys = {keys.begin(), keys.end()}, .use = use, .retry = retry});
        if (refuseOrder && use == LevelUse::Order) {
            return std::unexpected(TableError{.code = TableErrorCode::Unavailable, .column = {}, .message = "full"});
        }
        return {};
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): `wanted` records the requests and `refuseOrder` switches the refusal; the cases read and set both directly
    std::vector<Wanted> wanted;
    bool refuseOrder = false;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    std::vector<ColumnInfo> _columns{tabletest::levelCol("r", ColumnKind::Text, "recipe")};
};

ServedTable scriptedTable(std::shared_ptr<ScriptedLevel> const& level) {
    auto description = describedOf({column("id", ColumnKind::Integer), column("r", ColumnKind::Text)});
    description.columns[1].info.level = "recipe";
    description.levels = {"recipe"};
    std::vector<std::vector<Cell>> rows;
    for (std::int64_t row = 0; row < 10; ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
        rows.push_back({Cell{row}});
    }
    return detail::pinned(
        ServedTable{.description = describe(std::move(description)),
                    .source = tabletest::makeSource({tabletest::col("id", ColumnKind::Integer)}, std::move(rows)),
                    .levels = {level},
                    .mayRead = {},
                    .scope = {},
                    .services = {},
                    .limits = {}});
}

std::set<RowId> keySet(std::vector<RowId> const& keys) { return {keys.begin(), keys.end()}; }

}  // namespace

// Mutations: the order want leaving out the undecided rows; the wants not passing `retry`; `awaiting` read from the
// view's rows alone (the unloaded rows are all undecided).
TEST_CASE("table: a query's levels are wanted with the rows it needed", "[table][levels]") {
    auto const level = std::make_shared<ScriptedLevel>();
    TableQuery query;
    query.filters.columns.emplace(
        "r", ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.contains = "a"}}, .exclude = {}});
    query.columns = std::vector<std::string>{"id", "r"};
    query.page = PageRequest{.offset = 0, .limit = 1};
    query.retry = true;
    auto const page = detail::answerQuery(scriptedTable(level), query);
    REQUIRE(page.has_value());
    CHECK(page->keys == std::vector<RowId>{RowId{std::int64_t{0}}});
    CHECK(page->undecided == 6);
    CHECK(page->awaiting == std::vector<std::string>{"recipe"});
    CHECK(page->failed == std::vector<std::string>{"recipe"});
    REQUIRE(level->wanted.size() == 2);
    auto const& order = level->wanted[0];
    CHECK(order.use == LevelUse::Order);
    CHECK(keySet(order.keys) == std::set<RowId>{RowId{std::int64_t{0}}, RowId{std::int64_t{2}}, RowId{std::int64_t{4}},
                                                RowId{std::int64_t{5}}, RowId{std::int64_t{6}}, RowId{std::int64_t{7}},
                                                RowId{std::int64_t{8}}, RowId{std::int64_t{9}}});
    auto const& display = level->wanted[1];
    CHECK(display.use == LevelUse::Display);
    CHECK(display.keys == page->keys);
    CHECK(order.retry);
    CHECK(display.retry);
}

// Mutation: the order keys built for every view (the base-sorted view holds ten keys besides its rows).
TEST_CASE("view cache: a view keeps order keys only when its query has an order level", "[table][views][levels]") {
    auto const level = std::make_shared<ScriptedLevel>();
    ViewCache views;
    detail::AnswerContext const answer{.views = &views, .record = "T.rows", .rowScoped = false};
    TableQuery query;
    query.sort = {SortKey{.column = "id", .dir = SortDirection::Ascending}};
    query.columns = std::vector<std::string>{"id", "r"};
    REQUIRE(detail::answerQuery(scriptedTable(level), query, answer).has_value());
    CHECK(views.stats().bytes == std::size_t{4} * 10);  // the view's ten rows and nothing else
    query.sort = {SortKey{.column = "r", .dir = SortDirection::Ascending}};
    REQUIRE(detail::answerQuery(scriptedTable(level), query, answer).has_value());
    CHECK(views.stats().bytes >= (2UZ * 4 * 10) + (10 * sizeof(RowId)));  // the level-sorted view keeps its ten keys
}

// Mutation: a failed row read as merely unloaded (its `failed` left empty).
TEST_CASE("table: failed levels are read from the snapshots", "[table][levels]") {
    auto const level = std::make_shared<ScriptedLevel>();
    TableQuery query;
    query.columns = std::vector<std::string>{"id", "r"};
    auto const page = detail::answerQuery(scriptedTable(level), query);
    REQUIRE(page.has_value());
    REQUIRE(page->unloadedRows.size() == 6);
    for (auto const& row : page->unloadedRows) {
        CHECK(row.levels == std::vector<std::string>{"recipe"});
        CHECK(row.failed == (row.index >= 7 ? std::vector<std::string>{"recipe"} : std::vector<std::string>{}));
    }
    CHECK(page->awaiting.empty());  // no order level: nothing is sorted or filtered by `r`
}

// Mutation: answerQuery ignoring an order want's refusal (a page is returned).
TEST_CASE("table: an order use the level refuses refuses the query", "[table][levels]") {
    auto const level = std::make_shared<ScriptedLevel>();
    level->refuseOrder = true;
    TableQuery query;
    query.sort = {SortKey{.column = "r", .dir = SortDirection::Ascending}};
    query.columns = std::vector<std::string>{"id"};
    auto const refused = detail::answerQuery(scriptedTable(level), query);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::Unavailable);
    CHECK(refused.error().column == "r");
    CHECK(level->wanted.size() == 1);  // no display want follows a refusal
}

// Mutation: find not joining a running computation (each request computes; the first holds the gate until all have
// asked, so the others compute too).
TEST_CASE("view cache: concurrent requests through a server compute one view", "[table][views][threads]") {
    bench::WideTableModel::recipeGate().release();
    bench::WideTableModel::configure({.rows = 2'000, .columns = 300, .seed = 7, .loadDelay = {}},
                                     {.rows = 1'000, .columns = 30, .seed = 7, .loadDelay = {}});
    constexpr int kRequests = 8;
    morph::exec::ThreadPoolExecutor pool{4};
    morph::backend::ServerConfig config;
    config.tableWork = std::make_shared<morph::exec::ThreadPoolExecutor>(kRequests);
    std::shared_ptr<morph::backend::RemoteServer> server;
    // The first computation waits until every other request has joined it, or ten seconds when they compute
    // instead.
    config.views.onCompute = [&server] {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (server->viewStats().waits < kRequests - 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
    };
    server = std::make_shared<morph::backend::RemoteServer>(pool, std::move(config));
    server->attach(bench::WideTableModel::recipeCache());
    std::vector<std::shared_ptr<morph::testing::WaitReply>> waiters;
    for (int request = 0; request < kRequests; ++request) {
        // An instance each: one instance runs its executes one at a time, and these must overlap.
        auto const registered = morph::wire::decode(
            server->handleInline(morph::wire::encode(morph::wire::makeRegister(std::string{bench::kWideModelType}))));
        REQUIRE(registered.kind == "ok");
        morph::wire::Envelope execute;
        execute.kind = "execute";
        execute.callId = static_cast<std::uint64_t>(100 + request);
        execute.modelId = registered.modelId;
        execute.modelType = std::string{bench::kWideModelType};
        execute.actionType = "Table.small";
        execute.body = R"({"sort":[{"column":"i001","dir":"desc"}],"columns":["id","i001"],)"
                       R"("page":{"offset":0,"limit":20}})";
        auto waiter = std::make_shared<morph::testing::WaitReply>();
        server->handle(morph::wire::encode(execute), [waiter](std::string const& reply) { (*waiter)(reply); });
        waiters.push_back(std::move(waiter));
    }
    for (auto const& waiter : waiters) {
        REQUIRE(waiter->await(std::chrono::seconds{30}));
        REQUIRE(waiter->env.kind == "ok");
    }
    auto const stats = server->viewStats();
    CHECK(stats.computations == 1);
    CHECK(stats.waits == kRequests - 1);
}

namespace {

// An order level that records the keys taken back from it, and may refuse.
class LeaseLevel final : public LevelSource {
public:
    LeaseLevel(std::string level, bool refuse) : _level{std::move(level)}, _refuse{refuse} {
        _columns.push_back(tabletest::levelCol(_level == "recipe" ? "r" : "s", ColumnKind::Text, _level));
    }
    [[nodiscard]] std::string_view level() const override { return _level; }
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _columns; }
    [[nodiscard]] std::shared_ptr<LevelSnapshot const> snapshot() const override {
        struct Held final : LevelSnapshot {
            [[nodiscard]] LevelRead read(RowId const& key, std::span<Cell> out) const override {
                out[0] = Cell{std::to_string(std::get<std::int64_t>(key))};
                return LevelRead::Loaded;
            }
            [[nodiscard]] std::uint64_t generation() const override { return 1; }
        };
        return std::make_shared<Held const>();
    }
    std::expected<void, TableError> want(std::span<RowId const> /*keys*/, LevelUse use, bool /*retry*/) override {
        if (_refuse && use == LevelUse::Order) {
            return std::unexpected(TableError{.code = TableErrorCode::Unavailable, .column = {}, .message = "full"});
        }
        return {};
    }
    void unwant(std::span<RowId const> keys) override { unwanted += keys.size(); }

    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): `unwanted` counts the requests the source should not have seen; the case reads it
    std::size_t unwanted = 0;

private:
    std::string _level;
    bool _refuse;
    std::vector<ColumnInfo> _columns;
};

}  // namespace

// Mutation: answerQuery not taking back the accepted levels' wants (recipe keeps its lease).
TEST_CASE("table: a refused order use takes back the other levels' leases", "[table][levels]") {
    auto const recipe = std::make_shared<LeaseLevel>("recipe", false);
    auto const stock = std::make_shared<LeaseLevel>("stock", true);
    auto description =
        describedOf({column("id", ColumnKind::Integer), column("r", ColumnKind::Text), column("s", ColumnKind::Text)});
    description.columns[1].info.level = "recipe";
    description.columns[2].info.level = "stock";
    description.levels = {"recipe", "stock"};
    std::vector<std::vector<Cell>> rows;
    for (std::int64_t row = 0; row < 4; ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
        rows.push_back({Cell{row}});
    }
    auto const served = detail::pinned(
        ServedTable{.description = describe(std::move(description)),
                    .source = tabletest::makeSource({tabletest::col("id", ColumnKind::Integer)}, std::move(rows)),
                    .levels = {recipe, stock},
                    .mayRead = {},
                    .scope = {},
                    .services = {},
                    .limits = {}});
    TableQuery query;
    query.sort = {SortKey{.column = "r", .dir = SortDirection::Ascending},
                  SortKey{.column = "s", .dir = SortDirection::Ascending}};
    query.columns = std::vector<std::string>{"id"};
    auto const refused = detail::answerQuery(served, query);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().column == "s");
    CHECK(recipe->unwanted == 4);

    // Mutation: the distinct branch not taking back the accepted levels' wants (recipe keeps its second lease).
    TableQuery distinct;
    distinct.filters.columns.emplace(
        "s", ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.contains = "a"}}, .exclude = {}});
    distinct.distinct = DistinctRequest{.column = "r"};
    auto const refusedDistinct = detail::answerQuery(served, distinct);
    REQUIRE_FALSE(refusedDistinct.has_value());
    CHECK(refusedDistinct.error().column == "s");
    CHECK(recipe->unwanted == 8);
}

namespace {

// A table of four rows whose column `r` is served by a real level cache and `s` by a level that refuses order uses.
struct DistinctLevels {
    DistinctLevels()
        : recipe{std::make_shared<CellLevelCache>(
              LevelCacheOptions{.records = {"T.rows"},
                                .level = "recipe",
                                .columns = {tabletest::levelCol("r", ColumnKind::Text, "recipe")}},
              [](std::span<RowId const> keys) {
                  std::vector<std::pair<RowId, std::vector<Cell>>> out;
                  for (auto const& key : keys) {
                      out.emplace_back(key, std::vector<Cell>{Cell{std::to_string(std::get<std::int64_t>(key))}});
                  }
                  return out;
              })},
          stock{std::make_shared<LeaseLevel>("stock", true)} {
        auto description = describedOf(
            {column("id", ColumnKind::Integer), column("r", ColumnKind::Text), column("s", ColumnKind::Text)});
        description.columns[1].info.level = "recipe";
        description.columns[2].info.level = "stock";
        description.levels = {"recipe", "stock"};
        std::vector<std::vector<Cell>> rows;
        for (std::int64_t row = 0; row < 4; ++row) {
            // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
            rows.push_back({Cell{row}});
        }
        served = detail::pinned(
            ServedTable{.description = describe(std::move(description)),
                        .source = tabletest::makeSource({tabletest::col("id", ColumnKind::Integer)}, std::move(rows)),
                        .levels = {recipe, stock},
                        .mayRead = {},
                        .scope = {},
                        .services = {},
                        .limits = {}});
    }

    std::shared_ptr<CellLevelCache> recipe;
    std::shared_ptr<LeaseLevel> stock;
    ServedTable served;
    ViewCache views;
    detail::AnswerContext context{.views = &views, .record = "T.rows", .rowScoped = false};
};

}  // namespace

// Mutation: the distinct branch building a new key set for every request (each request fits a set of its own).
TEST_CASE("table: distinct requests over one snapshot hold one set of an order level's keys", "[table][levels]") {
    DistinctLevels const table;
    TableQuery distinct;
    distinct.distinct = DistinctRequest{.column = "r"};
    REQUIRE(detail::answerQuery(table.served, distinct, table.context).has_value());
    REQUIRE(detail::answerQuery(table.served, distinct, table.context).has_value());
    CHECK(detail::LevelCacheAccess::fits(*table.recipe) == 1);  // the second request repeated the kept set
    CHECK(detail::LevelCacheAccess::acceptedSets(*table.recipe) == 1);
}

// Mutation: the distinct branch not taking back the accepted levels' wants (recipe keeps the refused request's set).
TEST_CASE("table: a refused distinct request takes back its set from the levels that accepted it", "[table][levels]") {
    DistinctLevels const table;
    TableQuery distinct;
    distinct.filters.columns.emplace(
        "s", ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.contains = "a"}}, .exclude = {}});
    distinct.distinct = DistinctRequest{.column = "r"};
    auto const refused = detail::answerQuery(table.served, distinct, table.context);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().column == "s");
    CHECK(detail::LevelCacheAccess::fits(*table.recipe) == 1);  // recipe accepted the set before stock refused
    CHECK(detail::LevelCacheAccess::acceptedSets(*table.recipe) == 0);
}

namespace {

// The distinct cases' first query: city oneOf Berlin, Paris; status eq closed; and a distinct request on status.
TableQuery distinctStatusQuery() {
    TableQuery q;
    q.filters.columns["city"] =
        ColumnFilter{.combine = Combine::Any,
                     .include = {FilterEntry{.oneOf = std::vector<std::string>{"Berlin", "Paris"}}},
                     .exclude = {}};
    q.filters.columns["status"] =
        ColumnFilter{.combine = Combine::Any, .include = {FilterEntry{.eq = "closed"}}, .exclude = {}};
    q.distinct = DistinctRequest{.column = "status"};
    return q;
}

ServedTable distinctTable(std::shared_ptr<VectorSource> source) {
    auto description =
        TableDescription{.id = "d",
                         .label = "D",
                         .columns = {column("status", ColumnKind::Text), column("city", ColumnKind::Text),
                                     column("qty", ColumnKind::Integer)},
                         .key = "qty"};
    return detail::pinned(ServedTable{.description = describe(std::move(description)),
                                      .source = std::move(source),
                                      .levels = {},
                                      .mayRead = {},
                                      .scope = {},
                                      .services = {},
                                      .limits = {}});
}

}  // namespace

// Mutation: the writer leaving `distinct` out.
TEST_CASE("generic page: a distinct reply round-trips", "[table][description][distinct]") {
    auto const source = tabletest::distinctSource();
    auto const q = distinctStatusQuery();
    GenericPage page;
    page.revision = "r";
    page.distinct = *distinctValues(*source, q, *q.distinct);
    REQUIRE(page.distinct->values.size() == 2);
    auto const description = describedOf({column("status", ColumnKind::Text)});
    auto const back = readGenericPage(toJson(page), description);
    REQUIRE(back.has_value());
    CHECK(back->rows.empty());
    REQUIRE(back->distinct);
    CHECK(*back->distinct == *page.distinct);
}

TEST_CASE("answer query: a distinct request answers values and no rows", "[table][description][distinct]") {
    auto const source = tabletest::distinctSource();
    auto const served = distinctTable(source);
    auto const q = distinctStatusQuery();
    auto const page = detail::answerQuery(served, q);
    REQUIRE(page);
    CHECK(page->rows.empty());
    CHECK(page->keys.empty());
    REQUIRE(page->distinct);
    CHECK(*page->distinct == *distinctValues(*source, q, *q.distinct));
    // Mutation: answerQuery ignoring query.distinct: `distinct` is absent.

    auto refusing = served;
    refusing.description.capabilities.distinct = false;
    auto const refused = detail::answerQuery(refusing, q);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == TableErrorCode::Unavailable);
    // Mutation: the branch not reading capabilities.distinct.
}

namespace {

// The groups truth table: companyA..C (Text), declared group "company"; keys 0..4.
// 0: F, S, X   1: F, F, X   2: S, F, F   3: F, X, Y   4: X, Y, Z
std::shared_ptr<VectorSource> companyRows() {
    using S = std::string;
    auto source = tabletest::makeSource(
        {tabletest::col("companyA", ColumnKind::Text), tabletest::col("companyB", ColumnKind::Text),
         tabletest::col("companyC", ColumnKind::Text)},
        {{S{"F"}, S{"S"}, S{"X"}},
         {S{"F"}, S{"F"}, S{"X"}},
         {S{"S"}, S{"F"}, S{"F"}},
         {S{"F"}, S{"X"}, S{"Y"}},
         {S{"X"}, S{"Y"}, S{"Z"}}});
    source->setGroups(
        {ColumnGroupInfo{.id = "company", .label = "Company", .columns = {"companyA", "companyB", "companyC"}}});
    return source;
}

// `slow`, when set, is a level the table serves.
ServedTable companyTable(std::shared_ptr<DataSource const> source, std::shared_ptr<LevelSource> slow = nullptr) {
    auto description =
        TableDescription{.id = "companies",
                         .label = "Companies",
                         .columns = {column("companyA", ColumnKind::Text), column("companyB", ColumnKind::Text),
                                     column("companyC", ColumnKind::Text)},
                         .key = "companyA"};
    std::vector<std::shared_ptr<LevelSource>> levels;
    if (slow != nullptr) {
        description.levels = {std::string{slow->level()}};
        levels.push_back(std::move(slow));
    }
    description.groups.assign(source->groups().begin(), source->groups().end());
    return detail::pinned(ServedTable{.description = describe(std::move(description)),
                                      .source = std::move(source),
                                      .levels = std::move(levels),
                                      .mayRead = {},
                                      .scope = {},
                                      .services = {},
                                      .limits = {}});
}

}  // namespace

// Mutations: answerQuery calling validate without the source's groups, and detail::PinnedSource not forwarding
// groups(): the first reply is UnknownColumn naming "company". The scoped source, or the LeveledSource every reply
// is computed over, returning no groups: the scoped or levelled replies are UnknownColumn.
TEST_CASE("answer query: a declared group filters and pools on the server", "[table][description][groups]") {
    auto const source = companyRows();
    TableQuery q;
    q.filters.groups.push_back({.group = "company",
                                .mode = GroupMode::Together,
                                .include = {FilterEntry{.eq = "F"}, FilterEntry{.eq = "S"}}});
    TableQuery pooled;
    pooled.distinct = DistinctRequest{.group = "company"};
    auto const expectedValues = distinctValues(*source, {}, *pooled.distinct);
    REQUIRE(expectedValues);
    std::vector<RowId> const all{std::int64_t{0}, std::int64_t{1}, std::int64_t{2}, std::int64_t{3}, std::int64_t{4}};
    for (auto const& served : {companyTable(source), companyTable(scoped(source, all)),
                               companyTable(source, std::make_shared<RecordingLevel>("notes"))}) {
        auto const page = detail::answerQuery(served, q);
        REQUIRE(page);
        CHECK(page->keys == std::vector<RowId>{std::int64_t{0}, std::int64_t{2}});
        auto const values = detail::answerQuery(served, pooled);
        REQUIRE(values);
        REQUIRE(values->distinct);
        CHECK(*values->distinct == *expectedValues);
    }
}

namespace tabledesc {

// A typed table whose row type declares a column group.
class GroupModel {
public:
    std::shared_ptr<std::vector<tabletest::GroupRow> const> rows(morph::table::TableContext const& context);
};

}  // namespace tabledesc

BRIDGE_REGISTER_RECORD(tabletest::GroupRow, "GroupRow", morph::record::key<&tabletest::GroupRow::name>)
BRIDGE_REGISTER_MODEL(tabledesc::GroupModel, "GroupModel")
BRIDGE_REGISTER_TABLE(tabledesc::GroupModel, "groupRows",
                      morph::table::typed<tabletest::GroupRow>(&tabledesc::GroupModel::rows))

std::shared_ptr<std::vector<tabletest::GroupRow> const> tabledesc::GroupModel::rows(
    morph::table::TableContext const& context) {
    static_cast<void>(context);
    return std::make_shared<std::vector<tabletest::GroupRow> const>(
        std::vector<tabletest::GroupRow>{{.companyA = "F", .companyB = "S", .companyC = "X", .name = "a"},
                                         {.companyA = "F", .companyB = "F", .companyC = "X", .name = "b"},
                                         {.companyA = "S", .companyB = "F", .companyC = "F", .name = "c"}});
}

// Mutation: typedTable leaving description.groups empty.
TEST_CASE("typed: a row's column groups reach the description", "[table][registration][groups]") {
    morph::session::Context const session{};
    TableContext const context{.session = session};
    tabledesc::GroupModel model;
    auto const served = detail::serve<&tabledesc::GroupModel::rows>(model, context);
    REQUIRE(served.description.groups.size() == 1);
    CHECK(served.description.groups[0] ==
          ColumnGroupInfo{.id = "company", .label = "Company", .columns = {"companyA", "companyB", "companyC"}});
    auto const companyB = std::ranges::find(served.description.columns, std::string{"companyB"},
                                            [](TableColumn const& column) { return column.info.id; });
    REQUIRE(companyB != served.description.columns.end());
    CHECK(companyB->group == "company");
    auto const name = std::ranges::find(served.description.columns, std::string{"name"},
                                        [](TableColumn const& column) { return column.info.id; });
    REQUIRE(name != served.description.columns.end());
    CHECK(name->group.empty());
    TableQuery q;
    q.filters.groups.push_back({.group = "company",
                                .mode = GroupMode::Together,
                                .include = {FilterEntry{.eq = "F"}, FilterEntry{.eq = "S"}}});
    auto const page = detail::answerQuery(detail::pinned(served), q);
    REQUIRE(page);
    CHECK(page->keys == std::vector<RowId>{std::string{"a"}, std::string{"c"}});
}

namespace {

// A level that forwards to another and records every key it is wanted for, with the use.
class WantLog final : public LevelSource {
public:
    explicit WantLog(std::shared_ptr<LevelSource> inner) : _inner{std::move(inner)} {}
    [[nodiscard]] std::string_view level() const override { return _inner->level(); }
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _inner->columns(); }
    [[nodiscard]] std::shared_ptr<LevelSnapshot const> snapshot() const override { return _inner->snapshot(); }
    std::expected<void, TableError> want(std::span<RowId const> keys, LevelUse use, bool retry) override {
        for (auto const& key : keys) {
            wanted.emplace_back(key, use);
        }
        return _inner->want(keys, use, retry);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): `wanted` records the requests the source saw; the case reads it
    std::vector<std::pair<RowId, LevelUse>> wanted;

private:
    std::shared_ptr<LevelSource> _inner;
};

}  // namespace

// Mutation: the distinct branch running distinctValues over served.source instead of the LeveledSource: the first
// reply lists the base source's values and is not partial.
TEST_CASE("answer query: a distinct request reads a slow column through its level", "[table][description][distinct]") {
    auto const plain = tabletest::distinctSource();
    auto const status = tabletest::levelCol("status", ColumnKind::Text, "slow");
    tabletest::ManualExecutor loads;
    auto const snapshot = plain->snapshot();
    auto const cache = std::make_shared<CellLevelCache>(
        LevelCacheOptions{.records = {"d"}, .level = "slow", .columns = {status}, .loads = &loads},
        [snapshot](std::span<RowId const> keys) {
            std::vector<std::pair<RowId, std::vector<Cell>>> out;
            for (auto const& key : keys) {
                out.emplace_back(
                    key, std::vector<Cell>{snapshot->cell(static_cast<std::size_t>(std::get<std::int64_t>(key)), 0)});
            }
            return out;
        });
    auto log = std::make_shared<WantLog>(cache);
    auto description =
        TableDescription{.id = "d",
                         .label = "D",
                         .columns = {column("status", ColumnKind::Text), column("city", ColumnKind::Text),
                                     column("qty", ColumnKind::Integer)},
                         .key = "qty"};
    description.columns[0].info.level = "slow";
    description.levels = {"slow"};
    auto const served = detail::pinned(ServedTable{.description = describe(std::move(description)),
                                                   .source = plain,
                                                   .levels = {log},
                                                   .mayRead = {},
                                                   .scope = {},
                                                   .services = {},
                                                   .limits = {}});
    auto const q = distinctStatusQuery();
    auto const first = detail::answerQuery(served, q);
    REQUIRE(first);
    REQUIRE(first->distinct);
    CHECK(first->distinct->partial);
    CHECK(first->distinct->values.empty());
    std::vector<RowId> wantedKeys;
    for (auto const& [key, use] : log->wanted) {
        CHECK(use == LevelUse::Order);
        wantedKeys.push_back(key);
    }
    std::ranges::sort(wantedKeys);
    std::vector<RowId> allSixKeys;
    for (std::int64_t row = 0; row < 6; ++row) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `allSixKeys` is a fixture list built once; sizing it first would not change what the case checks
        allSixKeys.emplace_back(row);
    }
    CHECK(wantedKeys == allSixKeys);
    tabletest::drain(loads);
    auto const second = detail::answerQuery(served, q);
    REQUIRE(second);
    REQUIRE(second->distinct);
    CHECK_FALSE(second->distinct->partial);
    CHECK(*second->distinct == *distinctValues(*plain, q, *q.distinct));
}

// Mutation: the distinct branch answered before the column-presence check: the first request is a partial list
// over an unloaded column (SourceContract after it), the second a list that stays partial however often it is asked.
TEST_CASE("answer query: a distinct request is refused a column the table cannot read",
          "[table][description][distinct]") {
    auto const q = [] {
        TableQuery out;
        out.distinct = DistinctRequest{.column = "status"};
        return out;
    }();
    // A description column the base source lacks.
    auto missingBase =
        TableDescription{.id = "d",
                         .label = "D",
                         .columns = {column("status", ColumnKind::Text), column("extra", ColumnKind::Text)},
                         .key = "status"};
    auto const base = detail::pinned(ServedTable{.description = describe(std::move(missingBase)),
                                                 .source = tabletest::distinctSource(),
                                                 .levels = {},
                                                 .mayRead = {},
                                                 .scope = {},
                                                 .services = {},
                                                 .limits = {}});
    auto const refusedBase = detail::answerQuery(base, q);
    REQUIRE_FALSE(refusedBase);
    CHECK(refusedBase.error().code == TableErrorCode::UnknownColumn);
    CHECK(refusedBase.error().column == "extra");
    // A level column its level source does not serve: listed, it would stay unloaded for every row.
    auto missingLevel =
        TableDescription{.id = "d",
                         .label = "D",
                         .columns = {column("status", ColumnKind::Text), column("note", ColumnKind::Text)},
                         .key = "status"};
    missingLevel.columns[1].info.level = "notes";
    missingLevel.levels = {"notes"};
    auto const levelled = detail::pinned(ServedTable{.description = describe(std::move(missingLevel)),
                                                     .source = tabletest::distinctSource(),
                                                     .levels = {std::make_shared<RecordingLevel>("notes")},
                                                     .mayRead = {},
                                                     .scope = {},
                                                     .services = {},
                                                     .limits = {}});
    TableQuery notes;
    notes.distinct = DistinctRequest{.column = "note"};
    auto const refusedLevel = detail::answerQuery(levelled, notes);
    REQUIRE_FALSE(refusedLevel);
    CHECK(refusedLevel.error().code == TableErrorCode::UnknownColumn);
    CHECK(refusedLevel.error().column == "note");
}

namespace {

// Three columns: `id`, the Integer key, `name` Text and `c3` Decimal.
TableDescription threeColumns() {
    return TableDescription{.id = "t",
                            .label = "T",
                            .columns = {column("id", ColumnKind::Integer), column("name", ColumnKind::Text),
                                        column("c3", ColumnKind::Decimal)},
                            .key = "id"};
}

}  // namespace

// Mutation: servedDescription leaving capabilities at the default 8.
TEST_CASE("served description: limits carry the sort bound, up to 32", "[table][description]") {
    ServedTable served{.description = threeColumns()};
    auto const eight = servedDescription(served);
    REQUIRE(eight);
    CHECK(eight->capabilities.maxSortKeys == 8);
    CHECK(eight->revision.size() == 16);
    CHECK(eight->revision == describe(*eight).revision);
    served.limits.maxSortKeys = 32;
    auto const wide = servedDescription(served);
    REQUIRE(wide);
    CHECK(wide->capabilities.maxSortKeys == 32);
    CHECK(wide->revision != eight->revision);  // Mutation: the revision taken before the bound is set.
    served.limits.maxSortKeys = 33;
    CHECK(servedDescription(served).error().code == TableErrorCode::SourceContract);
    CHECK(servedDescription(served).error().message == "a table's `limits.maxSortKeys` is 1 to 32");
    served.limits.maxSortKeys = 0;
    CHECK(servedDescription(served).error().code == TableErrorCode::SourceContract);
}

// Mutation: answerQuery hashing served.description as the model gave it (the page names a revision no describe reply
// has).
TEST_CASE("served description: a page carries the served description's revision", "[table][description]") {
    ServedTable served = threeRowTable(true);
    served.limits.maxSortKeys = 32;
    auto const page = detail::answerQuery(served, TableQuery{});
    REQUIRE(page);
    CHECK(page->revision == servedDescription(served)->revision);
    CHECK(page->revision != served.description.revision);
    served.limits.maxSortKeys = 33;
    auto const refused = detail::answerQuery(served, TableQuery{});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == TableErrorCode::SourceContract);
}

namespace tabledesc {

// A model whose one table's `limits.maxSortKeys` is `bound`, and whose description names the lock companion `nope`,
// which it does not have, while `badLock` is set.
class BoundModel {
public:
    static inline std::size_t bound = 8;
    static inline bool badLock = false;
    morph::table::ServedTable rows(morph::table::TableContext const& context);
};

}  // namespace tabledesc

BRIDGE_REGISTER_MODEL(tabledesc::BoundModel, "BoundModel")
BRIDGE_REGISTER_TABLE(tabledesc::BoundModel, "boundRows", &tabledesc::BoundModel::rows)

morph::table::ServedTable tabledesc::BoundModel::rows(morph::table::TableContext const& context) {
    static_cast<void>(context);
    ServedTable served{.description = threeColumns()};
    served.source = std::make_shared<VectorSource>(columnInfos(served.description));
    served.limits.maxSortKeys = bound;
    if (badLock) {
        served.description.rowLock = "nope";
    }
    return served;
}

// Mutation: DescribeTable describing served.description as the model gave it (the bound stays 8).
TEST_CASE("served description: the describe action answers the served description", "[table][description][threads]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<tabledesc::BoundModel> handler{bridge, &owner};
    struct Restore {
        Restore() = default;
        Restore(Restore const&) = delete;
        Restore& operator=(Restore const&) = delete;
        Restore(Restore&&) = delete;
        Restore& operator=(Restore&&) = delete;
        ~Restore() { tabledesc::BoundModel::bound = 8; }
    } const restore;
    tabledesc::BoundModel::bound = 32;
    auto const described =
        morph::testing::awaitValueOn(owner, handler.execute(DescribeTable<&tabledesc::BoundModel::rows>{}));
    CHECK(described.capabilities.maxSortKeys == 32);
    ServedTable served{.description = threeColumns()};
    served.limits.maxSortKeys = 32;
    CHECK(described.revision == servedDescription(served)->revision);
    tabledesc::BoundModel::bound = 33;
    CHECK_THROWS(morph::testing::awaitValueOn(owner, handler.execute(DescribeTable<&tabledesc::BoundModel::rows>{})));
}

namespace {

// A served table of 40 Integer columns k00..k39, `k00` the key, no rows, held to `bound` sort keys.
ServedTable fortyColumns(std::size_t bound) {
    std::vector<TableColumn> columns;
    for (int at = 0; at < 40; ++at) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `columns` is a fixture list built once; sizing it first would not change what the case checks
        columns.push_back(column(std::format("k{:02}", at), ColumnKind::Integer));
    }
    ServedTable served{
        .description = TableDescription{.id = "forty", .label = "Forty", .columns = std::move(columns), .key = "k00"}};
    served.source = std::make_shared<VectorSource>(columnInfos(served.description));
    served.limits.maxSortKeys = bound;
    return detail::pinned(std::move(served));
}

// A query sorting by the first `keys` columns.
TableQuery sortedBy(std::size_t keys) {
    TableQuery query;
    for (std::size_t at = 0; at < keys; ++at) {
        query.sort.push_back(SortKey{.column = std::format("k{:02}", at), .dir = SortDirection::Ascending});
    }
    return query;
}

}  // namespace

// Mutation: validate holding 8 sort keys whatever the table's limits say (the 32-key and 12-key queries are
// refused). Mutation: validate ignoring the bound (the 33-key and 13-key queries are answered).
TEST_CASE("served description: a query's sort is held to the table's raised bound", "[table][description]") {
    ServedTable const wide = fortyColumns(32);
    CHECK(detail::answerQuery(wide, sortedBy(32)).has_value());
    auto const over = detail::answerQuery(wide, sortedBy(33));
    REQUIRE_FALSE(over.has_value());
    CHECK(over.error().code == TableErrorCode::LimitExceeded);
    ServedTable const twelve = fortyColumns(12);
    CHECK(detail::answerQuery(twelve, sortedBy(12)).has_value());
    auto const thirteen = detail::answerQuery(twelve, sortedBy(13));
    REQUIRE_FALSE(thirteen.has_value());
    CHECK(thirteen.error().code == TableErrorCode::LimitExceeded);
}

namespace tabledesc {

// A typed row with an amount in a named unit.
struct MeasureRow {
    std::int64_t id = 0;
    morph::table::MeasureValue weight;
};

// The units a measured model's rows name: "", t, kg; kg's number is 2 here.
inline std::shared_ptr<UnitRegistry const> const& modelUnits() {
    static auto const built = [] {
        UnitRegistryBuilder builder;
        std::ignore = (builder.add(UnitInfo{.id = "t",
                                            .symbol = "t",
                                            .dimension = "mass",
                                            .toBase = Rational{1000, DecimalPlaces{0}},
                                            .decimals = 3}));
        std::ignore = (builder.add(UnitInfo{
            .id = "kg", .symbol = "kg", .dimension = "mass", .toBase = Rational{1, DecimalPlaces{0}}, .decimals = 3}));
        return std::move(builder).build();
    }();
    return built;
}

inline std::shared_ptr<std::vector<MeasureRow> const> measureRows() {
    return std::make_shared<std::vector<MeasureRow> const>(
        std::vector<MeasureRow>{{.id = 1, .weight = {.amount = dec("0.3"), .unit = "kg"}}});
}

// A typed table with a measure whose model gives its units.
class MeasuredModel {
public:
    std::shared_ptr<std::vector<MeasureRow> const> rows(morph::table::TableContext const& context) {
        static_cast<void>(context);
        return measureRows();
    }
    std::shared_ptr<UnitRegistry const> tableUnits(morph::table::TableContext const& context) {
        static_cast<void>(context);
        return modelUnits();
    }
};

// The same table, its model declaring no units.
class UnmeasuredModel {
public:
    std::shared_ptr<std::vector<MeasureRow> const> rows(morph::table::TableContext const& context) {
        static_cast<void>(context);
        return measureRows();
    }
};

}  // namespace tabledesc

BRIDGE_REGISTER_RECORD(tabledesc::MeasureRow, "MeasureRow")
BRIDGE_REGISTER_MODEL(tabledesc::MeasuredModel, "MeasuredModel")
BRIDGE_REGISTER_TABLE(tabledesc::MeasuredModel, "measured",
                      morph::table::typed<tabledesc::MeasureRow>(&tabledesc::MeasuredModel::rows))
BRIDGE_REGISTER_MODEL(tabledesc::UnmeasuredModel, "UnmeasuredModel")
BRIDGE_REGISTER_TABLE(tabledesc::UnmeasuredModel, "unmeasured",
                      morph::table::typed<tabledesc::MeasureRow>(&tabledesc::UnmeasuredModel::rows))

// Mutation: readGenericPage with no registry returning SourceContract instead of resolving through
// registryOf(description) (the no-registry read fails).
TEST_CASE("generic page: a measure round-trips", "[table][description][measure]") {
    auto const& server = tabledesc::modelUnits();
    auto description = describedOf({column("w", ColumnKind::Measure)});
    description.units = {server->info(*server->find("kg"))};
    description = describe(std::move(description));
    VectorSource source{columnInfos(description)};
    source.setRows(intKeys(2), {{Cell{Measure{.amount = dec("0.3"), .unit = *server->find("kg")}}},
                                {Cell{Measure{.amount = dec("1"), .unit = kUnknownUnit}}}});
    std::vector<std::string> const columns{"w"};
    auto const json = toJson(writeGenericPage(pageOf(2), source, columns, server.get()));
    CHECK(json.contains(R"("rows":[[[3,10,"kg"]],[[1,1,null]]])"));
    auto const client = registryOf(description);
    REQUIRE(client.has_value());
    auto const kg = (*client)->find("kg");
    REQUIRE(kg.has_value());
    CHECK(*kg != *server->find("kg"));  // the numbers are each registry's own; the wire names the id
    auto const withRegistry = readGenericPage(json, description, client->get());
    REQUIRE(withRegistry.has_value());
    CHECK(std::get<Measure>(withRegistry->rows[0][0]) == Measure{.amount = dec("0.3"), .unit = *kg});
    CHECK(std::get<Measure>(withRegistry->rows[1][0]).unit == kUnknownUnit);
    auto const withoutRegistry = readGenericPage(json, description);
    REQUIRE(withoutRegistry.has_value());
    CHECK(std::get<Measure>(withoutRegistry->rows[0][0]) == Measure{.amount = dec("0.3"), .unit = *kg});
    CHECK(toJson(*withoutRegistry) == json);  // a page read is written back with its units' ids
    std::string unknown = json;
    unknown.replace(unknown.find(R"("kg")"), 4, R"("lb")");
    auto const read = readGenericPage(unknown, description);
    REQUIRE(read.has_value());
    CHECK(std::get<Measure>(read->rows[0][0]) == Measure{.amount = dec("0.3"), .unit = kUnknownUnit});
}

// Mutation: serve leaving a typed table's served.services.units null (the measured table's description is refused
// SourceContract).
TEST_CASE("typed table: a model's units reach its measures", "[table][registration][measure]") {
    morph::session::Context const session{};
    TableContext const context{.session = session};
    tabledesc::MeasuredModel measured;
    auto const served = detail::serve<&tabledesc::MeasuredModel::rows>(measured, context);
    CHECK(served.description.columns.at(1).info.kind == ColumnKind::Measure);
    auto const described = servedDescription(served);
    REQUIRE(described.has_value());
    REQUIRE(described->units.size() == 3);  // every dimension's units, for a column naming none: "", t and kg
    CHECK(described->units[2].id == "kg");
    auto const page = detail::answerQuery(detail::pinned(served), TableQuery{});
    REQUIRE(page.has_value());
    CHECK(page->revision == described->revision);
    CHECK(toJson(*page).contains(R"("rows":[[1,[3,10,"kg"]]])"));
    tabledesc::UnmeasuredModel unmeasured;
    auto const bare = detail::serve<&tabledesc::UnmeasuredModel::rows>(unmeasured, context);
    auto const refused = detail::answerQuery(detail::pinned(bare), TableQuery{});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::SourceContract);
    auto const refusedDescription = servedDescription(bare);
    REQUIRE_FALSE(refusedDescription.has_value());
    CHECK(refusedDescription.error().code == TableErrorCode::SourceContract);
}

// Mutation: validateDisplay skipping rowLock, which accepts `missingLock`.
TEST_CASE("description: a column's display is refused where it cannot apply", "[table][description][display]") {
    auto const base = [] {
        TableDescription d;
        d.id = "samples";
        d.key = "id";
        d.columns = {
            TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}, .label = "Id"},
            TableColumn{.info = {.id = "mass", .kind = ColumnKind::Decimal},
                        .label = "Mass",
                        .display = {.decimals = 2, .unit = "kg"},
                        .tone = "massTone",
                        .tooltip = "massTip"},
            TableColumn{.info = {.id = "amount", .kind = ColumnKind::Measure}, .label = "Amount"},
            TableColumn{.info = {.id = "massTone", .kind = ColumnKind::Text}, .shown = false},
            TableColumn{.info = {.id = "massTip", .kind = ColumnKind::Text}, .shown = false},
            TableColumn{.info = {.id = "lockedBy", .kind = ColumnKind::Text}, .shown = false},
        };
        d.rowLock = "lockedBy";
        return d;
    };
    REQUIRE(validateDisplay(base()).has_value());

    auto measureDecimals = base();
    measureDecimals.columns[2].display.decimals = 3;
    CHECK(validateDisplay(measureDecimals).error().code == TableErrorCode::SourceContract);
    auto measureUnit = base();
    measureUnit.columns[2].display.unit = "kg";
    CHECK(validateDisplay(measureUnit).error().code == TableErrorCode::SourceContract);
    auto missingTone = base();
    missingTone.columns[1].tone = "nope";
    CHECK(validateDisplay(missingTone).error().code == TableErrorCode::SourceContract);
    auto numericTooltip = base();
    numericTooltip.columns[1].tooltip = "id";
    CHECK(validateDisplay(numericTooltip).error().code == TableErrorCode::SourceContract);
    auto missingLock = base();
    missingLock.rowLock = "nope";
    REQUIRE_FALSE(validateDisplay(missingLock).has_value());
    CHECK(validateDisplay(missingLock).error().code == TableErrorCode::SourceContract);
    CHECK(validateDisplay(missingLock).error().column == "nope");
}

// Mutation: companions added for every column of the description, which puts massTone in `name`'s list.
TEST_CASE("description: companions travel with their column", "[table][description][display]") {
    TableDescription d;  // id, mass (tone massTone, tooltip massTip), name; rowTone rowState; rowLock lockedBy
    d.key = "id";
    d.columns = {
        TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}},
        TableColumn{.info = {.id = "mass", .kind = ColumnKind::Decimal}, .tone = "massTone", .tooltip = "massTip"},
        TableColumn{.info = {.id = "name", .kind = ColumnKind::Text}},
        TableColumn{.info = {.id = "massTone", .kind = ColumnKind::Text}, .shown = false},
        TableColumn{.info = {.id = "massTip", .kind = ColumnKind::Text}, .shown = false},
        TableColumn{.info = {.id = "rowState", .kind = ColumnKind::Text}, .shown = false},
        TableColumn{.info = {.id = "lockedBy", .kind = ColumnKind::Text}, .shown = false},
    };
    d.rowTone = "rowState";
    d.rowLock = "lockedBy";
    std::vector<std::string> const mass{"mass"};
    std::vector<std::string> const name{"name"};
    CHECK(columnsWithCompanions(d, mass) ==
          std::vector<std::string>{"mass", "massTone", "massTip", "rowState", "lockedBy"});
    CHECK(columnsWithCompanions(d, name) == std::vector<std::string>{"name", "rowState", "lockedBy"});
    CHECK(columnsWithCompanions(d, mass, {.tone = {"mass"}, .tooltip = {}, .rowTone = true}) ==
          std::vector<std::string>{"mass", "massTip", "lockedBy"});
    CHECK(companionColumns(d) == std::vector<std::string>{"lockedBy", "massTip", "massTone", "rowState"});
}

// Mutation: `Rounding` without glaze meta, which writes 1 for "halfUp".
TEST_CASE("description: a display travels in the description and changes its revision",
          "[table][description][display]") {
    TableDescription d;
    d.id = "samples";
    d.key = "id";
    d.columns = {
        TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}, .label = "Id"},
        TableColumn{.info = {.id = "mass", .kind = ColumnKind::Decimal},
                    .label = "Mass",
                    .display = {.decimals = 2,
                                .rounding = Rounding::HalfUp,
                                .labels = {ValueLabel{.operand = "3", .text = "three"}}}},
    };
    auto const described = describe(d);
    auto const json = glz::write_json(described).value_or(std::string{});
    CHECK(json.contains(R"("rounding":"halfUp")"));
    CHECK(json.contains(R"("labels":[{"operand":"3","text":"three"}])"));
    TableDescription back;
    REQUIRE_FALSE(glz::read_json(back, json));
    CHECK(back == described);
    auto halfEven = d;
    halfEven.columns[1].display.rounding = Rounding::HalfEven;
    CHECK(describe(halfEven).revision != described.revision);
}

// Mutation: the validateDisplay call removed from answerQuery, which answers the query for `bad`.
TEST_CASE("description: a served table with a bad companion is refused", "[table][description][display]") {
    auto const source = tabletest::makeSource({tabletest::col("id", ColumnKind::Integer)}, {{Cell{std::int64_t{1}}}});
    TableDescription d;
    d.id = "samples";
    d.key = "id";
    d.columns = {TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}, .label = "Id"}};
    ServedTable const good{
        .description = d, .source = source, .levels = {}, .mayRead = {}, .scope = {}, .services = {}, .limits = {}};
    REQUIRE(detail::answerQuery(detail::pinned(good), TableQuery{}).has_value());
    ServedTable bad = good;
    bad.description.rowLock = "nope";
    auto const refused = detail::answerQuery(detail::pinned(bad), TableQuery{});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::SourceContract);
    CHECK(refused.error().column == "nope");
}

// Mutation: the validateDisplay call removed from DescribeTable::invoke, which describes the table.
TEST_CASE("description: the describe action refuses a bad companion", "[table][description][display][threads]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<tabledesc::BoundModel> handler{bridge, &owner};
    struct Restore {
        Restore() = default;
        Restore(Restore const&) = delete;
        Restore& operator=(Restore const&) = delete;
        Restore(Restore&&) = delete;
        Restore& operator=(Restore&&) = delete;
        ~Restore() { tabledesc::BoundModel::badLock = false; }
    } const restore;
    REQUIRE_NOTHROW(
        morph::testing::awaitValueOn(owner, handler.execute(DescribeTable<&tabledesc::BoundModel::rows>{})));
    tabledesc::BoundModel::badLock = true;
    CHECK_THROWS(morph::testing::awaitValueOn(owner, handler.execute(DescribeTable<&tabledesc::BoundModel::rows>{})));
}

// Mutation: TableColumn written by reflection, every member, which writes "display", "tone" and "tooltip" for `id`.
TEST_CASE("description: a column that shows its cells plainly writes no display", "[table][description][display]") {
    TableDescription d;
    d.id = "samples";
    d.key = "id";
    d.columns = {
        TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}, .label = "Id"},
        TableColumn{.info = {.id = "mass", .kind = ColumnKind::Decimal},
                    .label = "Mass",
                    .display = {.decimals = 2},
                    .tone = "massTone"},
        TableColumn{.info = {.id = "massTone", .kind = ColumnKind::Text}, .shown = false},
    };
    auto const described = describe(d);
    auto const json = glz::write_json(described).value_or(std::string{});
    auto const count = [&json](std::string_view needle) {
        std::size_t found = 0;
        for (auto at = json.find(needle); at != std::string::npos; at = json.find(needle, at + 1)) {
            ++found;
        }
        return found;
    };
    CHECK(count(R"("display":)") == 1);
    CHECK(count(R"("tone":)") == 1);
    CHECK(count(R"("tooltip":)") == 0);
    TableDescription back;
    REQUIRE_FALSE(glz::read_json(back, json));
    CHECK(back == described);
}

namespace {

// NOLINTNEXTLINE(bugprone-throwing-static-initialization, cert-err58-cpp): `kMassBand` is a test constant built from literals; if building it threw, the run would fail at load, which is what a broken fixture should do
std::vector<BandInfo> const kMassBand{
    {.label = "Mass retained", .unit = "g", .columns = {"m8", "m4", "m2"}, .bands = {}}};

std::vector<TableColumn> bandColumns() {
    std::vector<TableColumn> columns;
    for (char const* const id : {"m8", "m4", "m2", "total"}) {
        columns.push_back(TableColumn{.info = {.id = id, .kind = ColumnKind::Decimal}});
    }
    return columns;
}

void refusedNaming(std::vector<BandInfo> const& bands, std::string_view label) {
    auto const checked = checkBands(bands, bandColumns());
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error().code == TableErrorCode::SourceContract);
    CHECK(checked.error().message.contains(label));
}

}  // namespace

// Mutation: checkBands not walking child bands.
TEST_CASE("description: bands are checked", "[table][description][bands]") {
    std::vector<TableColumn> const columns = bandColumns();
    CHECK(checkBands(kMassBand, columns).has_value());
    SECTION("four levels deep") {
        BandInfo deepest{.label = "Fourth", .unit = {}, .columns = {"m8"}, .bands = {}};
        BandInfo third{.label = "Third", .unit = {}, .columns = {"m8"}, .bands = {deepest}};
        BandInfo second{.label = "Second", .unit = {}, .columns = {"m8"}, .bands = {third}};
        refusedNaming({BandInfo{.label = "First", .unit = {}, .columns = {"m8"}, .bands = {second}}}, "Fourth");
        third.bands.clear();
        second.bands = {third};
        CHECK(checkBands(
                  std::vector<BandInfo>{BandInfo{.label = "First", .unit = {}, .columns = {"m8"}, .bands = {second}}},
                  columns)
                  .has_value());
    }
    SECTION("a band over no column") {
        refusedNaming({BandInfo{.label = "Empty", .unit = {}, .columns = {}, .bands = {}}}, "Empty");
    }
    SECTION("a child over a column its parent lacks") {
        BandInfo parent = kMassBand.front();
        parent.bands.push_back(BandInfo{.label = "Stray", .unit = {}, .columns = {"m8", "total"}, .bands = {}});
        refusedNaming({parent}, "Stray");
    }
    SECTION("a column in two bands of one level") {
        refusedNaming({kMassBand.front(), BandInfo{.label = "Again", .unit = {}, .columns = {"m8"}, .bands = {}}},
                      "Again");
    }
    SECTION("a column the table does not have") {
        refusedNaming({BandInfo{.label = "Unknown", .unit = {}, .columns = {"m16"}, .bands = {}}}, "Unknown");
    }
}

TEST_CASE("description: bands cross the wire", "[table][description][bands]") {
    TableDescription d;
    d.id = "sieves";
    d.columns = bandColumns();
    d.bands = kMassBand;
    d.bands.front().bands.push_back(BandInfo{.label = "Fine", .unit = {}, .columns = {"m2"}, .bands = {}});
    auto const described = describe(d);
    auto const json = glz::write_json(described).value_or(std::string{});
    TableDescription back;
    REQUIRE_FALSE(glz::read_json(back, json));
    CHECK(back == described);
    CHECK(back.bands.front().bands.front().label == "Fine");
}

namespace {

// The input schemas `checkEdits` reads in the cases below, by action id.
std::function<std::optional<std::string>(std::string_view)> editSchemas(std::string countValue) {
    return [countValue = std::move(countValue)](std::string_view action) -> std::optional<std::string> {
        if (action == "Lims_EditSpecimenCount") {
            return R"({"type":"object","properties":{"key":{"type":"integer"},"value":)" + countValue +
                   R"(},"required":["key","value"]})";
        }
        if (action == "Lims_EditSpecimenRow") {
            return std::string{R"({"type":"object","properties":{"key":{"type":"integer"},"values":{"type":"object",)"
                               R"("properties":{"label":{"type":["string","null"]}}}},"required":["key"]})"};
        }
        return std::nullopt;
    };
}

TableColumn editedColumn(std::string const& id, ColumnKind kind, EditInfo edit) {
    TableColumn out = column(id, kind);
    out.edit = std::move(edit);
    return out;
}

}  // namespace

// Mutation: checkEdits not comparing kinds.
TEST_CASE("description: an edit whose field does not fit its column is a source contract",
          "[table][description][edits]") {
    auto const count = describedOf({column("id", ColumnKind::Integer),
                                    editedColumn("count", ColumnKind::Integer, {.action = "Lims_EditSpecimenCount"})});
    auto const refused = checkEdits(count, editSchemas(R"({"type":"string"})"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TableErrorCode::SourceContract);
    CHECK(refused.error().column == "count");
    CHECK(checkEdits(count, editSchemas(R"({"type":"integer","maximum":10})")).has_value());

    auto const missing =
        describedOf({column("id", ColumnKind::Integer),
                     editedColumn("label", ColumnKind::Text, {.action = "Lims_EditSpecimenRow", .member = "nosuch"})});
    auto const lacking = checkEdits(missing, editSchemas(R"({"type":"integer"})"));
    REQUIRE_FALSE(lacking.has_value());
    CHECK(lacking.error().code == TableErrorCode::SourceContract);
    CHECK(lacking.error().message.contains("nosuch"));

    auto const fits =
        describedOf({column("id", ColumnKind::Integer),
                     editedColumn("count", ColumnKind::Integer, {.action = "Lims_EditSpecimenCount"}),
                     editedColumn("label", ColumnKind::Text, {.action = "Lims_EditSpecimenRow", .member = "label"})});
    CHECK(checkEdits(fits, editSchemas(R"({"type":"integer"})")).has_value());
    // An action the server does not know cannot edit a column.
    auto const unknown = describedOf(
        {column("id", ColumnKind::Integer), editedColumn("count", ColumnKind::Integer, {.action = "Nope"})});
    CHECK_FALSE(checkEdits(unknown, editSchemas(R"({"type":"integer"})")).has_value());
}

// Mutation: checkEdits not checking a measure field's unit against the table's units (every cell would open in a unit
// the table cannot convert into).
TEST_CASE("description: a measure edited in a unit the table lacks is a source contract",
          "[table][description][edits]") {
    auto const schemas = [](std::string_view action) -> std::optional<std::string> {
        if (action != "Lims_EditWeight") {
            return std::nullopt;
        }
        return std::string{
            R"({"type":"object","properties":{"key":{"type":"integer"},"value":{"type":["object","null"],)"
            R"("x-decimalPlaces":3,"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}}},"required":["key"]})"};
    };
    auto const measured = [](std::vector<UnitInfo> units) {
        TableDescription description{
            .id = "t",
            .label = "T",
            .columns = {column("id", ColumnKind::Integer),
                        editedColumn("weight", ColumnKind::Measure, {.action = "Lims_EditWeight"})},
            .key = "id"};
        description.units = std::move(units);
        return describe(std::move(description));
    };
    UnitInfo const gram{.id = "g",
                        .symbol = "g",
                        .dimension = "mass",
                        .toBase = Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}},
                        .decimals = 1};
    UnitInfo const kilogram{.id = "kg",
                            .symbol = "kg",
                            .dimension = "mass",
                            .toBase = Rational{Numerator{1}, Denominator{1}, DecimalPlaces{0}},
                            .decimals = 3};
    auto const lacking = checkEdits(measured({gram}), schemas);
    REQUIRE_FALSE(lacking.has_value());
    CHECK(lacking.error().code == TableErrorCode::SourceContract);
    CHECK(lacking.error().message.contains("kg"));
    CHECK(checkEdits(measured({gram, kilogram}), schemas).has_value());
}

// Mutation: the TableColumn meta leaving `edit` out (the read column has none).
TEST_CASE("description: a column's edit is on the wire, and absent when it has none", "[table][description][edits]") {
    auto const described =
        describedOf({column("id", ColumnKind::Integer),
                     editedColumn("label", ColumnKind::Text,
                                  {.action = "Lims_EditSpecimenRow", .member = "label", .readonly = "frozen"})});
    std::string const json = glz::write_json(described).value_or("");
    CHECK(json.contains(R"("edit":{"action":"Lims_EditSpecimenRow","member":"label","readonly":"frozen"})"));
    TableDescription read;
    REQUIRE_FALSE(glz::read_json(read, json));
    CHECK(read == described);
    CHECK(!glz::write_json(describedOf({column("id", ColumnKind::Integer)})).value_or("").contains("edit"));
}

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace descedit {

struct FitsEdit;
struct MisfitsEdit;

// Two tables whose `count` column (Integer) names an edit action: `fits` one taking an integer, `misfits` one taking
// text.
class EditsModel {
public:
    morph::table::ServedTable fits(morph::table::TableContext const& context);
    morph::table::ServedTable misfits(morph::table::TableContext const& context);
    morph::table::ChangedRows<&EditsModel::fits> execute(FitsEdit const& action);
    morph::table::ChangedRows<&EditsModel::misfits> execute(MisfitsEdit const& action);
};

struct FitsEdit : morph::table::CellEdit<&EditsModel::fits, std::int64_t> {};
struct MisfitsEdit : morph::table::CellEdit<&EditsModel::misfits, std::string> {};

}  // namespace descedit
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(descedit::EditsModel, "EditsModel")
BRIDGE_REGISTER_TABLE(descedit::EditsModel, "fits", &descedit::EditsModel::fits)
BRIDGE_REGISTER_TABLE(descedit::EditsModel, "misfits", &descedit::EditsModel::misfits)
BRIDGE_REGISTER_ACTION(descedit::EditsModel, descedit::FitsEdit, "FitsEdit")
BRIDGE_REGISTER_ACTION(descedit::EditsModel, descedit::MisfitsEdit, "MisfitsEdit")

namespace descedit {

namespace {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `editedTable(id, action)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
morph::table::ServedTable editedTable(std::string const& id, std::string action) {
    morph::table::ServedTable served;
    served.description.id = id;
    served.description.label = id;
    served.description.key = "id";
    served.description.columns = {column("id", ColumnKind::Integer),
                                  editedColumn("count", ColumnKind::Integer, {.action = std::move(action)})};
    auto source = std::make_shared<VectorSource>(columnInfos(served.description));
    source->setRows({RowId{std::int64_t{1}}}, {{Cell{std::int64_t{1}}, Cell{std::int64_t{1}}}});
    served.source = std::move(source);
    return served;
}

}  // namespace

morph::table::ServedTable EditsModel::fits(morph::table::TableContext const& /*context*/) {
    return editedTable("fits", "FitsEdit");
}

morph::table::ServedTable EditsModel::misfits(morph::table::TableContext const& /*context*/) {
    return editedTable("misfits", "MisfitsEdit");
}

morph::table::ChangedRows<&EditsModel::fits> EditsModel::execute(FitsEdit const& action) {
    return {.rows = {ChangedRow{.key = action.key}}};
}

morph::table::ChangedRows<&EditsModel::misfits> EditsModel::execute(MisfitsEdit const& action) {
    return {.rows = {ChangedRow{.key = action.key}}};
}

}  // namespace descedit

// Mutation: DescribeTable not calling checkEdits (the misfitting description is served).
TEST_CASE("description: a server refuses to describe a table whose edit does not fit its column",
          "[table][description][edits][threads]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool, morph::backend::ServerConfig{});
    auto const registered =
        morph::wire::decode(server->handleInline(morph::wire::encode(morph::wire::makeRegister("EditsModel"))));
    REQUIRE(registered.kind == "ok");
    auto const describe = [&](std::string const& action) {
        morph::wire::Envelope execute;
        execute.kind = "execute";
        execute.callId = 9;
        execute.modelId = registered.modelId;
        execute.modelType = "EditsModel";
        execute.actionType = action;
        execute.body = "{}";
        auto const waiter = std::make_shared<morph::testing::WaitReply>();
        server->handle(morph::wire::encode(execute), [waiter](std::string const& reply) { (*waiter)(reply); });
        REQUIRE(waiter->await(std::chrono::seconds{30}));
        return waiter->env;
    };
    auto const fits = describe("Table.fits.describe");
    CHECK(fits.kind == "ok");
    CHECK(fits.body.contains(R"("edit":{"action":"FitsEdit")"));
    auto const misfits = describe("Table.misfits.describe");
    CHECK(misfits.kind == "err");
    INFO(misfits.message);
    REQUIRE(misfits.refusal.has_value());
    CHECK(misfits.refusal->code == "table.sourceContract");
}
