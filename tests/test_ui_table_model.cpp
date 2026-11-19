// SPDX-License-Identifier: Apache-2.0

// The client table model: `EngineRows` over the engine, `WindowRows` over a server's pages, the status line's counts,
// and the model-backed `table` node on a screen (docs/spec/table/table.md, "The client table model", "The window
// under changes", "The status line", "The document `table` node on the engine", "Persisted state").
//
// Each case names the mutation it was seen to fail on.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <morph/ui/mount.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "table/wide_table_model.hpp"
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
using morph::ui::testing::keyedJsonAt;
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
        INFO("op " << index << ": kind " << static_cast<int>(op.kind) << " first " << op.first << " count "
                   << op.count << " to " << op.to << " on a list of " << list.size());
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
    RowsFixture h;  auto const rows = h.windowRows("samples");
    auto const before = refetchFirstPages(h, *rows);
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    std::vector<std::int64_t> keys;
    for (std::int64_t key = 0; key < 200; ++key) { if (key != 5) { keys.push_back(key); } }
    keys.insert(keys.begin() + 9, 5);                                // key 5 now at view row 9
    h.replyPage(morph::ui::testing::pageJsonOfKeys(0, keys, 300, h.columns));
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].isReset());
    CHECK(has(changes[0], table::ViewOp::Kind::Moved));
    checkReplay(before, changes[0], *rows);
}

// Mutation: a new total resets the view.
TEST_CASE("window rows: a new total with the same keys moves the view's end, not a reset", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    auto const before = refetchFirstPages(h, *rows);
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    h.replyPage(pageJson(0, 200, 350, h.columns));                   // the same rows; the view grew by 50
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].isReset());
    REQUIRE_FALSE(changes[0].ops.empty());
    CHECK(std::ranges::contains(changes[0].ops,
                                table::ViewOp{.kind = table::ViewOp::Kind::Inserted, .first = 300, .count = 50, .to = 0}));
    CHECK_FALSE(has(changes[0], table::ViewOp::Kind::Moved));
    checkReplay(before, changes[0], *rows);
}

// Mutation: a page whose keys differ resets the view.
TEST_CASE("window rows: a row that came and a row that went are an insert and a remove", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    auto const before = refetchFirstPages(h, *rows);
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    std::vector<std::int64_t> keys;
    for (std::int64_t key = 0; key < 200; ++key) { if (key != 3) { keys.push_back(key); } }
    keys.insert(keys.begin() + 7, 1000);                             // 3 went, 1000 came at view row 7
    h.replyPage(morph::ui::testing::pageJsonOfKeys(0, keys, 300, h.columns));
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].isReset());
    CHECK(has(changes[0], table::ViewOp::Kind::Removed));
    CHECK(has(changes[0], table::ViewOp::Kind::Inserted));
    checkReplay(before, changes[0], *rows);
}

// Mutation: `retry` clearing its flag when its own call returns (the held-back refetch goes with `retry: false`).
TEST_CASE("window rows: a retry held back by the awaited spacing still carries the flag", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    std::string const awaited = pageJson(0, 200, 200, h.columns, table::TotalKind::Exact, 0, {"recipe"});
    rows->visible(0, 30, h.columns);  h.replyPage(awaited);
    h.change({0});  h.replyPage(awaited);                            // a refetch at 0 ms starts the spacing
    std::size_t const before = h.dispatcher.count("Table.samples");
    h.advance(10ms);  rows->retry();  h.settle();
    CHECK(h.dispatcher.count("Table.samples") == before);            // held back: 10 ms after the last refetch
    h.advance(240ms);
    REQUIRE(h.dispatcher.count("Table.samples") == before + 1);
    CHECK(h.dispatcher.calls.back().body.contains(R"("retry":true)"));
}

// Mutation: an unreadable keyed reply only setting `error()` (no page goes again).
TEST_CASE("window rows: a keyed reply that does not read fetches the pages again", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 300);
    h.change({5});
    REQUIRE(rows->stats().rowsFetches == 1);
    std::size_t const before = h.dispatcher.count("Table.samples");
    h.replyPage("not a page");
    CHECK(rows->error().has_value());
    CHECK(h.dispatcher.count("Table.samples") == before + 2);        // pages 0 and 1, held and now stale
}

// Mutations: `Runs` not admitting an action that runs itself (`CatalogFacts::action` of `Table.small` does not
// compile); `recordsInfo` dropping `opaque` (the catalog's record of `Table.small` is not opaque).
TEST_CASE("table catalog: a table's actions are catalogued, holding the record opaquely", "[ui][table]") {
    morph::ui::author::CatalogFacts facts;
    facts.action<bench::WideTableModel, table::QueryTable<&bench::WideTableModel::small>>();
    facts.action<bench::WideTableModel, table::DescribeTable<&bench::WideTableModel::small>>();
    morph::ui::Catalog const catalog = facts.catalog();
    auto const query = catalog.action("Table.small");
    REQUIRE(query.has_value());
    CHECK(query->modelType == "WideTableModel");
    REQUIRE(query->records.size() == 1);
    CHECK(query->records.front().record == "WideTableModel.small");
    CHECK(query->records.front().opaque);
    CHECK(query->records.front().paths.empty());
    auto const describe = catalog.action("Table.small.describe");
    REQUIRE(describe.has_value());
    CHECK(describe->modelType == "WideTableModel");
    CHECK(describe->records.empty());
}

// A table's actions only read: the builder's `evaluate` and `mutation` refuse them at compile time, while `query`
// and the catalog take them.
template <typename Action>
concept TableEvaluatable = requires(morph::ui::author::DeclScope& scope,
                                    morph::ui::author::ModelHandle<bench::WideTableModel> const& model) {
    scope.evaluate<Action>("e", model);
};
template <typename Action>
concept TableMutable = requires(morph::ui::author::DeclScope& scope,
                                morph::ui::author::ModelHandle<bench::WideTableModel> const& model) {
    scope.mutation<Action>("m", model);
};
template <typename Action>
concept TableQueryable = requires(morph::ui::author::DeclScope& scope,
                                  morph::ui::author::ModelHandle<bench::WideTableModel> const& model) {
    scope.query<Action>("q", model);
};

// Mutation: `evaluate` and `mutation` constrained by `Runs` (both static_asserts on them fail to hold).
TEST_CASE("table catalog: a table's actions are no builder's evaluate or mutation", "[ui][table]") {
    using Query = table::QueryTable<&bench::WideTableModel::small>;
    STATIC_CHECK_FALSE(TableEvaluatable<Query>);
    STATIC_CHECK_FALSE(TableMutable<Query>);
    STATIC_CHECK(TableQueryable<Query>);
}

// Mutation: the view's end removed after the page's diff rather than before it (rows 250 to 299 are held twice while
// the page's rows move in).
TEST_CASE("window rows: a page that spans a shrunk view's end gives operations in bounds", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 300);
    rows->retry();  h.settle();
    h.replyPage(pageJson(0, 200, 300, h.columns));                   // page 0 again, as it was
    auto const before = morph::ui::testing::shownKeys(*rows);
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    std::vector<std::int64_t> keys;
    for (std::int64_t key = 250; key < 300; ++key) { keys.push_back(key); }
    h.replyPage(morph::ui::testing::pageJsonOfKeys(200, keys, 250, h.columns));   // keys 200 to 249 were deleted
    REQUIRE_FALSE(changes.empty());
    CHECK_FALSE(changes[0].isReset());
    CHECK(rows->total().rows == 250);
    checkReplay(before, changes[0], *rows);
}

// Mutation: the view's end inserted before the page's diff rather than after it (rows 250 to 299 are held twice
// until the page's rows move out).
TEST_CASE("window rows: a page that spans a grown view's end never holds a key twice", "[ui][table]") {
    RowsFixture h;  auto const rows = h.windowRows("samples");
    h.answerFirstPage(*rows, /*total*/ 300);
    rows->retry();  h.settle();
    h.replyPage(pageJson(0, 200, 300, h.columns));                   // page 0 again, as it was
    auto const before = morph::ui::testing::shownKeys(*rows);
    std::vector<table::ViewChange> changes;
    rows->onChange([&changes](table::ViewChange const& change) { changes.push_back(change); });
    std::vector<std::int64_t> keys;
    for (std::int64_t key = 1000; key < 1050; ++key) { keys.push_back(key); }
    for (std::int64_t key = 200; key < 300; ++key) { keys.push_back(key); }
    h.replyPage(morph::ui::testing::pageJsonOfKeys(200, keys, 350, h.columns));   // 50 rows came at view row 200
    REQUIRE_FALSE(changes.empty());
    CHECK_FALSE(changes[0].isReset());
    CHECK(rows->total().rows == 350);
    checkReplay(before, changes[0], *rows);
}

// ── The model-backed table on a screen ──────────────────────────────────────

namespace {

using morph::ui::CellLoad;
using morph::ui::Key;
using morph::ui::MountStats;
using morph::ui::testing::answerPages;
using morph::ui::testing::change;
using morph::ui::testing::coreOf;
using morph::ui::testing::diagnosticsMentioning;
using morph::ui::testing::elementOf;
using morph::ui::testing::elementValue;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::headerLabels;
using morph::ui::testing::kBoundSort;
using morph::ui::testing::keyedJson;
using morph::ui::testing::kModelTable;
using morph::ui::testing::kNoPersist;
using morph::ui::testing::kNoStatus;
using morph::ui::testing::kStored;
using morph::ui::testing::lastQuery;
using morph::ui::testing::mountShown;
using morph::ui::testing::openCalls;
using morph::ui::testing::oldestOpen;
using morph::ui::testing::pageJsonRenaming;
using morph::ui::testing::pageJsonUnloaded;
using morph::ui::testing::queryOf;
using morph::ui::testing::readJson;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::replyPage;
using morph::ui::testing::shownText;
using morph::ui::testing::statsOf;
using morph::ui::testing::statusOf;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;
using morph::ui::testing::tableOf;
using morph::ui::testing::withLevel;
using morph::ui::testing::withTableKey;

// The revision of the description the harness answered last.
std::string revisionOf(TableHarness const& h) { return h.described.revision; }

// Whether a `Table.samples` call from @p first on asks for the page at @p offset.
bool askedForPage(TableHarness const& h, std::size_t first, std::int64_t offset) {
    for (std::size_t index = first; index < h.dispatcher.calls.size(); ++index) {
        if (h.dispatcher.calls[index].action == "Table.samples" && queryOf(h, index).page.offset == offset) {
            return true;
        }
    }
    return false;
}

}  // namespace

// Mutation: a lowering that needs the node's `columns` (no header: `headerLabels` is empty).
TEST_CASE("model table: a client given only the catalog draws its columns, sorts and pages it", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(kModelTable);
    CHECK(h.dispatcher.count("Table.samples.describe") == 1);
    CHECK(h.dispatcher.count("Table.samples") == 0);                          // no page before its description
    replyDescribe(h);
    REQUIRE(h.dispatcher.count("Table.samples") == 1);
    answerPages(h, 9093);
    CHECK(headerLabels(h) == std::vector<std::string>{"Id", "Name", "C3"});
    CHECK(h.backend.find("Text", "text", "row-0").has_value());               // the default cell, by its text
    CHECK(statusOf(h).counts == "9,093 rows");  CHECK(statusOf(h).shown);
    h.backend.clickHeader(tableOf(h), 1);  h.settle();
    CHECK(lastQuery(h).sort == table::SortChain{{.column = "name"}});
    answerPages(h, 9093);
    std::size_t const scrolled = h.dispatcher.calls.size();
    h.backend.scrollTable(tableOf(h), 4'000, 30);  h.advance(80ms);
    CHECK(askedForPage(h, scrolled, 4'000));                                  // the window pages
    CHECK(h.dispatcher.count("Table.samples.describe") == 1);                 // W6: one request
}

// Mutation: a literal `filters` read as an expression (it evaluates to null and the query carries no filter).
TEST_CASE("model table: a literal filter and the element values reach the query and the document", "[ui][table]") {
    TableHarness h;
    auto screen = h.mount(withTableKey(kModelTable, R"("filters":{"columns":{"name":{"include":[{"contains":"a"}]}}})"));
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
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kStored});
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);
    CHECK(h.dispatcher.count("Table.samples") == 0);
    CHECK(store->lastLoadKey() == table::TableStateKey{.application = "lims", .model = "Lims_SampleModel",
                                                       .table = "samples", .qualifier = "samples", .part = "state"});
    store->release();  h.settle();
    REQUIRE(h.dispatcher.count("Table.samples") == 1);
    CHECK(lastQuery(h).sort == table::SortChain{{.column = "name", .dir = table::SortDirection::Descending}});
    CHECK(lastQuery(h).filters.columns.contains("name"));
}

// Mutations: no debounce (fifty saves); unmount not flushing (the count stays 3); a change during a save dropped
// (the third save never goes).
TEST_CASE("model table: saves are coalesced, and unmounting saves at once", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);  store->release();  h.settle();
    for (int i = 0; i < 50; ++i) { h.backend.clickHeader(tableOf(h), i % 2 == 0 ? 0 : 1);  h.advance(5ms); }
    CHECK(store->saves() == 0);                                               // inside the debounce
    h.advance(500ms);  CHECK(store->saves() == 1);
    CHECK(table::readState(store->lastSaved())->sort->at(0).column == "name");   // the last of the fifty
    store->holdSaves();
    h.backend.clickHeader(tableOf(h), 0);  h.advance(500ms);  CHECK(store->saves() == 2);   // in flight, held
    h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);  CHECK(store->saves() == 2);   // one per key in flight
    store->releaseSave();  h.settle();  CHECK(store->saves() == 3);           // one more after it
    store->releaseSave();  h.settle();
    h.backend.clickHeader(tableOf(h), 0);  screen.reset();  h.settle();
    CHECK(store->saves() == 4);                                               // at once, no debounce
}

// Mutations: applying a late state after the user scrolled (a query more in the second section); a failed load
// reported per table mount or not at all; a failed load that stops saving.
TEST_CASE("model table: a slow or failed load", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kStored});
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);
    h.advance(299ms);  CHECK(h.dispatcher.count("Table.samples") == 0);
    h.advance(1ms);    REQUIRE(h.dispatcher.count("Table.samples") >= 1);     // defaults after tableStateTimeout
    CHECK(queryOf(h, h.dispatcher.calls.size() - 1).sort.empty());
    answerPages(h, 9093);
    std::size_t const shown = h.dispatcher.count("Table.samples");
    std::size_t const calls = h.dispatcher.calls.size();
    SECTION("the late state is applied") {
        store->release();  h.settle();
        REQUIRE(h.dispatcher.count("Table.samples") > shown);                // one more query, for the pages in view
        CHECK(queryOf(h, calls).sort.at(0).column == "name");
    }
    SECTION("not under the user's hands") {
        h.backend.scrollTable(tableOf(h), 40, 30);  h.settle();               // within the held pages: no fetch
        store->release();  h.settle();
        CHECK(h.dispatcher.count("Table.samples") == shown);
        CHECK(lastQuery(h).sort.empty());
    }
    SECTION("a failed load") {
        store->fail();  h.settle();
        CHECK(diagnosticsMentioning(h, "state") == 1);                        // reported once
        CHECK(h.dispatcher.count("Table.samples") == shown);
        h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);
        CHECK(store->saves() == 1);                                           // changes still save
    }
}

// Mutations: applying a stored sort over a bound one (the query sorts by name); saving a bound sort (the saved
// document has a `sort`); `persist: false` still loading.
TEST_CASE("model table: bound values win", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kStored});
    TableHarness h{store};
    SECTION("a bound sort") {
        auto screen = h.mount(kBoundSort);  replyDescribe(h);  store->release();  h.settle();
        REQUIRE(h.dispatcher.count("Table.samples") == 1);
        CHECK(lastQuery(h).sort == table::SortChain{{.column = "id"}});      // the document's
        CHECK(lastQuery(h).filters.columns.contains("name"));                 // the stored filter still applies
        h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);
        screen.reset();  h.settle();  CHECK(store->saves() == 0);             // a bound sort is not saved
    }
    SECTION("persist: false") {
        auto screen = h.mount(kNoPersist);  replyDescribe(h);  h.settle();
        CHECK(store->loads() == 0);
        REQUIRE(h.dispatcher.count("Table.samples") == 1);                    // no restoring state
        h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);  screen.reset();  h.settle();
        CHECK(store->saves() == 0);
    }
}

// Review Focus 3, the table's half. Mutations: no report (the count is 0); the stored text saved back unchanged.
TEST_CASE("model table: a stored state that does not read starts from the defaults", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":2,"junk":true})");
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);  store->release();  h.settle();
    REQUIRE(h.dispatcher.count("Table.samples") == 1);  CHECK(lastQuery(h).sort.empty());
    CHECK(diagnosticsMentioning(h, "the stored state of table `list` is not used") == 1);
    h.advance(1'000ms);  CHECK(store->saves() == 0);                          // nothing written until a change
    h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);
    REQUIRE(store->saves() == 1);
    CHECK_FALSE(store->lastSaved().contains("junk"));
    CHECK(table::readState(store->lastSaved())->version == table::kStateVersion);
}

// Invariant 31. Mutation: `onSaveFailed` not wired to `tableMessage` (the message stays empty).
TEST_CASE("model table: a refused save is said in the status line and keeps the arrangement", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);  store->release();  h.settle();
    store->refuseSaves("over the table-state quota");
    h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);
    CHECK(statusOf(h).message == "The table's arrangement could not be saved: over the table-state quota");
    CHECK(lastQuery(h).sort.at(0).column == "name");                          // the arrangement stays
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
    auto store = std::make_shared<GatedTableStateStore>(
        storeExecutor(), R"({"version":1,"layout":{"order":["id","name","c3"],"hidden":["c3"],"pinned":1},"view":"Mine"})");
    TableHarness h{store};
    auto screen = h.mount(withTablePoints());  replyDescribe(h);  store->release();  h.settle();
    CHECK(readJson(*screen, R"({"pref":"cols.pinned"})") == "1");
    CHECK(readJson(*screen, R"({"pref":"cols.hidden"})") == R"(["c3"])");
    CHECK(readJson(*screen, R"({"pref":"views"})") == R"("Mine")");
    auto const layout = morph::ui::parseJson(R"({"order":["name","id","c3"],"hidden":[],"pinned":2})");
    REQUIRE(layout.has_value());
    CHECK(screen->setPreference("cols", *morph::ui::valueFromJson(*layout)));
    CHECK(screen->setPreference("views", morph::ui::Value{std::string{"Shared"}}));
    CHECK_FALSE(screen->setPreference("cols", morph::ui::Value{std::int64_t{7}}));   // not a layout
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

// Mutation: the row-scope index never pruned (it holds an entry for every row scrolled past).
TEST_CASE("model table: scrolling 10,000 rows keeps one row scope per realized row", "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, withTableKey(kModelTable, R"("rowLabel":{"lit":"a row"})"), 100'000);
    for (std::size_t first = 0; first < 10'000; first += 30) {
        h.backend.scrollTable(tableOf(h), first, 30);  h.settle();
        answerPages(h, 100'000);
    }
    // The realized rows, and at most one scroll's worth that left and are pruned when the next row comes.
    CHECK(morph::ui::detail::ScreenCore::tableRowScopes(elementOf(*screen, "list")) <= 2 * 50);
}

// Mutation: a new revision clearing the element's selection (`selectedCount` is 0 after it).
TEST_CASE("model table: a new revision keeps the selection and the message", "[ui][table]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    auto screen = h.mount(withTableKey(kModelTable, R"("selectionMode":"multiple")"));
    replyDescribe(h);  store->release();  h.settle();  answerPages(h, 9093);
    h.backend.selectRows(tableOf(h), {Key{std::int64_t{1}}, Key{std::int64_t{2}}});  h.settle();
    store->refuseSaves("over the table-state quota");
    h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);
    REQUIRE(statusOf(h).message == "The table's arrangement could not be saved: over the table-state quota");
    table::TableDescription const b = samplesDescription(2);
    h.reply(oldestOpen(h, "Table.samples"), morph::ui::testing::pageJsonAt(b.revision, 0, 200, 9093, h.columns));
    replyDescribe(h, b);
    REQUIRE(headerLabels(h) == std::vector<std::string>{"Id", "Name"});      // mounted again from revision B
    CHECK(elementValue(*screen, "list", "selectedCount") == "2");
    CHECK(statusOf(h).message == "The table's arrangement could not be saved: over the table-state quota");
}

// Mutation: the body's `query` members replaced without a report (no diagnostic names `filters`).
TEST_CASE("model table: a body's query member the table owns is reported", "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(
        h, withTableKey(kModelTable, R"("body":{"lit":{"query":{"filters":{"columns":{}},"limit":3}}})"));
    CHECK(diagnosticsMentioning(h, "`filters` is the table's own; the body's is not used") == 1);
    CHECK(diagnosticsMentioning(h, "limit") == 0);
}


namespace {

// `kModelTable` with `"selectionMode": "multiple"`.
std::string selectTable() { return withTableKey(kModelTable, R"("selectionMode":"multiple")"); }

// `kModelTable` with a state `hk`, null, as the table's `highlightKey`, and three buttons that set it to 5012, 77 and
// null.
std::string const kHighlight =
    R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},)"
    R"("state":{"hk":{"init":{"lit":null}}},)"
    R"("view":{"kind":"column","children":[)"
    R"({"kind":"button","label":"5012","testId":"to5012","onClick":[{"set":"hk","value":5012}]},)"
    R"({"kind":"button","label":"77","testId":"to77","onClick":[{"set":"hk","value":77}]},)"
    R"({"kind":"button","label":"none","testId":"toNone","onClick":[{"set":"hk","value":{"lit":null}}]},)"
    R"({"kind":"table","id":"list","testId":"list","model":"lab","table":"samples","highlightKey":{"ref":"hk"},)"
    R"("empty":[{"kind":"text","text":"No samples"}]}]}})";

// Clicks the button with @p testId, then settles.
void click(TableHarness& h, std::string_view testId) {
    h.backend.click(h.byTestId("Button", testId));
    h.settle();
}

// Answers every open `Table.samples` call that names no columns with a page of keys alone, at the offset and limit
// its query names, of a view of 100,000 rows; settles after each.
void replyKeyPages(TableHarness& h) {
    for (int round = 0; round < 64 && openCalls(h, "Table.samples") > 0; ++round) {
        std::optional<std::size_t> found;
        for (std::size_t index = 0; index < h.dispatcher.calls.size() && !found; ++index) {
            auto const& call = h.dispatcher.calls[index];
            if (call.action == "Table.samples" && !call.answered && !call.options.stop.stop_requested() &&
                queryOf(h, index).columns == std::optional<std::vector<std::string>>{std::vector<std::string>{}}) {
                found = index;
            }
        }
        if (!found) {
            return;
        }
        table::TableQuery const query = queryOf(h, *found);
        h.reply(*found, morph::ui::testing::pageJson(query.page.offset, query.page.limit, 100'000, {}));
    }
}

// Answers the oldest open keyed `Table.samples` call with @p rows (key and view row), then settles.
void replyKeyed(TableHarness& h, std::vector<std::pair<std::int64_t, std::int64_t>> const& rows) {
    for (std::size_t index = 0; index < h.dispatcher.calls.size(); ++index) {
        auto const& call = h.dispatcher.calls[index];
        if (call.action == "Table.samples" && !call.answered && !call.options.stop.stop_requested() &&
            queryOf(h, index).keys.has_value()) {
            h.reply(index, keyedJson(rows, 9093, {}));
            return;
        }
    }
    FAIL("no open keyed call");
}

// Whether the call at @p index was stopped.
bool stopped(TableHarness const& h, std::size_t index) {
    return h.dispatcher.calls.at(index).options.stop.stop_requested();
}

}  // namespace

// Mutation: `keysOf` fetching the visible columns (a call's body lacks `"columns":[]`).
TEST_CASE("model table: a range over rows not held resolves through pages that name no columns", "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, selectTable(), 100'000);  // rows 0 to 399 held
    std::size_t const sent = h.dispatcher.calls.size();
    h.backend.selectTableRange(tableOf(h), 500, 1'499);
    h.settle();
    REQUIRE(h.dispatcher.calls.size() > sent);
    for (std::size_t call = sent; call < h.dispatcher.calls.size(); ++call) {
        CHECK(h.dispatcher.calls.at(call).action == "Table.samples");
        CHECK(h.dispatcher.calls.at(call).body.contains(R"("columns":[])"));
    }
    replyKeyPages(h);
    CHECK(elementValue(*screen, "list", "selectedCount") == "1000");
    CHECK(statusOf(h).counts == "1,000 of 100,000 selected");
}

// Mutation: no bound (keysOf is called and the calls go out).
TEST_CASE("model table: a range over maxRangeRows is refused and selects nothing", "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, selectTable(), 100'000);
    std::size_t const sent = h.dispatcher.calls.size();
    h.backend.selectTableRange(tableOf(h), 0, 10'000);  // 10,001 rows
    h.settle();
    CHECK(h.dispatcher.calls.size() == sent);
    CHECK(statusOf(h).message == "A range selects at most 10,000 rows");
    CHECK(elementValue(*screen, "list", "selectedCount") == "0");
    h.backend.selectTableRange(tableOf(h), 0, 9'999);  // 10,000 rows: allowed
    h.settle();
    CHECK(h.dispatcher.calls.size() > sent);
}

// Mutations: the view index taken from the reply's position (0) instead of `indices`; the highlight written into the
// selection; no stop of the earlier key's fetch.
TEST_CASE("model table: highlightKey scrolls to the row and marks it", "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, kHighlight, 9'093);
    int const table = tableOf(h);
    click(h, "to5012");
    REQUIRE(lastQuery(h).keys.has_value());
    CHECK(*lastQuery(h).keys == std::vector<table::RowId>{std::int64_t{5012}});
    CHECK(h.dispatcher.calls.back().body.contains(R"("columns":[])"));
    replyKeyed(h, {{5012, 5011}});
    CHECK(h.backend.prop(table, "scrolledToRow") == "5011");
    CHECK(h.backend.prop(table, "highlightedRow") == "5011");
    std::size_t const marked = h.dispatcher.calls.size();
    h.advance(80ms);
    CHECK(askedForPage(h, marked, 5'000));  // the page that holds view row 5,011
    CHECK(elementValue(*screen, "list", "selectedCount") == "0");  // never the selection
    click(h, "to77");
    replyKeyed(h, {});  // filtered out or gone: no row
    CHECK(h.backend.prop(table, "highlightedRow") == "none");
    CHECK(h.backend.prop(table, "scrolledToRow") == "5011");  // nothing scrolled
    std::size_t const first = h.dispatcher.calls.size();
    click(h, "to5012");
    click(h, "to77");  // two keys before a reply
    CHECK(stopped(h, first));  // the first key's fetch is stopped
    click(h, "toNone");
    CHECK(h.backend.prop(table, "highlightedRow") == "none");
}

namespace {

// A model-backed table whose `name` column's cell is a button `pick`, and a button that focuses `pick` in row 250.
std::string const kPick =
    R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},)"
    R"("view":{"kind":"column","children":[)"
    R"({"kind":"button","label":"250","testId":"to250","onClick":{"focus":"pick","row":250}},)"
    R"({"kind":"table","id":"list","testId":"list","model":"lab","table":"samples",)"
    R"("columns":[{"id":"id"},{"id":"name","cell":[{"kind":"button","id":"pick","label":{"row":"name"}}]}],)"
    R"("empty":[{"kind":"text","text":"No samples"}]}]}})";

// The key of the table row whose cell @p widget is.
std::string rowKeyHolding(TableHarness const& h, int widget) {
    for (int const row : h.backend.children(tableOf(h))) {
        if (std::ranges::contains(h.backend.children(row), widget)) {
            return h.backend.prop(row, "rowKey");
        }
    }
    return {};
}

}  // namespace

namespace {

// Answers the open `Table.samples` call for the page at offset 0 with @p keys, then settles.
void replyFirstPage(TableHarness& h, std::vector<std::int64_t> const& keys) {
    for (std::size_t index = 0; index < h.dispatcher.calls.size(); ++index) {
        auto const& call = h.dispatcher.calls[index];
        if (call.action == "Table.samples" && !call.answered && !call.options.stop.stop_requested() &&
            !queryOf(h, index).keys && queryOf(h, index).page.offset == 0) {
            h.reply(index, morph::ui::testing::pageJsonOfKeys(0, keys, 9'093, h.columns));
            return;
        }
    }
    FAIL("no open call for the first page");
}

// The keys 0 to 199 with @p a and @p b swapped: the first page in another order.
std::vector<std::int64_t> swappedPage(std::int64_t a, std::int64_t b) {
    std::vector<std::int64_t> keys(200);
    std::iota(keys.begin(), keys.end(), std::int64_t{0});
    std::swap(keys.at(static_cast<std::size_t>(a)), keys.at(static_cast<std::size_t>(b)));
    return keys;
}

}  // namespace

// Mutation: focus not looked for by key after a reset (it stays on view row 7, which another row now holds).
TEST_CASE("model table: after a new sort, focus is found again by key once its page arrives", "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, kModelTable, 9'093);
    int const table = tableOf(h);
    h.backend.scrollTable(table, 0, 30);
    h.settle();
    answerPages(h, 9'093);
    h.backend.focusMove(table, morph::ui::FocusMove::First);
    for (int step = 0; step < 7; ++step) {
        h.backend.focusMove(table, morph::ui::FocusMove::Down);
    }
    h.settle();
    REQUIRE(h.backend.focusCell(table).first == 7);
    h.backend.clickHeader(table, 1);  // a new query: the window drops its pages, then announces a reset
    h.settle();
    replyFirstPage(h, swappedPage(7, 20));  // key 7 is at view row 20 now
    answerPages(h, 9'093);
    CHECK(h.backend.focusCell(table).first == 20);
}

// Mutation: an index past the row count before a change moved by the first page's `Inserted` (the mark goes to view
// row 9,123, past the end).
TEST_CASE("model table: a highlight located before the first page of a new sort keeps its row", "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, kHighlight, 9'093);
    int const table = tableOf(h);
    click(h, "to77");
    replyKeyed(h, {{77, 77}});
    REQUIRE(h.backend.prop(table, "highlightedRow") == "77");
    h.backend.clickHeader(table, 1);  // a new query: the lowering locates key 77 again
    h.settle();
    replyKeyed(h, {{77, 30}});  // the one-row reply comes first
    REQUIRE(h.backend.prop(table, "highlightedRow") == "30");
    replyFirstPage(h, swappedPage(30, 77));
    answerPages(h, 9'093);
    CHECK(h.backend.prop(table, "highlightedRow") == "30");
}

// Mutation: a cell's row scope keyed by the row it was built for (the focus lands on no cell, or on another row's).
TEST_CASE("model table: a reused cell's elements are focused by the row it shows", "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, kPick, 9'093);
    h.backend.scrollTable(tableOf(h), 0, 30);  // rows 0 to 39
    h.settle();
    answerPages(h, 9'093);
    h.backend.scrollTable(tableOf(h), 240, 30);  // row 250 takes the slot, and the cell, row 20 had
    h.settle();
    answerPages(h, 9'093);
    REQUIRE(statsOf(*screen).cellsRebound > 0);
    h.backend.click(h.byTestId("Button", "to250"));
    h.settle();
    REQUIRE(h.backend.focused() != 0);
    CHECK(rowKeyHolding(h, h.backend.focused()) == "250");
}

// Mutation: the mount leaving out the rows a `Changed` operation names (no row is bound again: 0, and the renamed
// text is not shown).  Gate K5.
TEST_CASE("model table: a refetch that changed 20 of 200 rows evaluates 20 rows again", "[ui][table]") {
    TableHarness h;  auto screen = mountShown(h);
    h.backend.scrollTable(tableOf(h), 0, 30);  h.settle();                    // rows 0 to 39 realized (overscan 10)
    MountStats const before = statsOf(*screen);
    change(h, std::vector<std::int64_t>{});                                   // a change without keys: the pages again
    // the same page 0, with rows 0 to 19 renamed and rows 20 to 199 as they were
    h.reply(oldestOpen(h, "Table.samples"), pageJsonRenaming(revisionOf(h), 0, 200, 9093, h.columns, 0, 20));
    CHECK(statsOf(*screen).rowBinds - before.rowBinds == 20);                 // K5
    CHECK(statsOf(*screen).cellsCreated == before.cellsCreated);
    CHECK(h.backend.find("Text", "text", "renamed-0").has_value());
}

// Review Focus 1. Mutation: the fitting of a new revision not reporting the dropped filter (the count is 0).
TEST_CASE("table rows: a new description revision drops what names a removed column", "[ui][table]") {
    auto store = std::make_shared<GatedTableStateStore>(
        storeExecutor(),
        R"({"version":1,"sort":[{"column":"c3","direction":"asc"}],"filters":{"columns":{"c3":{"include":[{"gt":"1"}]}}}})");
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);  store->release();  h.settle();   // revision A: id, name, c3
    REQUIRE(lastQuery(h).sort.at(0).column == "c3");
    table::TableDescription const b = samplesDescription(2);                 // revision B: id, name
    h.reply(oldestOpen(h, "Table.samples"),
            morph::ui::testing::pageJsonAt(b.revision, 0, 200, 9093, h.columns));   // a reply names another revision
    CHECK(h.dispatcher.count("Table.samples.describe") == 2);                 // exactly one more describe
    replyDescribe(h, samplesDescription(2));
    table::TableQuery const q = lastQuery(h);
    CHECK(std::ranges::none_of(q.sort, [](auto const& key) { return key.column == "c3"; }));
    CHECK_FALSE(q.filters.columns.contains("c3"));
    REQUIRE(q.columns.has_value());
    CHECK(std::ranges::find(*q.columns, "c3") == q.columns->end());
    CHECK(diagnosticsMentioning(h, "c3") == 1);                               // the filter; a sort key is not reported
    CHECK(headerLabels(h) == std::vector<std::string>{"Id", "Name"});         // mounted again from revision B
    CHECK(h.dispatcher.count("Table.samples.describe") == 2);                 // once per revision
}

// Review Focus 2. Mutation: the load completion run with no liveness check (a query is sent, or the test crashes
// under the sanitizer build).
TEST_CASE("model table: unmounted while its state load is held", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kStored});
    TableHarness h{store};
    auto screen = h.mount(kModelTable);  replyDescribe(h);
    h.backend.clickHeader(tableOf(h), 0);  h.settle();   // nothing loaded yet: the change is held, not saved
    CHECK(store->saves() == 0);
    screen.reset();  h.settle();
    store->release();  h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 0);     // nothing applied, no query
    REQUIRE(store->saves() == 1);                        // the held change, over the loaded document
    CHECK(store->lastSaved().contains(R"("contains":"a")"));   // the stored filter kept
    CHECK(table::readState(store->lastSaved())->sort->at(0).column == "id");
}

// Review Focus 4. Mutation: `empty` shown while `total().known` is false, or while rows are undecided.
TEST_CASE("model table: empty waits for the first reply and for undecided rows", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(kModelTable);
    CHECK_FALSE(shownText(h, "No samples"));                                  // before the description
    replyDescribe(h);
    CHECK_FALSE(shownText(h, "No samples"));                                  // before any page
    CHECK(statusOf(h).counts.empty());  CHECK(statusOf(h).shown);
    replyPage(h, morph::ui::testing::pageJsonAt(revisionOf(h), 0, 0, 0, h.columns, table::TotalKind::AtLeast, 40));
    CHECK_FALSE(shownText(h, "No samples"));
    CHECK(statusOf(h).counts == "0 so far, 40 loading");
    change(h, std::vector<std::int64_t>{});                                   // the level arrived: the page again,
    h.advance(80ms);                                                          // which holds no row, after tableSettle
    replyPage(h, morph::ui::testing::pageJsonAt(revisionOf(h), 0, 0, 0, h.columns));
    CHECK(shownText(h, "No samples"));
    CHECK(statusOf(h).counts == "0 rows");
}

// Mutations: a status line built without `failed` (no state, no retry); Retry sending the query without `retry`.
TEST_CASE("model table: a failed level offers Retry, which sends `retry`", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(kModelTable);
    replyDescribe(h, withLevel(samplesDescription(3), "c3", "recipe"));
    replyPage(h, pageJsonUnloaded(revisionOf(h), 0, 200, 9093, h.columns,
                                  {{.index = 1, .levels = {"recipe"}, .failed = {"recipe"}}}, /*failed*/ {"recipe"}));
    CHECK(statusOf(h).state == "Recipe data could not be loaded.");
    REQUIRE(statusOf(h).retry);
    std::size_t const sent = h.dispatcher.count("Table.samples");
    h.backend.retryTable(tableOf(h));  h.settle();
    REQUIRE(h.dispatcher.count("Table.samples") > sent);
    CHECK(lastQuery(h).retry);
}

// Mutation: `WindowRows` not telling of a failed page fetch (no view change follows a failure, so the status line
// keeps its empty state).
TEST_CASE("model table: a failed page fetch is said in the status line", "[ui][table]") {
    TableHarness h;  auto screen = mountShown(h);
    CHECK(statusOf(h).state.empty());
    change(h, std::vector<std::int64_t>{});                                   // the pages again
    h.fail(oldestOpen(h, "Table.samples"));
    CHECK(statusOf(h).state == "The rows could not be loaded: it failed");
    CHECK(statusOf(h).retry);
}

// Mutation: a page refetch per event (the call names no `keys`, or twenty calls go out).  Gate E3 on a screen.
TEST_CASE("model table: an arrival is one keyed request", "[ui][table]") {
    TableHarness h;  auto screen = mountShown(h);
    std::size_t const sent = h.dispatcher.count("Table.samples");
    std::vector<std::int64_t> keys(20);  std::ranges::iota(keys, std::int64_t{0});
    change(h, keys);
    REQUIRE(h.dispatcher.count("Table.samples") == sent + 1);                 // exactly one more call
    REQUIRE(lastQuery(h).keys.has_value());
    CHECK(lastQuery(h).keys->size() == 20);                                   // the twenty keys; the run's probe is key 0
}

// Mutation: the mount ignoring unloadedLevels (every cell Loaded).  Cross-part decision 29.
TEST_CASE("model table: a cell of an unloaded level is a placeholder, of a failed one an error mark", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(kModelTable);
    replyDescribe(h, withLevel(samplesDescription(3), "c3", "recipe"));       // column 2 has the level
    replyPage(h, pageJsonUnloaded(revisionOf(h), 0, 200, 9093, h.columns,
                                  {{.index = 0, .levels = {"recipe"}, .failed = {}},
                                   {.index = 1, .levels = {"recipe"}, .failed = {"recipe"}}},
                                  /*failed*/ {"recipe"}));
    int const table = tableOf(h);
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{0}}, 2) == CellLoad::Unloaded);
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{0}}, 1) == CellLoad::Loaded);      // name has no level
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{1}}, 2) == CellLoad::Failed);
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{2}}, 2) == CellLoad::Loaded);
    auto const created = statsOf(*screen).cellsCreated;
    change(h, std::vector<std::int64_t>{0});                                  // the level arrives for row 0
    REQUIRE(lastQuery(h).keys.has_value());
    h.reply(h.dispatcher.last("Table.samples"), keyedJsonAt(revisionOf(h), {{0, 0}}, 9093, h.columns));
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{0}}, 2) == CellLoad::Loaded);
    CHECK(statsOf(*screen).cellsCreated == created);                          // the same cell, bound again
}

// Mutations: the miss fetched inside the binding (a signal write is refused there: the debug build aborts);
// `WindowRows::visible` keeping an id the description lacks (the query names "nosuch").
TEST_CASE("model table: a row field the window lacks is fetched once, after the binding", "[ui][table]") {
    // `c7` is described and not laid out, and read out of the whole row with `get`, which no column names; so is
    // `nosuch`, which no column of the description has. Each is its own text, so each read is made.
    std::string const readsAcross = withTableKey(
        kModelTable, R"("columns":[{"id":"id"},{"id":"name","cell":[)"
                     R"({"kind":"text","text":{"get":{"row":""},"path":"c7"}},)"
                     R"({"kind":"text","text":{"get":{"row":""},"path":"nosuch"}}]}])");
    TableHarness h;  auto screen = h.mount(readsAcross);  replyDescribe(h, samplesDescription(8));
    std::vector<std::string> const laidOut{"id", "name"};
    h.dispatcher.reply(oldestOpen(h, "Table.samples"),
                       morph::ui::testing::pageJsonAt(revisionOf(h), 0, 200, 9093, laidOut));   // no settle yet
    std::size_t const replied = h.dispatcher.calls.size();
    h.owner.runOne();                                                          // the reply is accepted, cells bind
    h.settle();
    CHECK(h.diagnostics.empty());                                              // no write refused inside a binding
    std::vector<table::TableQuery> naming;                                     // the calls since that name `c7`
    for (std::size_t index = replied; index < h.dispatcher.calls.size(); ++index) {
        table::TableQuery const query = queryOf(h, index);
        if (query.columns && std::ranges::contains(*query.columns, "c7")) {
            naming.push_back(query);
        }
    }
    REQUIRE(naming.size() == 1);                                               // one fetch for every row's miss
    CHECK(naming.front().page.offset == 0);                                    // the page in view
    CHECK_FALSE(std::ranges::contains(*naming.front().columns, "nosuch"));
}

// Mutations: no hold timer (the message is still there after six seconds); `status: false` still filling
// `message`; an empty text notifying.
TEST_CASE("model table: a message lasts six seconds or until the next gesture", "[ui][table]") {
    SECTION("in the status line") {
        TableHarness h;  auto screen = mountShown(h);
        morph::ui::detail::ScreenCore& core = coreOf(*screen);
        morph::ui::detail::ElementState& list = elementOf(*screen, "list");
        core.tableMessage(list, "Refused");  h.settle();  CHECK(statusOf(h).message == "Refused");
        h.advance(5'999ms);  CHECK(statusOf(h).message == "Refused");
        h.advance(1ms);      CHECK(statusOf(h).message.empty());
        core.tableMessage(list, "Again");  h.settle();
        h.backend.clickHeader(tableOf(h), 1);  h.settle();  CHECK(statusOf(h).message.empty());   // the next gesture
        core.tableMessage(list, "");  h.settle();  CHECK(h.host.notified.empty());                // notifies nothing
    }
    SECTION("status: false") {
        TableHarness h;  auto screen = mountShown(h, kNoStatus);
        coreOf(*screen).tableMessage(elementOf(*screen, "list"), "Refused");  h.settle();
        CHECK(h.host.notified == std::vector<std::pair<std::string, std::string>>{{"Refused", "warning"}});
        CHECK(statusOf(h).message.empty());  CHECK_FALSE(statusOf(h).shown);
    }
}

// Mutation: the mount leaving a realized row's cells unset on a change that names the row (no cell is bound again:
// `cellBinds` grows by 0).  Gate F6, on the recording backend.
TEST_CASE("model table: an arrival for 50 rows re-evaluates their cells and no other", "[ui][table]") {
    TableHarness h;  auto screen = mountShown(h);
    h.backend.scrollTable(tableOf(h), 10, 30);  h.settle();                   // rows 0 to 49 realized
    std::vector<std::int64_t> keys(50);  std::ranges::iota(keys, std::int64_t{0});
    auto const arrive = [&h](std::vector<std::int64_t> const& arrived) {
        change(h, arrived);
        table::TableQuery const asked = queryOf(h, h.dispatcher.last("Table.samples"));
        REQUIRE(asked.keys.has_value());
        std::vector<std::pair<std::int64_t, std::int64_t>> rows;
        for (table::RowId const& key : *asked.keys) {
            rows.emplace_back(std::get<std::int64_t>(key), std::get<std::int64_t>(key));
        }
        h.reply(h.dispatcher.last("Table.samples"), keyedJsonAt(revisionOf(h), rows, 9093, h.columns));
    };
    MountStats const before = statsOf(*screen);
    arrive(keys);
    CHECK(statsOf(*screen).cellBinds - before.cellBinds == 50 * 3);          // every cell of the 50 rows
    CHECK(statsOf(*screen).rowBinds - before.rowBinds == 50);
    CHECK(statsOf(*screen).cellsCreated == before.cellsCreated);             // nothing rebuilt
    MountStats const again = statsOf(*screen);
    arrive({keys.begin(), keys.begin() + 10});                                // the same values for ten rows
    CHECK(statsOf(*screen).cellBinds == again.cellBinds);                     // equal records: no bind
    CHECK(statsOf(*screen).rowBinds == again.rowBinds);
}

namespace {

// Whether a document loads against @p catalog.
bool loads(std::string_view text, morph::ui::Catalog const& catalog) {
    return morph::ui::loadDocumentText(text, morph::ui::LoadOptions{.catalog = catalog}).has_value();
}

// The reason a document is refused against the table catalog; empty when it loads.
std::string refusalOf(std::string_view text) {
    auto const loaded =
        morph::ui::loadDocumentText(text, morph::ui::LoadOptions{.catalog = morph::ui::testing::tableCatalog()});
    return loaded ? std::string{} : loaded.error().reason;
}

}  // namespace

// Mutations: `table` left out of kTableKeys (the first REQUIRE: "names its `action`"); no catalog check of
// `Table.<id>` (the "nosuch" CHECK loads).
TEST_CASE("ui document: a model-backed table names a table of its model", "[ui][document]") {
    using morph::ui::testing::kModelTable;
    using morph::ui::testing::kNoPersist;
    using morph::ui::testing::kNoStatus;
    using morph::ui::testing::tableCatalog;
    using morph::ui::testing::withTableKey;
    REQUIRE(refusalOf(kModelTable).empty());
    CHECK(loads(kNoPersist, tableCatalog()));  CHECK(loads(kNoStatus, tableCatalog()));
    CHECK(loads(withTableKey(kModelTable, R"("persist":"wide")"), tableCatalog()));
    CHECK(refusalOf(withTableKey(kModelTable, R"("table":"nosuch")")) == "the table `nosuch` is not in the catalog");
    CHECK(refusalOf(withTableKey(kModelTable, R"("action":"Lims_ListSamples")")) ==
          "a table names a `table` or an `action`, not both");
    CHECK(refusalOf(withTableKey(kModelTable, R"("model":"Lims_SampleModel")")) ==
          "`Lims_SampleModel` is not a model alias of this scope");          // a type is not an alias
    CHECK(refusalOf(withTableKey(kModelTable, R"("persist":{"ref":"x"})")) ==
          "`persist` is a literal: true, false or a name");
    CHECK(loads(withTableKey(kModelTable, R"("filters":{"columns":{"name":{"include":[{"contains":"a"}]}}})"),
                tableCatalog()));
    CHECK(refusalOf(withTableKey(kModelTable, R"("filters":{"columns":7})")).starts_with("`filters` is not a filter"));
    CHECK(refusalOf(withTableKey(kModelTable, R"("pageSize":20)")) ==
          "a model-backed table's engine pages; `pageSize` and `page` are for a table over `rows`");
}

// Mutations: the loader taking `table` without `model` (the document loads); a `saved` point's `of` left unchecked
// (the second document loads).
TEST_CASE("ui document: a table's `table` needs its `model`", "[ui][document]") {
    std::string const text =
        R"({"vocab":"ui/1","id":"samples","view":{"kind":"table","id":"list","table":"samples"}})";
    CHECK(refusalOf(text) == "`table` names one of a model's tables; give the table its `model`");
    CHECK(refusalOf(R"({"vocab":"ui/1","id":"s","customize":{"v":{"kind":"saved","of":"nosuch"}},)"
                    R"("view":{"kind":"text","text":"x"}})") == "a `saved` point names a table; `nosuch` is not one");
}
