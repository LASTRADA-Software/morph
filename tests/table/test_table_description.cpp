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
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

TableColumn column(std::string id, ColumnKind kind) {
    return TableColumn{
        .info = {.id = id, .kind = kind, .comparator = {}, .format = {}, .level = {}}, .label = id, .group = {}};
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
    ColumnInfo const info{.id = "v", .kind = kind, .comparator = {}, .format = {}, .level = {}};
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
    VectorSource source{{ColumnInfo{.id = "v", .kind = kind, .comparator = {}, .format = {}, .level = {}}}};
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
    auto const page = apply(source, query);
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

TEST_CASE("typed: a typed table writes the page the generic source writes", "[table][registration]") {
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
    auto base = std::make_shared<VectorSource>(std::vector<ColumnInfo>{tabletest::col("v", ColumnKind::Integer)});
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
          "[table][registration]") {
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

TEST_CASE("table action: a server builds a table's reply on tableWork", "[table][registration]") {
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
