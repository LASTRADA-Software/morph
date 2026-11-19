// SPDX-License-Identifier: Apache-2.0

// The client table model: `EngineRows` over the engine, `WindowRows` over a server's pages, and the status line's
// counts (docs/spec/table/table.md, "The client table model", "The window under changes", "The status line").
//
// Each case names the mutation it was seen to fail on.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/table/data_source.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <vector>

#include "ui_table_support.hpp"

namespace table = morph::table;
using morph::ui::countsText;
using morph::ui::EngineRows;
using morph::ui::FormatContext;
using morph::ui::Value;
using morph::ui::WindowRows;
using morph::ui::testing::describeJson;
using morph::ui::testing::formatFor;
using morph::ui::testing::keyedJson;
using morph::ui::testing::pageJson;
using morph::ui::testing::pageJsonAt;
using morph::ui::testing::RowsFixture;
using morph::ui::testing::samplesDescription;
using morph::ui::testing::TestRowsSource;
using morph::ui::testing::valueToJsonText;
using namespace std::chrono_literals;

// Mutations: a fetch per `visible` call (the first CHECK: 19 calls); no cancel of `superseded()` (the `stopped` and
// `cancelled` CHECKs); an adjacent page waiting for the settle (the last CHECK).  Gate K6.
TEST_CASE("window rows: a scroll that passes sends no fetch until it rests, and cancels what it left", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 100'000);                     // pages 0 and 1 are held
    std::size_t const before = h.dispatcher.count("Table.samples");
    for (std::int64_t at = 2'500; at < 50'000; at += 2'500) { rows->visible(at, 30, h.columns); h.advance(10ms); }
    CHECK(h.dispatcher.count("Table.samples") == before);           // inside tableSettle each time
    h.advance(80ms);                                                 // rests at 47,500
    std::size_t const rested = h.dispatcher.count("Table.samples");
    CHECK(rested > before);  CHECK(rested <= before + 3);            // K6: at most three for a resting place
    for (std::int64_t at = 50'000; at < 100'000; at += 2'500) { rows->visible(at, 30, h.columns); h.advance(10ms); }
    h.advance(80ms);                                                 // rests at 97,500; the first rest is unanswered
    for (std::size_t call = before; call < rested; ++call) { CHECK(h.stopped(call)); }   // what it left is cancelled
    CHECK(rows->stats().cancelled == rested - before);
    std::size_t const second = h.dispatcher.count("Table.samples");
    CHECK(second > rested);  CHECK(second <= rested + 3);
    while (h.pending("Table.samples") > 0) { h.replyPage(pageJson(97'400, 200, 100'000, h.columns)); }
    rows->visible(97'790, 30, h.columns);  h.settle();               // into the page after a held one, no clock advance
    CHECK(h.dispatcher.count("Table.samples") == second + 1);        // at once
}

// Mutation: a refetch per event (the first refetch's reply is older than the events after it and is fetched again
// at once: the 249 ms CHECK counts two).
TEST_CASE("window rows: an awaited order is throttled", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    // One full page and no other, so each refetch is one fetch.
    std::string const awaited = pageJson(0, 200, 200, h.columns, table::TotalKind::Exact, 0, {"recipe"});
    rows->visible(0, 30, h.columns);  h.replyPage(awaited);          // the order awaits `recipe`
    std::size_t const before = h.dispatcher.count("Table.samples");
    for (std::int64_t key = 0; key < 10; ++key) { h.change({key}); h.advance(10ms); }   // ten events in 100 ms
    CHECK(h.dispatcher.count("Table.samples") == before + 1);        // one, at the first event
    h.replyPage(awaited);
    h.advance(149ms);  CHECK(h.dispatcher.count("Table.samples") == before + 1);   // 249 ms after it
    h.advance(1ms);    CHECK(h.dispatcher.count("Table.samples") == before + 2);   // tableAwaitedRefetch
    h.replyPage(awaited);  h.advance(1'000ms);
    CHECK(h.dispatcher.count("Table.samples") == before + 2);        // no event since: nothing more
}

// Mutation: build the record from every column the window holds for the row (converts 40, not 20).  Gate K8.
TEST_CASE("window rows: a row of 300 columns converts the twenty it is asked for", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples", samplesDescription(300));
    std::vector<std::string> const forty = h.columnIds(0, 40);      // id, name, c3 … c40
    std::vector<std::string> const twenty = h.columnIds(0, 20);     // the first twenty of them
    rows->visible(0, 50, forty);
    h.replyPage(pageJson(0, 200, 9093, forty));                     // the window now holds 40 columns of row 0
    REQUIRE(h.window(*rows).holds(0, "c40"));                       // the precondition K8 needs: 40 held
    std::size_t const sent = h.dispatcher.count("Table.samples");
    rows->visible(0, 50, twenty);  h.settle();
    CHECK(h.dispatcher.count("Table.samples") == sent);             // held already: no fetch
    REQUIRE(rows->row(0).has_value());
    CHECK(rows->row(0)->row.asRecord()->size() == 20);
    CHECK(rows->stats().cellsConverted == 20);                      // K8; the mutation converts 40
    static_cast<void>(rows->row(0));  CHECK(rows->stats().cellsConverted == 20);  // memoised
}

// Mutations: decoding through a `Value` (`ui::valueFromJson` refuses 2.5 and the page fails whole: `error()` is set);
// a double `parseDecimal` refuses read as `null` (the row 1 REQUIRE).
TEST_CASE("window rows: a page with a Number cell is read from the reply's text", "[ui][table]") {
    RowsFixture h;
    table::TableDescription d = samplesDescription(2);              // id Integer key, then one more column
    d.columns[1].info = table::ColumnInfo{.id = "ratio", .kind = table::ColumnKind::Number};
    auto const rows = h.windowRows("samples", table::describe(d));
    std::vector<std::string> const columns{"id", "ratio"};
    rows->visible(0, 30, columns);
    h.replyPage(R"({"revision":")" + rows->description()->revision +
                R"(","columns":["id","ratio"],"keys":[1,2],"rows":[[1,2.5],[2,1e308]],"total":2})");
    REQUIRE_FALSE(rows->error().has_value());
    CHECK(rows->total().rows == 2);
    REQUIRE(rows->row(0)->row.field("ratio")->asDecimal() != nullptr);          // cellValue: double → Decimal
    CHECK(valueToJsonText(*rows->row(0)->row.field("ratio")) == R"({"num":5,"den":2,"dp":1})");
    REQUIRE(rows->row(1)->row.field("ratio")->asString() != nullptr);           // beyond a Decimal: its text
    CHECK(*rows->row(1)->row.field("ratio")->asString() == "1e+308");
}

// Mutation: a measure cell read without the description's registry (its amount alone, a Decimal).
TEST_CASE("window rows: a measure cell is a quantity in its own unit", "[ui][table]") {
    RowsFixture h;
    table::TableDescription d = samplesDescription(2);
    d.columns[1].info = table::ColumnInfo{.id = "weight", .kind = table::ColumnKind::Measure};
    d.units = {table::UnitInfo{
                   .id = "kg", .symbol = "kg", .dimension = "mass", .toBase = *table::parseDecimal("1"), .decimals = 3},
               table::UnitInfo{.id = "g",
                               .symbol = "g",
                               .dimension = "mass",
                               .toBase = *table::parseDecimal("0.001"),
                               .decimals = 0}};
    auto const rows = h.windowRows("samples", table::describe(d));
    std::vector<std::string> const columns{"id", "weight"};
    rows->visible(0, 30, columns);
    h.replyPage(R"({"revision":")" + rows->description()->revision +
                R"(","columns":["id","weight"],"keys":[1],"rows":[[1,[3,10,"g"]]],"total":1})");
    REQUIRE_FALSE(rows->error().has_value());
    auto const* const weight = rows->row(0)->row.field("weight")->asQuantity();
    REQUIRE(weight != nullptr);
    CHECK(weight->unit == "g");
    CHECK(weight->value == *table::parseDecimal("0.3"));
}

// Mutation: send the first page before `release`.
TEST_CASE("window rows: a restoring window sends no query until released", "[ui][table]") {
    RowsFixture h;  auto const rows = h.restoringRows("samples");
    rows->visible(0, 30, h.columns);  h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 0);
    REQUIRE(rows->setSort({{.column = "name", .dir = table::SortDirection::Descending}}));
    h.settle();  CHECK(h.dispatcher.count("Table.samples") == 0);   // still restoring
    rows->release();  h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 1);                // T3: one query, carrying the sort
    CHECK(h.dispatcher.calls.back().body.contains(R"("column":"name")"));
    CHECK(h.dispatcher.calls.back().body.contains(R"("dir":"desc")"));   // the wire's name for a direction
}

// Mutation: a description that does not read taken as an empty description (no error is set, and a page fetch
// goes out).
TEST_CASE("window rows: a description that does not read sends no page fetch", "[ui][table]") {
    RowsFixture h;
    WindowRows rows{h.session, h.options("samples")};
    h.settle();
    rows.setDescription(Value{"not a description"});
    rows.visible(0, 30, h.columns);  h.settle();
    REQUIRE(rows.error().has_value());
    CHECK(rows.error()->code == table::TableErrorCode::InvalidValue);
    CHECK(h.dispatcher.count("Table.samples") == 0);
    auto const json = morph::ui::parseJson(describeJson(samplesDescription(3)));
    REQUIRE(json.has_value());
    auto const described = morph::ui::valueFromJson(*json);
    REQUIRE(described.has_value());
    rows.setDescription(*described);  h.settle();                    // the describe entry's value reads
    CHECK_FALSE(rows.error().has_value());
    REQUIRE(rows.description() != nullptr);
    CHECK(rows.description()->revision == samplesDescription(3).revision);
    CHECK(h.dispatcher.count("Table.samples") == 1);
}

// Mutation: the stale-description handler called for every reply that names another revision (it counts two).
TEST_CASE("window rows: a reply of another revision is reported once and taken", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    int stale = 0;
    rows->onStaleDescription([&stale] { ++stale; });
    rows->visible(0, 30, h.columns);  h.settle();
    h.replyPage(pageJsonAt("another", 0, 200, 300, h.columns));      // page 0; page 1 goes next to it
    h.replyPage(pageJsonAt("another", 200, 200, 300, h.columns));
    CHECK(stale == 1);
    CHECK(rows->total().known);
    CHECK(rows->total().rows == 300);
    REQUIRE(rows->row(250).has_value());
}

// Mutation: a change with keys answered by `invalidate()` (no keyed fetch: `rowsFetches` is 0 and the pages go
// again).
TEST_CASE("window rows: a change with keys is a keyed fetch that patches the rows", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 300);                         // rows 0 to 299 are held
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    std::size_t const before = h.dispatcher.count("Table.samples");
    h.change({5});
    CHECK(rows->stats().rowsFetches == 1);
    REQUIRE(h.dispatcher.count("Table.samples") == before + 1);
    CHECK(h.dispatcher.calls.back().body.contains(R"("keys":[5,0])"));   // the key and the probe of the held run
    h.replyPage(keyedJson({{0, 0}, {5, 5}}, 300, h.columns));
    REQUIRE(rows->row(5).has_value());
    CHECK(*rows->row(5)->row.field("name")->asString() == "renamed-5");
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].ops.size() == 2);                               // rows 0 and 5, each changed in place
    CHECK(h.dispatcher.count("Table.samples") == before + 1);        // no page went again
}

// Mutations: countsText ignoring `undecided`; countsText grouping with a fixed "," (the "de" line fails).
TEST_CASE("status line: counts for exact, lower-bound and undecided totals", "[ui][table]") {
    using enum table::TotalKind;
    FormatContext const en = formatFor("en");
    FormatContext const de = formatFor("de");
    REQUIRE(en.numeric.groupSeparator == ",");                      // the helper took the locale table's row
    CHECK(countsText({.view = {9093, Exact, true}}, 0, de) == "9.093 rows");
    CHECK(countsText({.view = {9093, Exact, true}}, 0, en) == "9,093 rows");
    CHECK(countsText({.view = {100000, AtLeast, true}}, 0, en) == "100,000 or more");
    CHECK(countsText({.view = {12, AtLeast, true}, .undecided = 40}, 0, en) == "12 so far, 40 loading");
    CHECK(countsText({.view = {9093, Exact, true}}, 3, en) == "3 of 9,093 selected");
    CHECK(countsText({.view = {1, Exact, true}}, 0, en) == "1 row");
    CHECK(countsText({}, 0, en).empty());
}

// Mutations: `keysOf` completing only after the next view change (`keysNow` fails the test); `row` answering nothing
// outside the last `visible` range (the row 499 REQUIRE).
TEST_CASE("engine rows: every row is present, visible does nothing, keysOf completes at once", "[ui][table]") {
    RowsFixture h;  auto const source = std::make_shared<TestRowsSource>(500);
    EngineRows rows{source, source->recordOf(), table::EngineOptions{.owner = &h.owner}};
    CHECK(rows.total().rows == 500);  CHECK(rows.total().known);  CHECK(rows.total().kind == table::TotalKind::Exact);
    rows.visible(0, 10, {});  h.settle();                           // names ten rows and no column
    REQUIRE(rows.row(499).has_value());
    CHECK(rows.row(499)->key == table::RowId{std::int64_t{499}});
    CHECK(*rows.row(499)->row.field("name")->asString() == "row-499");
    CHECK(rows.unloadedLevels(499).levels.empty());
    CHECK_FALSE(rows.row(500).has_value());
    CHECK(h.keysNow(rows, 100, 3) == std::vector<table::RowId>{std::int64_t{100}, std::int64_t{101}, std::int64_t{102}});
    CHECK(rows.counts().undecided == 0);  CHECK_FALSE(rows.counts().pending);
}

// Mutation: no memo (recordsBuilt is 2 after the two reads).
TEST_CASE("engine rows: a row's record is built once until the row changes", "[ui][table]") {
    RowsFixture h;  auto const source = std::make_shared<TestRowsSource>(10);
    EngineRows rows{source, source->recordOf(), table::EngineOptions{.owner = &h.owner}};
    static_cast<void>(rows.row(0)); static_cast<void>(rows.row(0));  CHECK(rows.recordsBuilt() == 1);
    source->rename(0, "renamed");  h.settle();
    CHECK(*rows.row(0)->row.field("name")->asString() == "renamed");  CHECK(rows.recordsBuilt() == 2);
    static_cast<void>(rows.row(1));  CHECK(rows.recordsBuilt() == 3);   // another row: its own record
}
