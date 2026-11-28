// SPDX-License-Identifier: Apache-2.0

// `WindowRows` and `EngineRows` over a server's pages, and the table catalog.
//
// Part of the client table model tests; see docs/spec/table/table.md.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace table = morph::table;

using namespace table_model_support;

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
using morph::ui::testing::answerPages;
using morph::ui::testing::diagnosticsMentioning;
using morph::ui::testing::elementValue;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::headerLabels;
using morph::ui::testing::kBoundSort;
using morph::ui::testing::kModelTable;
using morph::ui::testing::kNoPersist;
using morph::ui::testing::kStored;
using morph::ui::testing::lastQuery;
using morph::ui::testing::queryOf;
using morph::ui::testing::readJson;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::statusOf;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;
using morph::ui::testing::tableOf;
using morph::ui::testing::withTableKey;

// Mutations: a fetch per `visible` call (the first CHECK: 19 calls); no cancel of `superseded()` (the `stopped` and
// `cancelled` CHECKs); an adjacent page waiting for the settle (the last CHECK).  Gate K6.

TEST_CASE("window rows: a scroll that passes sends no fetch until it rests, and cancels what it left", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 100'000);  // pages 0 and 1 are held
    std::size_t const before = h.dispatcher.count("Table.samples");
    for (std::int64_t at = 2'500; at < 50'000; at += 2'500) {
        rows->visible(at, 30, h.columns);
        h.advance(10ms);
    }
    CHECK(h.dispatcher.count("Table.samples") == before);  // inside tableSettle each time
    h.advance(80ms);                                       // rests at 47,500
    std::size_t const rested = h.dispatcher.count("Table.samples");
    CHECK(rested > before);
    CHECK(rested <= before + 3);  // K6: at most three for a resting place
    for (std::int64_t at = 50'000; at < 100'000; at += 2'500) {
        rows->visible(at, 30, h.columns);
        h.advance(10ms);
    }
    h.advance(80ms);  // rests at 97,500; the first rest is unanswered
    for (std::size_t call = before; call < rested; ++call) {
        CHECK(h.stopped(call));
    }  // what it left is cancelled
    CHECK(rows->stats().cancelled == rested - before);
    std::size_t const second = h.dispatcher.count("Table.samples");
    CHECK(second > rested);
    CHECK(second <= rested + 3);
    while (h.pending("Table.samples") > 0) {
        h.replyPage(pageJson(97'400, 200, 100'000, h.columns));
    }
    rows->visible(97'790, 30, h.columns);
    h.settle();                                                // into the page after a held one, no clock advance
    CHECK(h.dispatcher.count("Table.samples") == second + 1);  // at once
}

// Mutation: a refetch per event (the first refetch's reply is older than the events after it and is fetched again
// at once: the 249 ms CHECK counts two).

TEST_CASE("window rows: an awaited order is throttled", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    // One full page and no other, so each refetch is one fetch.
    std::string const awaited = pageJson(0, 200, 200, h.columns, table::TotalKind::Exact, 0, {"recipe"});
    rows->visible(0, 30, h.columns);
    h.replyPage(awaited);  // the order awaits `recipe`
    std::size_t const before = h.dispatcher.count("Table.samples");
    for (std::int64_t key = 0; key < 10; ++key) {
        h.change({key});
        h.advance(10ms);
    }  // ten events in 100 ms
    CHECK(h.dispatcher.count("Table.samples") == before + 1);  // one, at the first event
    h.replyPage(awaited);
    h.advance(149ms);
    CHECK(h.dispatcher.count("Table.samples") == before + 1);  // 249 ms after it
    h.advance(1ms);
    CHECK(h.dispatcher.count("Table.samples") == before + 2);  // tableAwaitedRefetch
    h.replyPage(awaited);
    h.advance(1'000ms);
    CHECK(h.dispatcher.count("Table.samples") == before + 2);  // no event since: nothing more
}

// Mutation: build the record from every column the window holds for the row (converts 40, not 20).  Gate K8.

TEST_CASE("window rows: a row of 300 columns converts the twenty it is asked for", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples", samplesDescription(300));
    std::vector<std::string> const forty = h.columnIds(0, 40);   // id, name, c3 … c40
    std::vector<std::string> const twenty = h.columnIds(0, 20);  // the first twenty of them
    rows->visible(0, 50, forty);
    h.replyPage(pageJson(0, 200, 9093, forty));  // the window now holds 40 columns of row 0
    REQUIRE(h.window(*rows).holds(0, "c40"));    // the precondition K8 needs: 40 held
    std::size_t const sent = h.dispatcher.count("Table.samples");
    rows->visible(0, 50, twenty);
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == sent);  // held already: no fetch
    REQUIRE(rows->row(0).has_value());
    CHECK(rows->row(0)->row.asRecord()->size() == 20);
    CHECK(rows->stats().cellsConverted == 20);  // K8; the mutation converts 40
    static_cast<void>(rows->row(0));
    CHECK(rows->stats().cellsConverted == 20);  // memoised
}

// Mutations: decoding through a `Value` (`ui::valueFromJson` refuses 2.5 and the page fails whole: `error()` is set);
// a double `parseDecimal` refuses read as `null` (the row 1 REQUIRE).

TEST_CASE("window rows: a page with a Number cell is read from the reply's text", "[ui][table]") {
    RowsFixture h;
    table::TableDescription d = samplesDescription(2);  // id Integer key, then one more column
    d.columns[1].info = table::ColumnInfo{.id = "ratio", .kind = table::ColumnKind::Number};
    auto const rows = h.windowRows("samples", table::describe(d));
    std::vector<std::string> const columns{"id", "ratio"};
    rows->visible(0, 30, columns);
    h.replyPage(R"({"revision":")" + rows->description()->revision +
                R"(","columns":["id","ratio"],"keys":[1,2],"rows":[[1,2.5],[2,1e308]],"total":2})");
    REQUIRE_FALSE(rows->error().has_value());
    CHECK(rows->total().rows == 2);
    REQUIRE(rows->row(0)->row.field("ratio")->asDecimal() != nullptr);  // cellValue: double → Decimal
    CHECK(valueToJsonText(*rows->row(0)->row.field("ratio")) == R"({"num":5,"den":2,"dp":1})");
    REQUIRE(rows->row(1)->row.field("ratio")->asString() != nullptr);  // beyond a Decimal: its text
    CHECK(*rows->row(1)->row.field("ratio")->asString() == "1e+308");
}

// Mutation: a measure cell read without the description's registry (its amount alone, a Decimal).

TEST_CASE("window rows: a measure cell is a quantity in its own unit", "[ui][table]") {
    RowsFixture h;
    table::TableDescription d = samplesDescription(2);
    d.columns[1].info = table::ColumnInfo{.id = "weight", .kind = table::ColumnKind::Measure};
    d.units = {
        table::UnitInfo{
            .id = "kg", .symbol = "kg", .dimension = "mass", .toBase = *table::parseDecimal("1"), .decimals = 3},
        table::UnitInfo{
            .id = "g", .symbol = "g", .dimension = "mass", .toBase = *table::parseDecimal("0.001"), .decimals = 0}};
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
    RowsFixture h;
    auto const rows = h.restoringRows("samples");
    rows->visible(0, 30, h.columns);
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 0);
    REQUIRE(rows->setSort({{.column = "name", .dir = table::SortDirection::Descending}}));
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 0);  // still restoring
    rows->release();
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 1);  // T3: one query, carrying the sort
    CHECK(h.dispatcher.calls.back().body.contains(R"("column":"name")"));
    CHECK(h.dispatcher.calls.back().body.contains(R"("dir":"desc")"));  // the wire's name for a direction
}

// Mutation: a description that does not read taken as an empty description (no error is set, and a page fetch
// goes out).

TEST_CASE("window rows: a description that does not read sends no page fetch", "[ui][table]") {
    RowsFixture h;
    WindowRows rows{h.session, h.options("samples")};
    h.settle();
    rows.setDescription(Value{"not a description"});
    rows.visible(0, 30, h.columns);
    h.settle();
    REQUIRE(rows.error().has_value());
    CHECK(rows.error()->code == table::TableErrorCode::InvalidValue);
    CHECK(h.dispatcher.count("Table.samples") == 0);
    auto const json = morph::ui::parseJson(describeJson(samplesDescription(3)));
    REQUIRE(json.has_value());
    auto const described = morph::ui::valueFromJson(*json);
    REQUIRE(described.has_value());
    rows.setDescription(*described);
    h.settle();  // the describe entry's value reads
    CHECK_FALSE(rows.error().has_value());
    REQUIRE(rows.description() != nullptr);
    CHECK(rows.description()->revision == samplesDescription(3).revision);
    CHECK(h.dispatcher.count("Table.samples") == 1);
}

// Mutation: the stale-description handler called for every reply that names another revision (it counts two).

TEST_CASE("window rows: a reply of another revision is reported once and taken", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    int stale = 0;
    rows->onStaleDescription([&stale] { ++stale; });
    rows->visible(0, 30, h.columns);
    h.settle();
    h.replyPage(pageJsonAt("another", 0, 200, 300, h.columns));  // page 0; page 1 goes next to it
    h.replyPage(pageJsonAt("another", 200, 200, 300, h.columns));
    CHECK(stale == 1);
    CHECK(rows->total().known);
    CHECK(rows->total().rows == 300);
    REQUIRE(rows->row(250).has_value());
}

// Mutation: a change with keys answered by `invalidate()` (no keyed fetch: `rowsFetches` is 0 and the pages go
// again).

TEST_CASE("window rows: a change with keys is a keyed fetch that patches the rows", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 300);  // rows 0 to 299 are held
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    std::size_t const before = h.dispatcher.count("Table.samples");
    h.change({5});
    CHECK(rows->stats().rowsFetches == 1);
    REQUIRE(h.dispatcher.count("Table.samples") == before + 1);
    CHECK(h.dispatcher.calls.back().body.contains(R"("keys":[5,0])"));  // the key and the probe of the held run
    h.replyPage(keyedJson({{0, 0}, {5, 5}}, 300, h.columns));
    REQUIRE(rows->row(5).has_value());
    CHECK(*rows->row(5)->row.field("name")->asString() == "renamed-5");
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].ops.size() == 2);                         // rows 0 and 5, each changed in place
    CHECK(h.dispatcher.count("Table.samples") == before + 1);  // no page went again
}

// Mutations: countsText ignoring `undecided`; countsText grouping with a fixed "," (the "de" line fails).

TEST_CASE("status line: counts for exact, lower-bound and undecided totals", "[ui][table]") {
    using enum table::TotalKind;
    FormatContext const en = formatFor("en");
    FormatContext const de = formatFor("de");
    REQUIRE(en.numeric.groupSeparator == ",");  // the helper took the locale table's row
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
    RowsFixture h;
    auto const source = std::make_shared<TestRowsSource>(500);
    EngineRows rows{source, morph::ui::testing::TestRowsSource::recordOf(), table::EngineOptions{.owner = &h.owner}};
    CHECK(rows.total().rows == 500);
    CHECK(rows.total().known);
    CHECK(rows.total().kind == table::TotalKind::Exact);
    rows.visible(0, 10, {});
    h.settle();  // names ten rows and no column
    REQUIRE(rows.row(499).has_value());
    CHECK(rows.row(499)->key == table::RowId{std::int64_t{499}});
    CHECK(*rows.row(499)->row.field("name")->asString() == "row-499");
    CHECK(rows.unloadedLevels(499).levels.empty());
    CHECK_FALSE(rows.row(500).has_value());
    CHECK(h.keysNow(rows, 100, 3) ==
          std::vector<table::RowId>{std::int64_t{100}, std::int64_t{101}, std::int64_t{102}});
    CHECK(rows.counts().undecided == 0);
    CHECK_FALSE(rows.counts().pending);
}

// Mutation: no memo (recordsBuilt is 2 after the two reads).

TEST_CASE("engine rows: a row's record is built once until the row changes", "[ui][table]") {
    RowsFixture h;
    auto const source = std::make_shared<TestRowsSource>(10);
    EngineRows rows{source, morph::ui::testing::TestRowsSource::recordOf(), table::EngineOptions{.owner = &h.owner}};
    static_cast<void>(rows.row(0));
    static_cast<void>(rows.row(0));
    CHECK(rows.recordsBuilt() == 1);
    source->rename(0, "renamed");
    h.settle();
    CHECK(*rows.row(0)->row.field("name")->asString() == "renamed");
    CHECK(rows.recordsBuilt() == 2);
    static_cast<void>(rows.row(1));
    CHECK(rows.recordsBuilt() == 3);  // another row: its own record
}

namespace {

// Rows 0 to 299 held in pages 0 and 1, both fetched again; the next reply answers page 0. Returns the view before it.

std::vector<std::string> refetchFirstPages(RowsFixture& h, WindowRows& rows) {
    h.answerFirstPage(rows, /*total*/ 300);
    rows.retry();
    h.settle();
    REQUIRE(h.pending("Table.samples") == 2);
    return morph::ui::testing::shownKeys(rows);
}

// Whether no key is held twice; an empty entry is a place not held.

bool keysUnique(std::vector<std::string> const& list) {
    std::vector<std::string> keys;
    std::ranges::copy_if(list, std::back_inserter(keys), [](std::string const& key) { return !key.empty(); });
    std::ranges::sort(keys);
    return std::ranges::adjacent_find(keys) == keys.end();
}

// Replays @p change on @p before as a keyed mount does: each operation must be in bounds of the list the one before
// it left, no key may be held twice after any of them, an inserted or changed place reads the new view's row, and
// the end must be what the rows hold now.

void checkReplay(std::vector<std::string> list, table::ViewChange const& change, WindowRows const& rows) {
    auto const after = morph::ui::testing::shownKeys(rows);
    for (std::size_t index = 0; index < change.ops.size(); ++index) {
        auto const& op = change.ops[index];
        INFO("op " << index << ": kind " << static_cast<int>(op.kind) << " first " << op.first << " count " << op.count
                   << " to " << op.to << " on a list of " << list.size());
        auto const first = static_cast<std::ptrdiff_t>(op.first);
        auto const count = static_cast<std::ptrdiff_t>(op.count);
        switch (op.kind) {
            case table::ViewOp::Kind::Removed:
                REQUIRE(op.first + op.count <= list.size());
                list.erase(list.begin() + first, list.begin() + first + count);
                break;
            case table::ViewOp::Kind::Moved: {
                REQUIRE(op.first < list.size());
                REQUIRE(op.to < list.size());
                std::string moved = list[op.first];
                list.erase(list.begin() + first);
                list.insert(list.begin() + static_cast<std::ptrdiff_t>(op.to), std::move(moved));
                break;
            }
            case table::ViewOp::Kind::Inserted:
                REQUIRE(op.first <= list.size());
                REQUIRE(op.first + op.count <= after.size());
                list.insert(list.begin() + first, after.begin() + first, after.begin() + first + count);
                break;
            case table::ViewOp::Kind::Changed:
                REQUIRE(op.first + op.count <= list.size());
                REQUIRE(op.first + op.count <= after.size());
                std::copy(after.begin() + first, after.begin() + first + count, list.begin() + first);
                break;
            case table::ViewOp::Kind::Reset:
            default:
                list = after;
                break;
        }
        CHECK(keysUnique(list));
    }
    CHECK(list == after);
}

bool has(table::ViewChange const& change, table::ViewOp::Kind kind) {
    return std::ranges::any_of(change.ops, [kind](table::ViewOp const& op) { return op.kind == kind; });
}

}  // namespace

// Mutation: identities by place rather than by key (no `Moved`; the replay keeps key 5 where it was).

TEST_CASE("window rows: a row that moves within its page is a move, not a reset", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    auto const before = refetchFirstPages(h, *rows);
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    std::vector<std::int64_t> keys;
    for (std::int64_t key = 0; key < 200; ++key) {
        if (key != 5) {
            keys.push_back(key);
        }
    }
    keys.insert(keys.begin() + 9, 5);  // key 5 now at view row 9
    h.replyPage(morph::ui::testing::pageJsonOfKeys(0, keys, 300, h.columns));
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].isReset());
    CHECK(has(changes[0], table::ViewOp::Kind::Moved));
    checkReplay(before, changes[0], *rows);
}

// Mutation: a new total resets the view.

TEST_CASE("window rows: a new total with the same keys moves the view's end, not a reset", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    auto const before = refetchFirstPages(h, *rows);
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    h.replyPage(pageJson(0, 200, 350, h.columns));  // the same rows; the view grew by 50
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].isReset());
    REQUIRE_FALSE(changes[0].ops.empty());
    CHECK(std::ranges::contains(
        changes[0].ops, table::ViewOp{.kind = table::ViewOp::Kind::Inserted, .first = 300, .count = 50, .to = 0}));
    CHECK_FALSE(has(changes[0], table::ViewOp::Kind::Moved));
    checkReplay(before, changes[0], *rows);
}

// Mutation: a page whose keys differ resets the view.

TEST_CASE("window rows: a row that came and a row that went are an insert and a remove", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    auto const before = refetchFirstPages(h, *rows);
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    std::vector<std::int64_t> keys;
    for (std::int64_t key = 0; key < 200; ++key) {
        if (key != 3) {
            keys.push_back(key);
        }
    }
    keys.insert(keys.begin() + 7, 1000);  // 3 went, 1000 came at view row 7
    h.replyPage(morph::ui::testing::pageJsonOfKeys(0, keys, 300, h.columns));
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].isReset());
    CHECK(has(changes[0], table::ViewOp::Kind::Removed));
    CHECK(has(changes[0], table::ViewOp::Kind::Inserted));
    checkReplay(before, changes[0], *rows);
}

// Mutation: `retry` clearing its flag when its own call returns (the held-back refetch goes with `retry: false`).

TEST_CASE("window rows: a retry held back by the awaited spacing still carries the flag", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    std::string const awaited = pageJson(0, 200, 200, h.columns, table::TotalKind::Exact, 0, {"recipe"});
    rows->visible(0, 30, h.columns);
    h.replyPage(awaited);
    h.change({0});
    h.replyPage(awaited);  // a refetch at 0 ms starts the spacing
    std::size_t const before = h.dispatcher.count("Table.samples");
    h.advance(10ms);
    rows->retry();
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == before);  // held back: 10 ms after the last refetch
    h.advance(240ms);
    REQUIRE(h.dispatcher.count("Table.samples") == before + 1);
    CHECK(h.dispatcher.calls.back().body.contains(R"("retry":true)"));
}

// Mutation: an unreadable keyed reply only setting `error()` (no page goes again).

TEST_CASE("window rows: a keyed reply that does not read fetches the pages again", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 300);
    h.change({5});
    REQUIRE(rows->stats().rowsFetches == 1);
    std::size_t const before = h.dispatcher.count("Table.samples");
    h.replyPage("not a page");
    CHECK(rows->error().has_value());
    CHECK(h.dispatcher.count("Table.samples") == before + 2);  // pages 0 and 1, held and now stale
}

// Mutation: the view's end removed after the page's diff rather than before it (rows 250 to 299 are held twice while
// the page's rows move in).

TEST_CASE("window rows: a page that spans a shrunk view's end gives operations in bounds", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 300);
    rows->retry();
    h.settle();
    h.replyPage(pageJson(0, 200, 300, h.columns));  // page 0 again, as it was
    auto const before = morph::ui::testing::shownKeys(*rows);
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    std::vector<std::int64_t> keys;
    for (std::int64_t key = 250; key < 300; ++key) {
        keys.push_back(key);
    }
    h.replyPage(morph::ui::testing::pageJsonOfKeys(200, keys, 250, h.columns));  // keys 200 to 249 were deleted
    REQUIRE_FALSE(changes.empty());
    CHECK_FALSE(changes[0].isReset());
    CHECK(rows->total().rows == 250);
    checkReplay(before, changes[0], *rows);
}

// Mutation: the view's end inserted before the page's diff rather than after it (rows 250 to 299 are held twice
// until the page's rows move out).

TEST_CASE("window rows: a page that spans a grown view's end never holds a key twice", "[ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 300);
    rows->retry();
    h.settle();
    h.replyPage(pageJson(0, 200, 300, h.columns));  // page 0 again, as it was
    auto const before = morph::ui::testing::shownKeys(*rows);
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    std::vector<std::int64_t> keys;
    for (std::int64_t key = 1000; key < 1050; ++key) {
        keys.push_back(key);
    }
    for (std::int64_t key = 200; key < 300; ++key) {
        keys.push_back(key);
    }
    h.replyPage(morph::ui::testing::pageJsonOfKeys(200, keys, 350, h.columns));  // 50 rows came at view row 200
    REQUIRE_FALSE(changes.empty());
    CHECK_FALSE(changes[0].isReset());
    CHECK(rows->total().rows == 350);
    checkReplay(before, changes[0], *rows);
}

// Mutation: a lowering that needs the node's `columns` (no header: `headerLabels` is empty).

TEST_CASE("model table: a client given only the catalog draws its columns, sorts and pages it", "[ui][table]") {
    TableHarness h;
    auto const screen = h.mount(kModelTable);
    CHECK(h.dispatcher.count("Table.samples.describe") == 1);
    CHECK(h.dispatcher.count("Table.samples") == 0);  // no page before its description
    replyDescribe(h);
    REQUIRE(h.dispatcher.count("Table.samples") == 1);
    answerPages(h, 9093);
    CHECK(headerLabels(h) == std::vector<std::string>{"Id", "Name", "C3"});
    CHECK(h.backend.find("Text", "text", "row-0").has_value());  // the default cell, by its text
    CHECK(statusOf(h).counts == "9,093 rows");
    CHECK(statusOf(h).shown);
    h.backend.clickHeader(tableOf(h), 1);
    h.settle();
    CHECK(lastQuery(h).sort == table::SortChain{{.column = "name"}});
    answerPages(h, 9093);
    std::size_t const scrolled = h.dispatcher.calls.size();
    h.backend.scrollTable(tableOf(h), 4'000, 30);
    h.advance(80ms);
    CHECK(askedForPage(h, scrolled, 4'000));                   // the window pages
    CHECK(h.dispatcher.count("Table.samples.describe") == 1);  // W6: one request
}

// Mutation: a literal `filters` read as an expression (it evaluates to null and the query carries no filter).

TEST_CASE("model table: a literal filter and the element values reach the query and the document", "[ui][table]") {
    TableHarness h;
    auto const screen =
        h.mount(withTableKey(kModelTable, R"("filters":{"columns":{"name":{"include":[{"contains":"a"}]}}})"));
    replyDescribe(h);
    REQUIRE(h.dispatcher.count("Table.samples") == 1);
    REQUIRE(lastQuery(h).filters.columns.contains("name"));
    CHECK(lastQuery(h).filters.columns.at("name").include.size() == 1);
    CHECK(elementValue(*screen, "list", "pending") == "true");
    answerPages(h, 9093, table::TotalKind::AtLeast, /*undecided*/ 40, {"recipe"});
    CHECK(elementValue(*screen, "list", "viewCount") == "9093");
    CHECK(elementValue(*screen, "list", "viewCountKind") == R"("atLeast")");
    CHECK(elementValue(*screen, "list", "undecided") == "40");
    CHECK(elementValue(*screen, "list", "awaiting") == R"(["recipe"])");
    CHECK(elementValue(*screen, "list", "failed") == "[]");
    CHECK(elementValue(*screen, "list", "pending") == "false");
}

// Mutation: a table that ignores its store sends the default query (the count is 1 before `release`).  Gate T3.

TEST_CASE("model table: persisted state before the first query", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kStored});
    TableHarness h{store};
    auto const screen = h.mount(kModelTable);
    replyDescribe(h);
    CHECK(h.dispatcher.count("Table.samples") == 0);
    CHECK(store->lastLoadKey() == table::TableStateKey{.application = "lims",
                                                       .model = "Lims_SampleModel",
                                                       .table = "samples",
                                                       .qualifier = "samples",
                                                       .part = "state"});
    store->release();
    h.settle();
    REQUIRE(h.dispatcher.count("Table.samples") == 1);
    CHECK(lastQuery(h).sort == table::SortChain{{.column = "name", .dir = table::SortDirection::Descending}});
    CHECK(lastQuery(h).filters.columns.contains("name"));
}

// Mutations: no debounce (fifty saves); unmount not flushing (the count stays 3); a change during a save dropped
// (the third save never goes).

TEST_CASE("model table: saves are coalesced, and unmounting saves at once", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    auto screen = h.mount(kModelTable);
    replyDescribe(h);
    store->release();
    h.settle();
    for (int i = 0; i < 50; ++i) {
        h.backend.clickHeader(tableOf(h), i % 2 == 0 ? 0 : 1);
        h.advance(5ms);
    }
    CHECK(store->saves() == 0);  // inside the debounce
    h.advance(500ms);
    CHECK(store->saves() == 1);
    CHECK(table::readState(store->lastSaved())->sort->at(0).column == "name");  // the last of the fifty
    store->holdSaves();
    h.backend.clickHeader(tableOf(h), 0);
    h.advance(500ms);
    CHECK(store->saves() == 2);  // in flight, held
    h.backend.clickHeader(tableOf(h), 1);
    h.advance(500ms);
    CHECK(store->saves() == 2);  // one per key in flight
    store->releaseSave();
    h.settle();
    CHECK(store->saves() == 3);  // one more after it
    store->releaseSave();
    h.settle();
    h.backend.clickHeader(tableOf(h), 0);
    screen.reset();
    h.settle();
    CHECK(store->saves() == 4);  // at once, no debounce
}

// Mutations: applying a late state after the user scrolled (a query more in the second section); a failed load
// reported per table mount or not at all; a failed load that stops saving.

TEST_CASE("model table: a slow or failed load", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kStored});
    TableHarness h{store};
    auto const screen = h.mount(kModelTable);
    replyDescribe(h);
    h.advance(299ms);
    CHECK(h.dispatcher.count("Table.samples") == 0);
    h.advance(1ms);
    REQUIRE(h.dispatcher.count("Table.samples") >= 1);  // defaults after tableStateTimeout
    CHECK(queryOf(h, h.dispatcher.calls.size() - 1).sort.empty());
    answerPages(h, 9093);
    std::size_t const shown = h.dispatcher.count("Table.samples");
    std::size_t const calls = h.dispatcher.calls.size();
    SECTION("the late state is applied") {
        store->release();
        h.settle();
        REQUIRE(h.dispatcher.count("Table.samples") > shown);  // one more query, for the pages in view
        CHECK(queryOf(h, calls).sort.at(0).column == "name");
    }
    SECTION("not under the user's hands") {
        h.backend.scrollTable(tableOf(h), 40, 30);
        h.settle();  // within the held pages: no fetch
        store->release();
        h.settle();
        CHECK(h.dispatcher.count("Table.samples") == shown);
        CHECK(lastQuery(h).sort.empty());
    }
    SECTION("a failed load") {
        store->fail();
        h.settle();
        CHECK(diagnosticsMentioning(h, "state") == 1);  // reported once
        CHECK(h.dispatcher.count("Table.samples") == shown);
        h.backend.clickHeader(tableOf(h), 1);
        h.advance(500ms);
        CHECK(store->saves() == 1);  // changes still save
    }
}

// Mutations: applying a stored sort over a bound one (the query sorts by name); saving a bound sort (the saved
// document has a `sort`); `persist: false` still loading.

TEST_CASE("model table: bound values win", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kStored});
    TableHarness h{store};
    SECTION("a bound sort") {
        auto screen = h.mount(kBoundSort);
        replyDescribe(h);
        store->release();
        h.settle();
        REQUIRE(h.dispatcher.count("Table.samples") == 1);
        CHECK(lastQuery(h).sort == table::SortChain{{.column = "id"}});  // the document's
        CHECK(lastQuery(h).filters.columns.contains("name"));            // the stored filter still applies
        h.backend.clickHeader(tableOf(h), 1);
        h.advance(500ms);
        screen.reset();
        h.settle();
        CHECK(store->saves() == 0);  // a bound sort is not saved
    }
    SECTION("persist: false") {
        auto screen = h.mount(kNoPersist);
        replyDescribe(h);
        h.settle();
        CHECK(store->loads() == 0);
        REQUIRE(h.dispatcher.count("Table.samples") == 1);  // no restoring state
        h.backend.clickHeader(tableOf(h), 1);
        h.advance(500ms);
        screen.reset();
        h.settle();
        CHECK(store->saves() == 0);
    }
}

// Review Focus 3, the table's half. Mutations: no report (the count is 0); the stored text saved back unchanged.

TEST_CASE("model table: a stored state that does not read starts from the defaults", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":2,"junk":true})");
    TableHarness h{store};
    auto const screen = h.mount(kModelTable);
    replyDescribe(h);
    store->release();
    h.settle();
    REQUIRE(h.dispatcher.count("Table.samples") == 1);
    CHECK(lastQuery(h).sort.empty());
    CHECK(diagnosticsMentioning(h, "the stored state of table `list` is not used") == 1);
    h.advance(1'000ms);
    CHECK(store->saves() == 0);  // nothing written until a change
    h.backend.clickHeader(tableOf(h), 1);
    h.advance(500ms);
    REQUIRE(store->saves() == 1);
    CHECK_FALSE(store->lastSaved().contains("junk"));
    CHECK(table::readState(store->lastSaved())->version == table::kStateVersion);
}

// Invariant 31. Mutation: `onSaveFailed` not wired to `tableMessage` (the message stays empty).

TEST_CASE("model table: a refused save is said in the status line and keeps the arrangement", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    auto const screen = h.mount(kModelTable);
    replyDescribe(h);
    store->release();
    h.settle();
    store->refuseSaves("over the table-state quota");
    h.backend.clickHeader(tableOf(h), 1);
    h.advance(500ms);
    CHECK(statusOf(h).message == "The table's arrangement could not be saved: over the table-state quota");
    CHECK(lastQuery(h).sort.at(0).column == "name");  // the arrangement stays
    CHECK(h.backend.prop(tableOf(h), "sort") == "1:Ascending");
}

namespace {

// `kModelTable` with a `columns` point `cols` and a `saved` point `views`, both naming the table.

std::string withTablePoints() {
    std::string text{kModelTable};
    std::string const models = R"("models":)";
    text.insert(text.find(models),
                R"("customize":{"cols":{"kind":"columns","of":"list"},"views":{"kind":"saved","of":"list"}},)");
    return text;
}

}  // namespace

// Mutations: the stored state's layout and view not shown at the points (`cols.pinned` reads null); a point's choice
// not written into the table's state (no save goes).

TEST_CASE("model table: the columns and saved points read and write the table's state", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(
        storeExecutor(),
        R"({"version":1,"layout":{"order":["id","name","c3"],"hidden":["c3"],"pinned":1},"view":"Mine"})");
    TableHarness h{store};
    auto screen = h.mount(withTablePoints());
    replyDescribe(h);
    store->release();
    h.settle();
    CHECK(readJson(*screen, R"({"pref":"cols.pinned"})") == "1");
    CHECK(readJson(*screen, R"({"pref":"cols.hidden"})") == R"(["c3"])");
    CHECK(readJson(*screen, R"({"pref":"views"})") == R"("Mine")");
    auto const layout = morph::ui::parseJson(R"({"order":["name","id","c3"],"hidden":[],"pinned":2})");
    REQUIRE(layout.has_value());
    CHECK(screen->setPreference("cols", *morph::ui::valueFromJson(*layout)));
    CHECK(screen->setPreference("views", morph::ui::Value{std::string{"Shared"}}));
    CHECK_FALSE(screen->setPreference("cols", morph::ui::Value{std::int64_t{7}}));  // not a layout
    h.advance(500ms);
    REQUIRE(store->saves() == 1);
    auto const saved = table::readState(store->lastSaved());
    REQUIRE(saved.has_value());
    REQUIRE(saved->layout.has_value());
    CHECK(saved->layout->pinned == 2);
    CHECK(saved->layout->order.front() == "name");
    CHECK(saved->view == std::optional<std::string>{"Shared"});
    CHECK(readJson(*screen, R"({"pref":"cols.pinned"})") == "2");
}
