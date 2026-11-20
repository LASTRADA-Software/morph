// SPDX-License-Identifier: Apache-2.0

// The client table model: `EngineRows` over the engine, `WindowRows` over a server's pages, the status line's counts,
// `ValueRowsSource`, a document's rows as a source, and the model-backed `table` node on a screen
// (docs/spec/table/table.md, "The client table model", "The window under changes", "The status line", "A document's
// rows as a source", "The document `table` node on the engine", "Persisted state").
//
// Each case names the mutation it was seen to fail on.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <memory>
#include <morph/ui/mount.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_change.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

namespace {

// Mounts the model table with thirty rows in view, focuses view row 7 (key 7), and starts a new sort: the window drops
// its pages, and the rows are asked where key 7 is.
int focusSevenThenSort(TableHarness& h) {
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
    h.backend.clickHeader(table, 1);
    h.settle();
    return table;
}

// The keys 0 to 199 with key 9,093 at view row 7: the first page of an order that lacks key 7 there.
std::vector<std::int64_t> pageWithoutSeven() {
    std::vector<std::int64_t> keys(200);
    std::iota(keys.begin(), keys.end(), std::int64_t{0});
    keys.at(7) = 9'093;
    return keys;
}

}  // namespace

// Mutations: the located index ignored, the fallback taken instead (focus stays on view row 7, which another key now
// holds); the located row not scrolled to when the rows do not hold it (its page is never asked for).
TEST_CASE("model table: a new sort that moves the focused key off-screen keeps focus on it", "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, kModelTable, 9'093);
    int const table = focusSevenThenSort(h);
    // The first page of the new order lacks key 7, and every row in view is fetched: still no answer, no move.
    replyFirstPage(h, pageWithoutSeven());
    CHECK(h.backend.focusCell(table).first == 7);
    std::size_t const located = h.dispatcher.calls.size();
    replyKeyed(h, {{7, 5'000}});  // key 7 is at view row 5,000
    CHECK(h.backend.focusCell(table).first == 5'000);
    CHECK(h.backend.prop(table, "scrolledToRow") == "5000");  // not held: scrolled to, so the window fetches it
    h.advance(80ms);  // the window's settle after a scroll
    CHECK(askedForPage(h, located, 5'000));
    answerPages(h, 9'093);
    CHECK(h.backend.focusCell(table).first == 5'000);
}

// Mutation: the fallback's clamp applied while the count is not known (focus goes to view row 0).
TEST_CASE("model table: an answer that the focused key is gone, before the first page, keeps focus at its place",
          "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, kModelTable, 9'093);
    int const table = focusSevenThenSort(h);
    replyKeyed(h, {});  // key 7 is gone; no page of the new order has arrived, so the count is not known
    replyFirstPage(h, pageWithoutSeven());
    answerPages(h, 9'093);
    CHECK(h.backend.focusCell(table).first == 7);  // the row now at its place, key 9,093
}

// Mutation: the backend told of a located row before the count is known (focus is on view row 5,000 of a view with no
// rows yet).
TEST_CASE("model table: a located row answered before the first page is told once the count is known",
          "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, kModelTable, 9'093);
    int const table = focusSevenThenSort(h);
    replyKeyed(h, {{7, 5'000}});
    CHECK(h.backend.focusCell(table).first == 7);  // no rows yet: nothing told
    std::size_t const located = h.dispatcher.calls.size();
    replyFirstPage(h, pageWithoutSeven());
    CHECK(h.backend.focusCell(table).first == 5'000);
    CHECK(h.backend.prop(table, "scrolledToRow") == "5000");
    h.advance(80ms);  // the window's settle after a scroll
    CHECK(askedForPage(h, located, 5'000));
    answerPages(h, 9'093);
    CHECK(h.backend.focusCell(table).first == 5'000);
}

// Mutation: an answer that the view lacks the key leaving focus looking for it (the row that took its place is not
// followed through the next sort: focus stays on view row 7).
TEST_CASE("model table: a focused key a new sort deleted hands focus to the row at its place", "[ui][table]") {
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
    // A new query in which key 7 is gone and key 9,093 is at view row 7: once the rows answer that the view lacks
    // key 7, the row at its place takes focus.
    h.backend.clickHeader(table, 1);
    h.settle();
    std::vector<std::int64_t> keys(200);
    std::iota(keys.begin(), keys.end(), std::int64_t{0});
    keys.at(7) = 9'093;
    replyFirstPage(h, keys);
    replyKeyed(h, {});
    answerPages(h, 9'093);
    REQUIRE(h.backend.focusCell(table).first == 7);
    // Another sort moves key 9,093 to view row 25: focus follows it there.
    h.backend.clickHeader(table, 1);
    h.settle();
    std::swap(keys.at(7), keys.at(25));
    replyFirstPage(h, keys);
    answerPages(h, 9'093);
    CHECK(h.backend.focusCell(table).first == 25);
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

using morph::ui::testing::loads;
using morph::ui::testing::refusalOf;

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

// ── How a column shows its cells ────────────────────────────────────────────

namespace {

using morph::ui::testing::TableHarness;

// A document with the model alias `sample` and @p view.
std::string doc(std::string_view view, std::string_view mutations = "{}") {
    return R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},"mutations":)" +
           std::string{mutations} + R"(,"view":)" + std::string{view} + "}";
}

// `id` (the key), `mass` (two places, half up, in kg, toned by `massTone` and described by `massTip`), `name`,
// `count`, and the companions `massTone`, `massTip` and `lockedBy`, the row's lock.
table::TableDescription companionDescription() {
    table::TableDescription d;
    d.id = "samples";
    d.label = "Samples";
    d.key = "id";
    d.columns = {
        table::TableColumn{.info = {.id = "id", .kind = table::ColumnKind::Integer}, .label = "Id"},
        table::TableColumn{.info = {.id = "mass", .kind = table::ColumnKind::Decimal},
                           .label = "Mass",
                           .display = {.decimals = 2, .rounding = table::Rounding::HalfUp, .unit = "kg"},
                           .tone = "massTone",
                           .tooltip = "massTip"},
        table::TableColumn{.info = {.id = "name", .kind = table::ColumnKind::Text}, .label = "Name"},
        table::TableColumn{.info = {.id = "count", .kind = table::ColumnKind::Integer}, .label = "Count"},
        table::TableColumn{.info = {.id = "massTone", .kind = table::ColumnKind::Text}, .shown = false},
        table::TableColumn{.info = {.id = "massTip", .kind = table::ColumnKind::Text}, .shown = false},
        table::TableColumn{.info = {.id = "lockedBy", .kind = table::ColumnKind::Text}, .shown = false},
    };
    d.rowLock = "lockedBy";
    return table::describe(std::move(d));
}

constexpr std::string_view kTable = R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples"})";

using CompanionRow = std::map<std::string, table::Cell, std::less<>>;

// The row the companion cases show: mass 0.125, toned "warn", described "Above limit"; `count` holds text, which an
// Integer column cannot read.
CompanionRow companionRow(std::int64_t key) {
    return CompanionRow{{"id", table::Cell{key}},
                        {"mass", table::Cell{table::parseDecimal("0.125").value()}},
                        {"name", table::Cell{std::string{"A"}}},
                        {"count", table::Cell{std::string{"abc"}}},
                        {"massTone", table::Cell{std::string{"warn"}}},
                        {"massTip", table::Cell{std::string{"Above limit"}}},
                        {"lockedBy", table::Cell{std::string{}}}};
}

// The page answering the latest query with @p rows, each holding the columns the query names.
std::string companionPage(TableHarness const& h, std::vector<CompanionRow> const& rows = {companionRow(1)}) {
    table::GenericPage page;
    page.revision = companionDescription().revision;
    page.columns = morph::ui::testing::lastQuery(h).columns.value_or(std::vector<std::string>{});
    page.total = static_cast<std::int64_t>(rows.size());
    page.totalKind = table::TotalKind::Exact;
    page.offset = 0;
    for (CompanionRow const& row : rows) {
        page.keys.emplace_back(std::get<std::int64_t>(row.at("id")));
        table::GenericRow cells;
        for (std::string const& column : page.columns) {
            auto const found = row.find(column);
            cells.push_back(found == row.end() ? table::Cell{} : found->second);
        }
        page.rows.push_back(std::move(cells));
    }
    return glz::write_json(page).value_or("");
}

}  // namespace

// Mutation: the default cell's text through `textOf`, which shows "0.125".
TEST_CASE("table display: a model-backed table draws its description's display", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto screen = h.mount(doc(kTable));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, companionPage(h));
    CHECK(cellText(h, 1, "mass") == "0.13");
    CHECK(cellAccent(h, 1, "mass") ==
          morph::ui::CellAccent{.tone = morph::ui::Tone::Warn, .tooltip = "Above limit", .invalid = false});
    CHECK(cellAccent(h, 1, "name") == morph::ui::CellAccent{});
    CHECK(columnOf(h, "mass").unit == "kg");
}

// Mutation: the accent built from the companions only (cellLook not read), which leaves `invalid` false.
TEST_CASE("table display: an invalid cell draws its mark and no text", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto screen = h.mount(doc(kTable));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, companionPage(h));
    CHECK(cellText(h, 1, "count").empty());
    CHECK(cellAccent(h, 1, "count") ==
          morph::ui::CellAccent{.tone = std::nullopt, .tooltip = "The value could not be read", .invalid = true});
    CHECK_FALSE(cellAccent(h, 1, "mass").invalid);
}

// Mutation: companions fetched for every described column, which puts massTone in the first query.
TEST_CASE("table display: companions are fetched with their column only", "[ui][table][display]") {
    using namespace morph::ui::testing;
    {
        TableHarness h;
        auto screen =
            h.mount(doc(R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples","columns":[{"id":"name"}]})"));
        replyDescribe(h, describeJson(companionDescription()));
        CHECK(lastQuery(h).columns == std::vector<std::string>{"id", "name", "lockedBy"});
    }
    {
        TableHarness h;
        auto screen = h.mount(doc(R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
                                      "columns":[{"id":"mass","tone":{"lit":"ok"}}]})"));
        replyDescribe(h, describeJson(companionDescription()));
        CHECK(lastQuery(h).columns == std::vector<std::string>{"id", "mass", "massTip", "lockedBy"});
        replyPage(h, companionPage(h));
        CHECK(cellAccent(h, 1, "mass").tone == morph::ui::Tone::Ok);  // the document's, not the companion's "warn"
    }
}

// Mutation: companion columns left in the layout (only `shown` read), which gives the table the columns massTone,
// massTip and lockedBy.
TEST_CASE("table display: companion columns are not arranged", "[ui][table][display]") {
    using namespace morph::ui::testing;
    {
        // Companions are never shown, even when the description marks them shown.
        table::TableDescription shownCompanions = companionDescription();
        for (table::TableColumn& column : shownCompanions.columns) {
            column.shown = true;
        }
        TableHarness h;
        auto screen = h.mount(doc(kTable));
        replyDescribe(h, describeJson(table::describe(shownCompanions)));
        std::vector<std::string> ids;
        for (morph::ui::TableColumn const& column : h.backend.tableColumns(h.byTestId("Table", "t"))) {
            ids.push_back(column.id);
        }
        CHECK(ids == std::vector<std::string>{"id", "mass", "name", "count"});
    }
    {
        // Mutation: the layout built from every column of the description (hiddenCount 3, and the menus offer to
        // show the companions).
        TableHarness h;
        auto screen = h.mount(doc(kTable));
        replyDescribe(h, describeJson(companionDescription()));
        CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["id","mass","name","count"])");
        CHECK(elementValue(*screen, "t", "hiddenCount") == "0");
        int const table = h.byTestId("Table", "t");
        std::vector<std::vector<morph::ui::MenuEntry>> menus{menuOf(h, table, std::nullopt)};
        for (std::size_t column = 0; column < 4; ++column) {
            menus.push_back(menuOf(h, table, column));
        }
        for (auto const& entries : menus) {
            for (std::string const& label : labelsOf(entries)) {
                for (std::string_view const companion : {"massTone", "massTip", "lockedBy"}) {
                    CHECK(label.find(companion) == std::string::npos);
                }
            }
        }
        // Mutation: the chooser listing every column of the description.
        chooseMenu(h, table, std::nullopt, {"Columns…"});
        for (std::string_view const column : {"id", "mass", "name", "count"}) {
            CHECK(h.backend.find("Checkbox", "testId", "table_columns_show_" + std::string{column}).has_value());
        }
        for (std::string_view const companion : {"massTone", "massTip", "lockedBy"}) {
            CHECK_FALSE(h.backend.find("Checkbox", "testId", "table_columns_show_" + std::string{companion}).has_value());
        }
    }
    TableHarness h;  // a companion the document names is reported and not shown
    auto screen = h.mount(doc(R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
                                  "columns":[{"id":"name"},{"id":"massTip"}]})"));
    replyDescribe(h, describeJson(companionDescription()));
    CHECK(headerLabels(h, "t") == std::vector<std::string>{"Name"});
    CHECK(diagnosticsMentioning(h, "`massTip` is a companion column of `samples`") == 1);
}

// Mutation: document parts read only on a `rows` table, which keeps "0.13".
TEST_CASE("table display: a document's parts override the description", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto screen = h.mount(doc(R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
                                  "columns":[{"id":"mass","decimals":1,"rounding":"halfEven","empty":"n/a"}]})"));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, companionPage(h));
    CHECK(cellText(h, 1, "mass") == "0.1");
    CHECK(columnOf(h, "mass").unit == "kg");  // a part that gives no `unit` keeps the description's
}

// Mutation: reporting per row, which reports twice ("purple" and "pink").
TEST_CASE("table display: an unknown tone is reported once for its column", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto screen = h.mount(doc(kTable));
    replyDescribe(h, describeJson(companionDescription()));
    CompanionRow first = companionRow(1);
    CompanionRow second = companionRow(2);
    first.insert_or_assign("massTone", table::Cell{std::string{"purple"}});
    second.insert_or_assign("massTone", table::Cell{std::string{"pink"}});
    replyPage(h, companionPage(h, {first, second}));
    CHECK_FALSE(cellAccent(h, 1, "mass").tone.has_value());
    CHECK_FALSE(cellAccent(h, 2, "mass").tone.has_value());
    // One report for the column, though the two rows name two unknown tones.
    std::vector<std::string> const reports = reportsOf(h);
    REQUIRE(reports.size() == 1);
    CHECK(reports.front().starts_with("column `mass`'s tone is \"p"));
    CHECK(reports.front().ends_with("\"; a tone is neutral, info, ok, warn or err"));
}

// Mutation: a sort over the formatted text, which orders "10.00" before "9.00".
TEST_CASE("table display: text never orders", "[ui][table][display]") {
    using namespace morph::ui::testing;
    auto const texts = [](TableHarness const& h, std::size_t cell) {
        std::vector<std::string> out;
        int const table = h.backend.all("Table").front();
        for (int const row : h.backend.children(table)) {
            out.push_back(h.backend.prop(h.backend.children(row).at(cell), "text"));
        }
        return out;
    };
    {
        TableHarness h;
        auto screen = h.mount(R"({"vocab":"ui/1","id":"plain",
            "state":{"items":{"init":{"lit":[{"id":1,"v":10},{"id":2,"v":9}]}},
                     "order":{"init":{"lit":[{"column":"v","direction":"asc"}]}}},
            "view":{"kind":"table","id":"t","rows":{"ref":"items"},"sort":{"ref":"order"},
                    "columns":[{"id":"id","label":"Id"},{"id":"v","label":"V","decimals":2,"sortable":true}]}})");
        CHECK(texts(h, 1) == std::vector<std::string>{"9.00", "10.00"});
    }
    // Labels change the text and not the order: 1 shows "zeta" and 3 "alpha", and 1 still comes first.
    TableHarness h;
    auto screen = h.mount(R"({"vocab":"ui/1","id":"plain",
        "state":{"items":{"init":{"lit":[{"id":1,"code":3},{"id":2,"code":1}]}},
                 "order":{"init":{"lit":[{"column":"code","direction":"asc"}]}}},
        "view":{"kind":"table","id":"t","rows":{"ref":"items"},"sort":{"ref":"order"},
                "columns":[{"id":"id","label":"Id"},{"id":"code","label":"Code","sortable":true,
                            "labels":{"lit":[{"operand":"3","text":"alpha"},{"operand":"1","text":"zeta"}]}}]}})");
    CHECK(texts(h, 1) == std::vector<std::string>{"zeta", "alpha"});
}

namespace {

// `id` (the key) and `v`, a Number column.
table::TableDescription numberDescription() {
    table::TableDescription d;
    d.id = "samples";
    d.label = "Samples";
    d.key = "id";
    d.columns = {
        table::TableColumn{.info = {.id = "id", .kind = table::ColumnKind::Integer}, .label = "Id"},
        table::TableColumn{.info = {.id = "v", .kind = table::ColumnKind::Number}, .label = "V"},
    };
    return table::describe(std::move(d));
}

// The page answering the latest query: row `key` holds `v` for each of @p values, keys from 1.
std::string numberPage(TableHarness const& h, std::vector<double> const& values) {
    table::GenericPage page;
    page.revision = numberDescription().revision;
    page.columns = morph::ui::testing::lastQuery(h).columns.value_or(std::vector<std::string>{});
    page.total = static_cast<std::int64_t>(values.size());
    page.totalKind = table::TotalKind::Exact;
    page.offset = 0;
    std::int64_t key = 1;
    for (double const value : values) {
        page.keys.emplace_back(key);
        table::GenericRow cells;
        for (std::string const& column : page.columns) {
            cells.push_back(column == "id" ? table::Cell{key} : table::Cell{value});
        }
        page.rows.push_back(std::move(cells));
        ++key;
    }
    return glz::write_json(page).value_or("");
}

}  // namespace

// Mutation: a Number column reading only exact decimal text (no `double` fallback), which draws each of these as
// invalid and empty.
TEST_CASE("table display: a Number beyond an exact decimal still shows", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto screen = h.mount(doc(kTable));
    replyDescribe(h, describeJson(numberDescription()));
    replyPage(h, numberPage(h, {1e21, 1e-20, 9.3e18}));
    CHECK(cellText(h, 1, "v") == "1000000000000000000000");
    CHECK(cellText(h, 2, "v") == "0.00000000000000000001");
    CHECK(cellText(h, 3, "v") == "9300000000000000000");
    for (std::int64_t const key : {1, 2, 3}) {
        CHECK_FALSE(cellAccent(h, key, "v").invalid);
    }
}

// Mutation: the decimals of a Number beyond an exact decimal ignored, which shows "0.00000000000000000001".
TEST_CASE("table display: a Number beyond an exact decimal follows the column's places", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto screen = h.mount(doc(R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
                                  "columns":[{"id":"id"},{"id":"v","decimals":2,"grouping":true}]})"));
    replyDescribe(h, describeJson(numberDescription()));
    replyPage(h, numberPage(h, {1e21, 1e-20}));
    CHECK(cellText(h, 1, "v") == "1,000,000,000,000,000,000,000.00");
    CHECK(cellText(h, 2, "v") == "0.00");
}

// Mutation: a rows table's column built without its `unit`, which gives the header none.
TEST_CASE("table display: a rows table's header shows its column's unit", "[ui][table][display]") {
    TableHarness h;
    auto screen = h.mount(R"({"vocab":"ui/1","id":"plain",
        "state":{"items":{"init":{"lit":[{"id":1,"m":3}]}}},
        "view":{"kind":"table","id":"t","testId":"t","rows":{"ref":"items"},
                "columns":[{"id":"id","label":"Id"},{"id":"m","label":"Mass","unit":"kg","decimals":2}]}})");
    int const table = h.backend.all("Table").front();
    std::vector<morph::ui::TableColumn> const columns = h.backend.tableColumns(table);
    REQUIRE(columns.size() == 2);
    CHECK(columns.at(0).unit.empty());
    CHECK(columns.at(1).unit == "kg");
    CHECK(h.backend.prop(table, "columns").contains(";unit=kg"));
}

// Mutation: the column part's display checks left out of checkColumnPart, which loads each of these.
TEST_CASE("ui document: a column's display parts are checked when the document loads", "[ui][document]") {
    auto const column = [](std::string_view part) {
        return R"({"vocab":"ui/1","id":"plain","state":{"items":{"init":{"lit":[]}}},)"
               R"("view":{"kind":"table","id":"t","rows":{"ref":"items"},"columns":[{"id":"v",)" +
               std::string{part} + "}]}}";
    };
    REQUIRE(refusalOf(column(R"("decimals":2,"rounding":"halfUp","date":"dd-MMM-yyyy",)"
                             R"("labels":{"lit":[{"operand":"1","text":"one"}]})"))
                .empty());
    CHECK(refusalOf(column(R"("decimals":19)")) == "`decimals` is a literal integer, 0 to 18");
    CHECK(refusalOf(column(R"("rounding":"up")")) ==
          "`up` is not one of the values of `rounding`: halfEven or halfUp");
    CHECK(refusalOf(column(R"("labels":{"lit":[{"operand":1,"text":"one"}]})")) ==
          R"(`labels` is a literal list of `{"operand": text, "text": text}`)");
    CHECK_FALSE(refusalOf(column(R"("date":"qq")")).empty());
}

// Mutation: the unit appended whatever the text, which shows "three kg" for the labelled value.
TEST_CASE("table display: a rows table's quantity keeps its unit unless a label names it", "[ui][table][display]") {
    TableHarness h;
    auto screen = h.mount(R"({"vocab":"ui/1","id":"plain",
        "state":{"items":{"init":{"lit":[{"id":1,"m":{"num":3,"den":1,"dp":0,"unit":"kg"}},
                                         {"id":2,"m":{"num":5,"den":2,"dp":1,"unit":"kg"}}]}}},
        "view":{"kind":"table","id":"t","rows":{"ref":"items"},
                "columns":[{"id":"id","label":"Id"},{"id":"m","label":"Mass","decimals":2,
                            "labels":{"lit":[{"operand":"3","text":"three"}]}}]}})");
    int const table = h.backend.all("Table").front();
    std::vector<std::string> texts;
    for (int const row : h.backend.children(table)) {
        texts.push_back(h.backend.prop(h.backend.children(row).at(1), "text"));
    }
    CHECK(texts == std::vector<std::string>{"three", "2.50 kg"});
}

// ── A document's rows as a source ────────────────────────────────────────────

namespace reactive = morph::reactive;

namespace {

using morph::ui::Decimal;
using morph::ui::Key;
using morph::ui::kGroupRunColumn;
using morph::ui::Quantity;
using morph::ui::ValueColumn;
using morph::ui::ValueList;
using morph::ui::ValueRecord;
using morph::ui::ValueRowsCollator;
using morph::ui::ValueRowsSource;

Value rec(ValueRecord fields) { return Value{std::move(fields)}; }

Value ts(std::int64_t ms) {
    return Value{morph::time::DateTime{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{ms}}}};
}

Value dec(std::string_view text) {
    auto const parsed = table::parseDecimal(text);
    REQUIRE(parsed.has_value());
    return Value{Decimal{*parsed}};
}

Value qty(std::string_view amount, std::string unit) {
    auto const parsed = table::parseDecimal(amount);
    REQUIRE(parsed.has_value());
    return Value{Quantity{.value = *parsed, .unit = std::move(unit)}};
}

// kg and g of mass.
std::shared_ptr<table::UnitRegistry const> testUnits() {
    table::UnitRegistryBuilder builder;
    REQUIRE(builder.add(table::UnitInfo{.id = "kg", .symbol = "kg", .dimension = "mass",
                                        .toBase = morph::math::Rational{1, morph::math::DecimalPlaces{0}},
                                        .decimals = 3}));
    REQUIRE(builder.add(table::UnitInfo{.id = "g", .symbol = "g", .dimension = "mass",
                                        .toBase = *table::parseDecimal("0.001"), .decimals = 0}));
    return std::move(builder).build();
}

ValueColumn col(std::string id) { return ValueColumn{.id = std::move(id), .kind = std::nullopt, .sortBy = {}}; }

ValueColumn col(std::string id, table::ColumnKind kind) {
    return ValueColumn{.id = std::move(id), .kind = kind, .sortBy = {}};
}

std::shared_ptr<ValueRowsSource> sourceOf(RowsFixture& h, std::vector<ValueColumn> columns,
                                          std::shared_ptr<table::UnitRegistry const> units = {}) {
    return std::make_shared<ValueRowsSource>(
        h.runtime, std::move(columns), [](Value const& row) { return Key{*row.field("id")->asInt()}; },
        std::move(units));
}

// Every change a source notifies, kept.
struct RecordingListener final : table::ChangeListener {
    explicit RecordingListener(table::DataSource& source) : source{&source} { source.subscribe(*this); }
    RecordingListener(RecordingListener const&) = delete;
    RecordingListener& operator=(RecordingListener const&) = delete;
    RecordingListener(RecordingListener&&) = delete;
    RecordingListener& operator=(RecordingListener&&) = delete;
    ~RecordingListener() override { source->unsubscribe(*this); }
    void rowsChanged(table::RowChange const& change) override { changes.push_back(change); }
    void clear() { changes.clear(); }

    table::DataSource* source;
    std::vector<table::RowChange> changes;
};

// The list written to JSON and read back: no storage shared with @p list.
ValueList freshDecodeOf(ValueList const& list) {
    auto const json = morph::ui::parseJson(morph::ui::detail::canonicalJson(Value{list}));
    REQUIRE(json.has_value());
    auto const value = morph::ui::valueFromJson(*json);
    REQUIRE(value.has_value());
    REQUIRE(value->asList() != nullptr);
    return *value->asList();
}

// A copy of @p list with one row's field replaced; the other rows share storage with @p list's.
ValueList withField(ValueList list, std::size_t row, std::string const& name, Value value) {
    ValueRecord fields = *list.at(row).asRecord();
    fields.insert_or_assign(name, std::move(value));
    list.at(row) = Value{std::move(fields)};
    return list;
}

std::size_t columnIndex(table::DataSource const& source, std::string_view column) {
    auto const columns = source.columns();
    auto const found = std::ranges::find(columns, column, &table::ColumnInfo::id);
    REQUIRE(found != columns.end());
    return static_cast<std::size_t>(found - columns.begin());
}

table::Cell cellAt(table::DataSource const& source, std::size_t row, std::string_view column) {
    return source.snapshot()->cell(row, columnIndex(source, column));
}

table::ColumnKind kindOf(table::DataSource const& source, std::string_view column) {
    return source.columns()[columnIndex(source, column)].kind;
}

std::unique_ptr<table::Engine> engineOver(RowsFixture& h, std::shared_ptr<ValueRowsSource> const& source,
                                          std::shared_ptr<table::TextCollator const> collator = {}) {
    return std::make_unique<table::Engine>(
        source, table::EngineOptions{.owner = &h.owner, .services = {.collator = std::move(collator)}});
}

std::vector<std::int64_t> keysInView(table::Engine const& engine) {
    std::vector<std::int64_t> keys;
    for (std::size_t at = 0; at < engine.viewRowCount(); ++at) {
        keys.push_back(std::get<std::int64_t>(engine.rowIdAt(at)));
    }
    return keys;
}

// `n` rows `{id: i, name: "n<i>", notes: "x", amount: i}`.
ValueList sampleRows(std::size_t n) {
    ValueList rows;
    rows.reserve(n);
    for (std::size_t at = 0; at < n; ++at) {
        auto const i = static_cast<std::int64_t>(at);
        rows.push_back(rec({{"id", i}, {"name", "n" + std::to_string(at)}, {"notes", "x"}, {"amount", i}}));
    }
    return rows;
}

}  // namespace

// Mutation: a diff by row value (the fresh decode notifies, or `cellsDerived` counts every field).
TEST_CASE("value rows source: a document's rows as a source", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name"), col("notes"), col("amount")});
    RecordingListener listener{*src};
    ValueList const rows = sampleRows(100);
    src->use("name");  src->assign(rows);  h.settle();  listener.clear();
    std::size_t const derived = src->cellsDerived();
    src->assign(freshDecodeOf(rows));  h.settle();
    CHECK(listener.changes.empty());                                          // the same cells: nothing notified
    CHECK(src->cellsDerived() - derived == 100);                              // one column in use, not three fields
    src->assign(withField(freshDecodeOf(rows), 7, "name", Value{std::string{"zeta"}}));  h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Updated);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{7});
    CHECK(src->cellsHeld() == 100);                                           // "notes" and "amount" hold no cells
    src->use("amount");  h.settle();  CHECK(src->cellsHeld() == 200);
    src->unuse("amount");  CHECK(src->cellsHeld() == 100);                    // its last use ended: its cells go
}

// Mutations: `sortBy` read outside a tracked computation (no change is notified); a per-row change notified as Reset.
TEST_CASE("value rows source: a change to what one row's sortBy reads updates that row", "[ui][table]") {
    RowsFixture h;
    std::vector<std::unique_ptr<reactive::Signal<std::int64_t>>> rank;
    for (int i = 0; i < 10; ++i) { rank.push_back(std::make_unique<reactive::Signal<std::int64_t>>(h.runtime, i)); }
    reactive::Signal<std::int64_t> scale{h.runtime, 1};
    ValueColumn ranked = col("name");
    ranked.sortBy = [&](Value const& row) { return Value{rank.at(*row.field("id")->asInt())->get() * scale.get()}; };
    auto const src = sourceOf(h, {ranked});  RecordingListener listener{*src};
    src->use("name");  src->assign(sampleRows(10));  h.settle();  listener.clear();
    rank.at(3)->set(99);  h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Updated);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{3});
    CHECK(cellAt(*src, 3, "name") == table::Cell{std::int64_t{99}});
    listener.clear();  scale.set(2);  h.settle();                             // what every row's cell reads
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Reset);
}

// Mutation: keep the last duplicate (the view holds "c" and not "a").
TEST_CASE("value rows source: a duplicate key keeps its first row", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name")});
    std::size_t const reports = h.reported(morph::ui::detail::site::kDuplicateKey);
    src->use("name");
    src->assign(ValueList{rec({{"id", 1}, {"name", "a"}}), rec({{"id", 2}, {"name", "b"}}), rec({{"id", 1}, {"name", "c"}})});
    h.settle();
    CHECK(src->snapshot()->rowCount() == 2);
    CHECK(h.reported(morph::ui::detail::site::kDuplicateKey) - reports == 1);   // once per duplicate
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = "name", .dir = table::SortDirection::Descending}}));  h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 1});            // "b", then the first "a"; never "c"
    CHECK(cellAt(*src, 0, "name") == table::Cell{std::string{"a"}});
}

// Mutation: Timestamp cells kept in milliseconds (row 2 reads 1'700'000'000'500 and sorts apart from row 1).
TEST_CASE("value rows source: a timestamp is a DateTime of whole seconds", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("at")});  src->use("at");
    src->assign(ValueList{rec({{"id", 1}, {"at", ts(1'700'000'000'000)}}), rec({{"id", 2}, {"at", ts(1'700'000'000'500)}}),
                          rec({{"id", 3}, {"at", ts(-1)}})});
    h.settle();
    CHECK(kindOf(*src, "at") == table::ColumnKind::DateTime);
    CHECK(cellAt(*src, 0, "at") == table::Cell{std::int64_t{1'700'000'000}});
    CHECK(cellAt(*src, 1, "at") == table::Cell{std::int64_t{1'700'000'000}});   // the same second: they tie
    CHECK(cellAt(*src, 2, "at") == table::Cell{std::int64_t{-1}});              // floor, not truncation toward 0
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = "at", .dir = table::SortDirection::Descending}}));  h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{1, 2, 3});         // the tie keeps source order
}

// Mutations: a missing case in the inference (a kind differs); an empty column given no kind (the "e" entry);
// a mixed Int and Decimal column inferred from its first cell (Integer, with one invalid cell).
TEST_CASE("value rows source: every value kind infers its column kind", "[ui][table]") {
    using enum table::ColumnKind;
    RowsFixture h;
    auto const src = sourceOf(h, {col("b"), col("i"), col("d"), col("s"), col("t"), col("q"), col("l"), col("id_d"),
                                  col("e"), col("n", Number)}, testUnits());  // testUnits(): kg and g of mass
    for (auto const* id : {"b", "i", "d", "s", "t", "q", "l", "id_d", "e", "n"}) { src->use(id); }
    src->assign(ValueList{
        rec({{"id", 1}, {"b", true}, {"i", 4}, {"d", dec("2.50")}, {"s", "x"}, {"t", ts(1'000)}, {"q", qty("3", "kg")},
             {"l", 7}, {"id_d", 4}, {"e", Value{}}, {"n", dec("0.5")}}),
        rec({{"id", 2}, {"b", false}, {"i", 5}, {"d", dec("0.125")}, {"s", "y"}, {"t", ts(2'000)}, {"q", qty("3", "zz")},
             {"l", ValueList{}}, {"id_d", dec("0.5")}, {"e", Value{}}, {"n", 2}})});
    h.settle();
    std::map<std::string, table::ColumnKind> kinds;
    for (table::ColumnInfo const& column : src->columns()) { kinds.emplace(column.id, column.kind); }
    CHECK(kinds == std::map<std::string, table::ColumnKind>{{"b", Bool}, {"i", Integer}, {"d", Decimal}, {"s", Text},
        {"t", DateTime}, {"q", Measure}, {"l", Integer}, {"id_d", Decimal}, {"e", Text}, {"n", Number}});
    CHECK(src->invalidCells("q") == 1);                                       // the second row's unit "zz" is unknown
    CHECK(src->invalidCells("l") == 1);                                       // the list cell
    CHECK(src->invalidCells("id_d") == 0);                                    // an integer is an exact decimal
    CHECK(cellAt(*src, 0, "n") == table::Cell{0.5});  CHECK(cellAt(*src, 1, "n") == table::Cell{2.0});
    RecordingListener listener{*src};
    src->assign(ValueList{rec({{"id", 1}, {"e", 9}})});  h.settle();          // the empty column's first vote
    CHECK(kindOf(*src, "e") == Integer);
    CHECK(listener.changes.back().kind == table::ChangeKind::Reset);
}

// Mutations: every row given run 0, as an index computed after the sort would leave it (groups split: 2, 3, 1);
// the run column named plain "group" (the source has two columns of that id, and the count CHECK reads 2).
TEST_CASE("value rows source: a grouped table sorts within its runs", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name"), col("group")});   // a document column named "group"
    src->setGroup([](Value const& row) { return *row.field("site"); }, {});
    src->use("name");  src->use("group");
    src->assign(ValueList{rec({{"id", 1}, {"site", "A"}, {"name", "z"}, {"group", "q"}}),
                          rec({{"id", 2}, {"site", "A"}, {"name", "a"}, {"group", "r"}}),
                          rec({{"id", 3}, {"site", "B"}, {"name", "m"}, {"group", "p"}})});
    h.settle();
    CHECK(std::ranges::count(src->columns(), std::string{"group"}, &table::ColumnInfo::id) == 1);
    CHECK(std::ranges::count(src->columns(), std::string{kGroupRunColumn}, &table::ColumnInfo::id) == 1);
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = std::string{kGroupRunColumn}}, {.column = "name"}}));  h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 1, 3});
    REQUIRE(engine->setSort({{.column = std::string{kGroupRunColumn}}, {.column = "group"}}));  h.settle();
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{1, 2, 3});         // by the document's "group", in runs
    CHECK(src->runRecords().size() == 2);  CHECK(src->runOf(2) == 1);
}

// Mutation: fold inside the comparison (about 3.4 million folds).  Gate U3.
TEST_CASE("value rows source: a sort of 100,000 rows folds each text once", "[ui][table]") {
    RowsFixture h;  auto const collator = std::make_shared<ValueRowsCollator>();
    auto const src = sourceOf(h, {col("name")});  src->use("name");  src->assign(sampleRows(100'000));  h.settle();
    auto engine = engineOver(h, src, collator);
    REQUIRE(engine->setSort({{.column = "name"}}));  h.settle();
    CHECK(collator->folds() == 100'000);
    CHECK(engine->stats().fullSorts == 1);
}

// Mutations: a fresh decode deriving every column (cellsDerived is 300,000); the changed row sorting the list
// (fullSorts grows).  Gates U2 and U4, as counts.
TEST_CASE("value rows source: one changed row derives one column and repairs", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name"), col("notes"), col("amount")});
    ValueList const rows = sampleRows(10'000);
    src->use("name");  src->assign(rows);  h.settle();
    auto engine = engineOver(h, src);  REQUIRE(engine->setSort({{.column = "name"}}));  h.settle();
    CHECK(src->cellsHeld() == 10'000);                                        // U4: one column's cells
    table::EngineStats const before = engine->stats();  std::size_t const derived = src->cellsDerived();
    src->assign(withField(freshDecodeOf(rows), 42, "name", Value{std::string{"zzz"}}));  h.settle();
    CHECK(src->cellsDerived() - derived == 10'000);                           // U2: the column in use only
    CHECK(engine->stats().repairs - before.repairs == 1);
    CHECK(engine->stats().fullSorts == before.fullSorts);
    CHECK(keysInView(*engine).back() == 42);
}

namespace {

// The counts a mounted grouped table's headings show, in order; each heading's text is its run's `count`.
std::vector<std::string> headingCounts(morph::ui::testing::Harness const& h) {
    std::vector<std::string> counts;
    for (int const row : h.backend.children(h.backend.all("Table").front())) {
        if (h.backend.prop(row, "heading") != "true") {
            continue;
        }
        int cell = row;
        while (h.backend.kindOf(cell) != "Text" && !h.backend.children(cell).empty()) {
            cell = h.backend.children(cell).front();
        }
        counts.push_back(h.backend.prop(cell, "text"));
    }
    return counts;
}

}  // namespace

// Mutation: runs broken where `Value ==` differs (three runs of one: 1 and 1.0 and 1.00 split; the counts read 1, 1,
// 1, 1).
TEST_CASE("value rows source: a group's runs are the interpreter's runs", "[ui][table]") {
    // `g` is 1, then 1.0, then 1.00, then 2: one value spelled three ways, then another.
    std::string const rows = R"([ { "id": 1, "g": 1 }, { "id": 2, "g": { "num": 1, "den": 1, "dp": 1 } },
        { "id": 3, "g": { "num": 1, "den": 1, "dp": 2 } }, { "id": 4, "g": 2 } ])";
    morph::ui::testing::Harness interpreter;
    auto screen = interpreter.mount(R"({ "vocab": "ui/1", "id": "runs",
      "state": { "items": { "init": { "lit": )" + rows + R"( } } },
      "view": { "kind": "table", "rows": { "ref": "items" },
                "group": { "by": { "row": "g" }, "heading": { "kind": "text", "text": { "fn": "format",
                           "args": [ "{0}", { "row": "count" } ] } } },
                "columns": [ { "id": "id", "label": "Id" } ] } })");
    REQUIRE(headingCounts(interpreter) == std::vector<std::string>{"3", "1"});

    RowsFixture h;  auto const src = sourceOf(h, {col("id")});
    src->setGroup([](Value const& row) { return *row.field("g"); }, {});
    auto const json = morph::ui::parseJson(rows);
    REQUIRE(json.has_value());
    auto const list = morph::ui::valueFromJson(*json);
    REQUIRE(list.has_value());
    src->assign(*list->asList());  h.settle();
    std::vector<std::string> counts;
    for (Value const& record : src->runRecords()) { counts.push_back(std::to_string(*record.field("count")->asInt())); }
    CHECK(counts == headingCounts(interpreter));
    CHECK(*src->runRecords().front().field("key") == Value{1});                // the run's first row's `by`
    CHECK(src->runOf(2) == 0);  CHECK(src->runOf(3) == 1);
}

// Mutation: the row value not compared when its cells are equal (no change is notified, and the record still reads
// "x").
TEST_CASE("value rows source: a field no column reads is redrawn in place", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name"), col("notes")});
    src->use("name");  src->assign(sampleRows(10));  h.settle();
    EngineRows rows{src, src->recordOf(), table::EngineOptions{.owner = &h.owner}};
    REQUIRE(rows.setSort({{.column = "name"}}));  h.settle();
    RecordingListener listener{*src};
    auto const viewOf7 = [&rows] {
        for (std::int64_t at = 0; at < rows.total().rows; ++at) {
            if (rows.row(at)->key == table::RowId{std::int64_t{7}}) { return at; }
        }
        return std::int64_t{-1};
    };
    std::int64_t const before = viewOf7();
    CHECK(*rows.row(before)->row.field("notes")->asString() == "x");
    std::size_t const sorts = rows.engine().stats().fullSorts;
    src->assign(withField(freshDecodeOf(sampleRows(10)), 7, "notes", Value{std::string{"y"}}));  h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Updated);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{7});
    CHECK(viewOf7() == before);                                               // in place
    CHECK(*rows.row(before)->row.field("notes")->asString() == "y");          // read again
    CHECK(rows.engine().stats().fullSorts == sorts);
}

// Mutation: a changed heading record not notified (the change names row 1 alone).
TEST_CASE("value rows source: a changed run heading is an update of the run's first row", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name")});
    src->setGroup([](Value const& row) { return *row.field("site"); }, {});
    src->use("name");
    ValueList const rows{rec({{"id", 1}, {"site", "A"}, {"name", "a"}, {"notes", "x"}}),
                         rec({{"id", 2}, {"site", "A"}, {"name", "b"}, {"notes", "x"}}),
                         rec({{"id", 3}, {"site", "B"}, {"name", "c"}, {"notes", "x"}})};
    src->assign(rows);  h.settle();
    RecordingListener listener{*src};
    src->assign(withField(rows, 1, "notes", Value{std::string{"y"}}));  h.settle();   // run A's `last` changes
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Updated);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{0, 1});    // row 1, and run A's heading at row 0
    CHECK(*src->runRecords().front().field("last")->field("notes") == Value{std::string{"y"}});
}

// Mutations: the first cell's kind taken as it is (the number-first column reads Integer, with 2 invalid cells);
// the clause applied whatever the first cell's kind (the text-first column reads Decimal, with 1 invalid cell).
TEST_CASE("value rows source: a text among numbers is one invalid cell", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("nf"), col("tf")});
    src->use("nf");  src->use("tf");
    src->assign(ValueList{rec({{"id", 1}, {"nf", 4}, {"tf", "x"}}), rec({{"id", 2}, {"nf", dec("0.5")}, {"tf", 4}}),
                          rec({{"id", 3}, {"nf", "x"}, {"tf", dec("0.5")}})});
    h.settle();
    CHECK(kindOf(*src, "nf") == table::ColumnKind::Decimal);                  // integers with decimals: one Decimal
    CHECK(src->invalidCells("nf") == 1);                                      // and the text is the one invalid cell
    CHECK(cellAt(*src, 0, "nf") == table::Cell{morph::math::Rational{4, morph::math::DecimalPlaces{0}}});
    CHECK(kindOf(*src, "tf") == table::ColumnKind::Text);                     // text first: a Text column
    CHECK(src->invalidCells("tf") == 2);                                      // and both numbers are invalid
}

// Mutations: an insertion notified as Reset (the Inserted CHECK); a removal notified as Reset (the Removed
// CHECK).
TEST_CASE("value rows source: a new key is inserted and a missing key removed", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("name")});  src->use("name");
    ValueList rows = sampleRows(3);
    src->assign(rows);  h.settle();
    auto engine = engineOver(h, src);
    REQUIRE(engine->setSort({{.column = "name", .dir = table::SortDirection::Descending}}));  h.settle();
    RecordingListener listener{*src};
    rows.push_back(rec({{"id", 3}, {"name", "a"}}));
    src->assign(rows);  h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Inserted);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{3});
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 1, 0, 3});
    listener.clear();
    src->assign(ValueList{rows.at(0), rows.at(2), rows.at(3)});  h.settle();
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Removed);
    CHECK(listener.changes.front().rows == std::vector<std::size_t>{1});      // its index in the old rows
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 0, 3});
    listener.clear();
    src->assign(ValueList{rows.at(2), rows.at(0)});  h.settle();               // one key gone and the rest reordered
    REQUIRE(listener.changes.size() == 1);
    CHECK(listener.changes.front().kind == table::ChangeKind::Reset);
    CHECK(keysInView(*engine) == std::vector<std::int64_t>{2, 0});
}

// Mutations: days truncated toward 0 (an instant 1 ms before the epoch reads day 0); a {num, den, unit} record not
// read as a measure (the column reads Text).
TEST_CASE("value rows source: a literal date and a measure record", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("d", table::ColumnKind::Date), col("m")}, testUnits());
    src->use("d");  src->use("m");
    src->assign(ValueList{
        rec({{"id", 1}, {"d", ts(86'400'000 + 1)}, {"m", rec({{"num", 3}, {"den", 2}, {"unit", "kg"}})}}),
        rec({{"id", 2}, {"d", ts(-1)}, {"m", rec({{"num", 1}, {"den", 1}, {"unit", "zz"}})}}),
        rec({{"id", 3}, {"d", "1970-01-03"}, {"m", Value{}}}),
        rec({{"id", 4}, {"d", "not a date"}, {"m", Value{}}})});
    h.settle();
    CHECK(cellAt(*src, 0, "d") == table::Cell{std::int64_t{1}});
    CHECK(cellAt(*src, 1, "d") == table::Cell{std::int64_t{-1}});              // the day before the epoch
    CHECK(cellAt(*src, 2, "d") == table::Cell{std::int64_t{2}});               // ISO text
    CHECK(src->invalidCells("d") == 1);                                        // text that is no date
    CHECK(kindOf(*src, "m") == table::ColumnKind::Measure);
    auto const kg = testUnits()->find("kg");
    REQUIRE(kg.has_value());
    CHECK(cellAt(*src, 0, "m") == table::Cell{table::Measure{
        .amount = morph::math::Rational{morph::math::Numerator{3}, morph::math::Denominator{2},
                                        morph::math::DecimalPlaces{1}},
        .unit = *kg}});
    CHECK(src->invalidCells("m") == 1);                                        // the unit "zz" is unknown
}

// ── A table over `rows` on the engine ───────────────────────────────────────

using morph::ui::Refusal;
using morph::ui::testing::coreOf;
using morph::ui::testing::describedFrom;
using morph::ui::testing::diagnosticsMentioning;
using morph::ui::testing::elementOf;
using morph::ui::testing::engineOf;
using morph::ui::testing::idRows;
using morph::ui::testing::kRowsSamples;
using morph::ui::testing::kThree;
using morph::ui::testing::mixedRows;
using morph::ui::testing::pagesOf;
using morph::ui::testing::rowOrder;
using morph::ui::testing::rowsDoc;
using morph::ui::testing::rowsDocNoId;

// Mutation: the interpreter's pipeline left in place for `rows` tables (the accented names order apart).
TEST_CASE("one order on every surface: a rows table and a model table order alike", "[ui][table]") {
    std::string const rows = mixedRows();
    for (std::vector<std::size_t> const& clicks : {std::vector<std::size_t>{1},          // by name
                                                   std::vector<std::size_t>{1, 1},       // by name, descending
                                                   std::vector<std::size_t>{2}}) {       // by weight
        TableHarness asRows;  auto rowsScreen = asRows.mount(rowsDoc(rows));
        TableHarness asModel;  auto modelScreen = asModel.mount(kRowsSamples);
        replyDescribe(asModel, describedFrom(rows));
        replyPage(asModel, pagesOf(rows, lastQuery(asModel)));
        for (std::size_t column : clicks) {
            asRows.backend.clickHeader(tableOf(asRows), column);  asRows.settle();
            asModel.backend.clickHeader(tableOf(asModel), column);  asModel.settle();
            replyPage(asModel, pagesOf(rows, lastQuery(asModel)));
        }
        asModel.backend.scrollTable(tableOf(asModel), 0, 200);  asModel.settle();  // a model table is windowed
        CHECK(rowOrder(asRows) == rowOrder(asModel));
        CHECK_FALSE(rowOrder(asRows).empty());
    }
}

// Mutation: sorting the whole list on a row change (fullSorts grows).  Gate U2 counts, on a screen.
TEST_CASE("rows table: one changed row is one repair", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(rowsDoc(kThree));
    h.backend.clickHeader(tableOf(h), 1);  h.settle();
    REQUIRE(rowOrder(h) == "a b c");
    table::EngineStats const before = engineOf(*screen, "list").stats();
    h.backend.click(h.byTestId("Button", "rename"));  h.settle();
    CHECK(engineOf(*screen, "list").stats().repairs - before.repairs == 1);
    CHECK(engineOf(*screen, "list").stats().fullSorts == before.fullSorts);
    CHECK(rowOrder(h) == "a c b");                                            // "zz" moved last
}

// Mutations: no bound check (no warning); the bound read as `>=` (a warning at 20,000); a warning per change of the
// list (two, for the list that grows to 20,002 rows: the screen drops only a repeat of the same words).
TEST_CASE("rows table: a list over 20,000 rows is reported once", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(rowsDoc(idRows(20'001)));
    std::string const warning = "table list holds 20,001 rows; a table over rows is ordered on the client up to "
                                "20,000 rows or 1,000,000 cells — serve it as a model's table";
    CHECK(diagnosticsMentioning(h, warning) == 1);
    h.backend.click(h.byTestId("Button", "rename"));  h.settle();             // the list changes: no second report
    CHECK(diagnosticsMentioning(h, warning) == 1);
    // A list of another size would word its report apart: still the one report.
    std::string const growing = [] {
        std::string text = rowsDoc(idRows(20'001));
        std::string const renamed = morph::ui::testing::renamedB(idRows(20'001));
        text.replace(text.find(renamed), renamed.size(), idRows(20'002));
        return text;
    }();
    TableHarness grows;  auto grown = grows.mount(growing);
    grows.backend.click(grows.byTestId("Button", "rename"));  grows.settle();
    CHECK(diagnosticsMentioning(grows, "serve it as a model's table") == 1);
    TableHarness under;  auto small = under.mount(rowsDoc(idRows(20'000)));
    CHECK(diagnosticsMentioning(under, "serve it as a model's table") == 0);  // at the bound: nothing
}

// Mutation: drawing the rows before the load settles (the first CHECK reads "a b c").
TEST_CASE("rows table: persisted state before the first rows", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(),
                                                        R"({"version":1,"sort":[{"column":"name","direction":"desc"}]})");
    TableHarness h{store};  auto screen = h.mount(rowsDoc(kThree));
    CHECK(rowOrder(h).empty());                                               // placeholders only while restoring
    CHECK(h.backend.children(tableOf(h)).size() == 3);
    CHECK(store->lastLoadKey() == table::TableStateKey{.application = "lims", .model = "", .table = "list",
                                                         .qualifier = "samples", .part = "state"});
    store->release();  h.settle();
    CHECK(rowOrder(h) == "c b a");                                            // first drawn: the stored order
    h.backend.clickHeader(tableOf(h), 1);  h.advance(500ms);  CHECK(store->saves() == 1);
}

// Mutation: a rows table without `id` given a key (a load goes out with table "").
TEST_CASE("rows table: a table without an id persists nothing", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};  auto screen = h.mount(rowsDocNoId(kThree));
    CHECK(rowOrder(h) == "a b c");                                            // no restoring state
    h.backend.clickHeader(h.backend.all("Table").front(), 1);  h.advance(500ms);
    CHECK(store->loads() == 0);  CHECK(store->saves() == 0);
}

// Mutations: `shown` always true for a rows table; a message under `status: false` kept in the line.
TEST_CASE("rows table: the status line takes space only while it has something to say", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(rowsDoc(kThree));
    CHECK_FALSE(statusOf(h).shown);
    coreOf(*screen).tableMessage(elementOf(*screen, "list"), "Refused");  h.settle();
    CHECK(statusOf(h).shown);  CHECK(statusOf(h).message == "Refused");  CHECK(statusOf(h).counts == "3 rows");
    h.advance(6'000ms);  CHECK_FALSE(statusOf(h).shown);
    TableHarness always;  auto shown = always.mount(rowsDoc(kThree, R"(,"status":true)"));
    CHECK(statusOf(always).shown);  CHECK(statusOf(always).counts == "3 rows");
    TableHarness never;  auto hidden = never.mount(rowsDoc(kThree, R"(,"status":false)"));
    coreOf(*hidden).tableMessage(elementOf(*hidden, "list"), "Refused");  never.settle();
    CHECK_FALSE(statusOf(never).shown);  CHECK(statusOf(never).message.empty());
}

// Mutations: no report of an invalid sort cell (the count is 0); one per view change (two, once a second column's
// invalid cell words the report apart).
TEST_CASE("rows table: a cell not of its column's kind is reported once at the sort path", "[ui][table]") {
    TableHarness h;
    auto screen = h.mount(rowsDoc(R"([{"id":"a","weight":1},{"id":"b","weight":"heavy"},{"id":"c","weight":2}])"));
    auto const sortReports = [&h] {
        return std::ranges::count_if(h.diagnostics, [](Refusal const& r) { return r.path.ends_with("/sort"); });
    };
    h.backend.clickHeader(tableOf(h), 2);  h.settle();
    CHECK(rowOrder(h) == "a c b");                                            // the invalid cell sorts last
    CHECK(sortReports() == 1);
    h.backend.clickHeader(tableOf(h), 2);  h.settle();
    CHECK(rowOrder(h) == "c a b");  CHECK(sortReports() == 1);                // last in either direction, once
    TableHarness two;
    auto both = two.mount(rowsDoc(R"([{"id":"a","name":"x","weight":1},{"id":"b","name":2,"weight":"heavy"},)"
                                  R"({"id":"c","name":"y","weight":2}])"));
    two.backend.clickHeader(tableOf(two), 2);  two.settle();
    two.backend.clickHeader(tableOf(two), 1);  two.settle();                  // another column, another invalid cell
    CHECK(std::ranges::count_if(two.diagnostics, [](Refusal const& r) { return r.path.ends_with("/sort"); }) == 1);
}

namespace {

// A grouped rows table: `g` groups, each heading shows its run's count, the column `note` is drawn by the default
// cell and read by no sort; the button `note` gives row 1 a new note, and `join` then gives row 3 the group of row 2.
constexpr std::string_view kGroupedRows = R"({"vocab":"ui/1","id":"samples",
  "state":{"items":{"init":{"lit":[{"id":1,"g":"x","note":"one"},{"id":2,"g":"x","note":"two"},
                                   {"id":3,"g":"y","note":"three"}]}}},
  "view":{"kind":"column","children":[
    {"kind":"table","id":"list","testId":"list","rows":{"ref":"items"},"key":{"row":"id"},
     "group":{"by":{"row":"g"},"heading":{"kind":"text","text":{"fn":"format","args":["{0}",{"row":"count"}]}}},
     "columns":[{"id":"id","label":"Id","sortable":true},{"id":"note","label":"Note"}]},
    {"kind":"button","testId":"note","label":"Note","onClick":[{"set":"items","value":{"lit":[
      {"id":1,"g":"x","note":"uno"},{"id":2,"g":"x","note":"two"},{"id":3,"g":"y","note":"three"}]}}]},
    {"kind":"button","testId":"join","label":"Join","onClick":[{"set":"items","value":{"lit":[
      {"id":1,"g":"x","note":"uno"},{"id":2,"g":"x","note":"two"},{"id":3,"g":"x","note":"three"}]}}]}]}})";

// The texts of the headings of the harness's first table, in order.
std::vector<std::string> headingTexts(TableHarness const& h) {
    std::vector<std::string> out;
    for (int const row : h.backend.children(h.backend.all("Table").front())) {
        if (h.backend.prop(row, "heading") != "true") {
            continue;
        }
        int cell = row;
        while (h.backend.kindOf(cell) != "Text" && !h.backend.children(cell).empty()) {
            cell = h.backend.children(cell).front();
        }
        out.push_back(h.backend.prop(cell, "text"));
    }
    return out;
}

}  // namespace

// Mutation: `EngineRows` keeping its record of an `Updated` row (the note stays "one").
TEST_CASE("rows table: a field no sort reads redraws its row, and a heading's count follows its run", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(kGroupedRows);
    h.backend.clickHeader(tableOf(h), 0);  h.settle();
    REQUIRE(headingTexts(h) == std::vector<std::string>{"2", "1"});
    REQUIRE(shownText(h, "one"));
    h.backend.click(h.byTestId("Button", "note"));  h.settle();              // the runs stay: an `Updated` row
    CHECK(shownText(h, "uno"));
    CHECK_FALSE(shownText(h, "one"));
    CHECK(headingTexts(h) == std::vector<std::string>{"2", "1"});
    h.backend.click(h.byTestId("Button", "join"));  h.settle();              // row 3 joins the first run
    CHECK(headingTexts(h) == std::vector<std::string>{"3"});
}

// Mutations: `TableSource::rowScopedCells` empty on a rows table (c's cells, rebound from a's place, show a's draft);
// every cell of a row dropped when its place goes to another key (the plain `label` cell is built again: 3 cells).
TEST_CASE("rows table: a row's cells share its scope, and a row given another's place builds its own", "[ui][table]") {
    constexpr char const* kDrafts = R"({"vocab":"ui/1","id":"drafts",
      "state":{"items":{"init":{"lit":[{"id":"a","name":"a","label":"A"},{"id":"b","name":"b","label":"B"}]}}},
      "view":{"kind":"column","children":[
        {"kind":"table","id":"list","rows":{"ref":"items"},"key":{"row":"id"},
         "scope":{"state":{"draft":{"init":{"row":"name"}}}},
         "columns":[
           {"id":"edit","label":"Edit","cell":{"kind":"textInput","testId":"edit","commitOn":"change",
              "value":{"ref":"draft"},"onChange":[{"set":"draft","value":{"ref":"event"}}]}},
           {"id":"shown","label":"Shown","cell":{"kind":"text","testId":"shown","text":{"ref":"draft"}}},
           {"id":"label","label":"Label"}]},
        {"kind":"button","testId":"swap","label":"Swap","onClick":[{"set":"items","value":{"lit":[
          {"id":"b","name":"b","label":"B"},{"id":"c","name":"c","label":"C"}]}}]}]}})";
    TableHarness h;  auto screen = h.mount(kDrafts);
    REQUIRE(shownText(h, "a"));
    h.backend.edit(h.byTestId("TextInput", "edit"), "edited");  h.settle();  // the first row's input: a's
    CHECK(shownText(h, "edited"));                                            // its text cell reads the same draft
    CHECK_FALSE(shownText(h, "a"));
    MountStats const before = statsOf(*screen);
    h.backend.click(h.byTestId("Button", "swap"));  h.settle();              // c takes the place a left
    CHECK(shownText(h, "b"));
    CHECK(shownText(h, "c"));
    CHECK_FALSE(shownText(h, "edited"));
    CHECK(shownText(h, "C"));                                                 // the plain cell, rebound to c
    CHECK_FALSE(shownText(h, "A"));
    CHECK(statsOf(*screen).cellsCreated - before.cellsCreated == 2);          // only the two scoped cells built
}

// Mutation: a page of focus taken from the reported range of a document's rows, empty before any report (PageDown
// moves one row).
TEST_CASE("rows table: before the backend reports a range, a page of focus is the overscan's rows", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(rowsDoc(idRows(50)));
    int const table = tableOf(h);
    h.backend.focusMove(table, morph::ui::FocusMove::PageDown);  h.settle();
    CHECK(h.backend.focusCell(table).first == 10);                            // Virtualization::overscan
}

// Mutation: `setGroup` given an empty unit table (1 kg and 1000 g are two runs).
TEST_CASE("value rows source: a group's quantities are compared through the screen's units", "[ui][table]") {
    RowsFixture h;  auto const src = sourceOf(h, {col("id")});
    morph::ui::UnitTable const units{
        {"g", morph::ui::UnitInfo{.canonical = "g", .toCanonical = *table::parseDecimal("1"), .display = {}}},
        {"kg", morph::ui::UnitInfo{.canonical = "g", .toCanonical = *table::parseDecimal("1000"), .display = {}}}};
    src->setGroup([](Value const& row) { return *row.field("w"); }, units);
    src->assign(ValueList{rec({{"id", 1}, {"w", qty("1", "kg")}}), rec({{"id", 2}, {"w", qty("1000", "g")}}),
                          rec({{"id", 3}, {"w", qty("2", "kg")}})});
    h.settle();
    CHECK(src->runRecords().size() == 2);
    CHECK(src->runOf(1) == 0);
    CHECK(src->runOf(2) == 1);
}

// Mutations: `kind` not in kColumnKeys (the bad kind loads); no `persist` check on a rows table without an id.
TEST_CASE("ui document: a rows table's column kind and its persist need what they name", "[ui][document]") {
    using morph::ui::testing::tableCatalog;
    using morph::ui::testing::withColumnKey;
    CHECK(loads(rowsDoc(kThree, R"(,"persist":"mine")"), tableCatalog()));
    CHECK(refusalOf(withColumnKey(rowsDoc(kThree), "name", R"("kind":"money")")) ==
          "`kind` is one of text, integer, decimal, number, date, dateTime, bool");
    CHECK(loads(withColumnKey(rowsDoc(kThree), "name", R"("kind":"text")"), tableCatalog()));
    CHECK(refusalOf(rowsDocNoId(kThree, R"(,"persist":true)")) ==
          "a table over `rows` needs an `id` to persist its arrangement");
    CHECK(loads(rowsDocNoId(kThree, R"(,"persist":false)"), tableCatalog()));
    CHECK(loads(rowsDocNoId(kThree), tableCatalog()));
}

// Mutation: no source made for the units the rows bring (every quantity in `m` an invalid cell, left in source
// order and reported at the sort path).
TEST_CASE("rows table: a quantity in a unit the screen does not list sorts by its value", "[ui][table]") {
    TableHarness h;
    auto screen = h.mount(rowsDoc(R"([{"id":"a","weight":{"num":3,"den":1,"dp":0,"unit":"m"}},)"
                                  R"({"id":"b","weight":{"num":1,"den":1,"dp":0,"unit":"m"}},)"
                                  R"({"id":"c","weight":{"num":2,"den":1,"dp":0,"unit":"m"}}])"));
    h.backend.clickHeader(tableOf(h), 2);  h.settle();
    CHECK(rowOrder(h) == "b c a");
    CHECK(std::ranges::none_of(h.diagnostics, [](Refusal const& r) { return r.path.ends_with("/sort"); }));
}

// Mutation: the lowering grouping with an empty unit table (1 kg and 1000 g are two runs: headings 1, 1, 1).
TEST_CASE("rows table: a group's quantities are compared through the screen's units", "[ui][table]") {
    TableHarness h;
    FormatContext format = formatFor("en");
    format.units.insert_or_assign("g", morph::ui::UnitInfo{.canonical = "g", .toCanonical = *table::parseDecimal("1"),
                                                           .display = {}});
    format.units.insert_or_assign("kg", morph::ui::UnitInfo{.canonical = "g",
                                                            .toCanonical = *table::parseDecimal("1000"), .display = {}});
    h.session.setFormatting(format);
    auto screen = h.mount(R"({"vocab":"ui/1","id":"samples",
      "state":{"items":{"init":{"lit":[{"id":1,"w":{"num":1,"den":1,"dp":0,"unit":"kg"}},
                                       {"id":2,"w":{"num":1000,"den":1,"dp":0,"unit":"g"}},
                                       {"id":3,"w":{"num":2,"den":1,"dp":0,"unit":"kg"}}]}}},
      "view":{"kind":"table","id":"list","testId":"list","rows":{"ref":"items"},"key":{"row":"id"},
              "group":{"by":{"row":"w"},"heading":{"kind":"text","text":{"fn":"format","args":["{0}",{"row":"count"}]}}},
              "columns":[{"id":"id","label":"Id"}]}})");
    CHECK(headingTexts(h) == std::vector<std::string>{"2", "1"});
}

// Mutation: the page always sending the mount a reset (one reset, and no change of the row alone).
TEST_CASE("rows table: one changed row reaches the mount as its own change, not a reset", "[ui][table]") {
    TableHarness h;  auto screen = h.mount(rowsDoc(kThree));
    h.backend.clickHeader(tableOf(h), 1);  h.settle();
    REQUIRE(rowOrder(h) == "a b c");
    MountStats const before = statsOf(*screen);
    h.backend.click(h.byTestId("Button", "rename"));  h.settle();
    CHECK(rowOrder(h) == "a c b");
    CHECK(statsOf(*screen).viewChanges - before.viewChanges == 1);
    CHECK(statsOf(*screen).viewResets == before.viewResets);
    CHECK(statsOf(*screen).cellsCreated == before.cellsCreated);              // b kept its widgets as it moved
}

// Mutation: the rebuild for a brought unit not carrying the engine's filter over (the new engine has none).
TEST_CASE("rows table: a unit the rows bring keeps the engine's filter", "[ui][table]") {
    std::string const quantities = R"([{"id":"a","name":"a","weight":{"num":1,"den":1,"dp":0,"unit":"m"}},)"
                                   R"({"id":"b","name":"b","weight":{"num":2,"den":1,"dp":0,"unit":"m"}}])";
    std::string text = rowsDoc(kThree);
    std::string const renamed = morph::ui::testing::renamedB(kThree);
    text.replace(text.find(renamed), renamed.size(), quantities);
    TableHarness h;  auto screen = h.mount(text);
    table::FilterSpec filter;
    filter.columns["name"].include.push_back(table::FilterEntry{.contains = "a"});
    REQUIRE(engineOf(*screen, "list").setFilter(filter));
    table::Engine const* const first = &engineOf(*screen, "list");
    h.backend.click(h.byTestId("Button", "rename"));  h.settle();             // the rows bring the unit `m`
    REQUIRE(&engineOf(*screen, "list") != first);                              // a new engine
    CHECK(engineOf(*screen, "list").filter() == filter);
}

namespace {

using morph::ui::ActionInfo;
using morph::ui::testing::addAction;
using morph::ui::testing::addPageSamples;
using morph::ui::testing::cellOf;
using morph::ui::testing::kActionTable;
using morph::ui::testing::kActionTableNoColumns;
using morph::ui::testing::kActionTableNoQuery;
using morph::ui::testing::kindOfColumn;
using morph::ui::testing::refusalOf;
using morph::ui::testing::unitOf;
using morph::ui::testing::windowOf;

/// The x-table request of the typed list actions these cases add.
constexpr std::string_view kQuerySchema = R"({"type":"object","properties":{"query":{"type":"object","x-table":true}}})";

/// Whether the body of the last call of @p action names @p text.
bool lastBodyHas(TableHarness const& h, std::string_view action, std::string_view text) {
    return h.dispatcher.calls.at(h.dispatcher.last(action)).body.contains(text);
}

}  // namespace

// Mutations: `{num, den, dp}` read as text (weight is Integer, from 3 alone, and not a Rational); a fractional number voted
// Decimal (ratio is Decimal); the query written at the body's top level (no "query" member in the body).
TEST_CASE("model table over an action: a typed list action's rows read into the window", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kActionTable);
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);                     // no describe call: none exists
    CHECK(h.dispatcher.calls.back().body.contains(R"("query":{)"));          // the query in the x-table member
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a","weight":{"num":5,"den":2,"dp":1},"ratio":0.5,"day":"2026-11-17"},
                        {"id":2,"name":"b","weight":3,"ratio":2,"day":null}],"total":2})");
    using enum table::ColumnKind;
    CHECK(kindOfColumn(*screen, "id") == Integer);    CHECK(kindOfColumn(*screen, "name") == Text);
    CHECK(kindOfColumn(*screen, "weight") == Decimal);                        // {num,den,dp} with an integer
    CHECK(kindOfColumn(*screen, "ratio") == Number);                          // a fraction with an integer
    CHECK(kindOfColumn(*screen, "day") == Date);                              // the literal kind
    CHECK(*cellOf(*screen, 0, "ratio") == table::Cell{0.5});  CHECK(*cellOf(*screen, 1, "ratio") == table::Cell{2.0});
    CHECK(std::holds_alternative<morph::math::Rational>(*cellOf(*screen, 1, "weight")));
    CHECK(*cellOf(*screen, 0, "day") == table::Cell{std::int64_t{20'774}});   // days since 1970-01-01
    CHECK(std::holds_alternative<std::monostate>(*cellOf(*screen, 1, "day")));
    CHECK(statusOf(h).counts == "2 rows");
    h.backend.clickHeader(tableOf(h), 1);  h.settle();
    CHECK(h.dispatcher.calls.back().body.contains(R"("sort":[{"column":"name")"));
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":3,"name":"c","weight":"heavy","ratio":1}],"total":1})");
    CHECK(kindOfColumn(*screen, "weight") == Decimal);                        // fixed by the first reply
}

// Mutation: the view document's `kind` ignored (id is inferred Text from a reply whose ids are strings).
TEST_CASE("model table over an action: columns and kinds come from the action's view document", "[ui][table]") {
    TableHarness h;                                                           // Lims_ListSamples has a v-columns view
    addAction(h, "Lims_ListSamples", R"({"type":"object","properties":{"query":{"type":"object","x-table":true}}})",
              ActionInfo{.modelType = "Lims_SampleModel"});
    auto screen = h.mount(withTableKey(kActionTableNoColumns, R"("action":"Lims_ListSamples")"));
    h.reply(h.dispatcher.last("Lims_ListSamples"), R"({"rows":[{"id":"7","name":"a"}],"total":1})");
    CHECK(headerLabels(h) == std::vector<std::string>{"ID", "Name"});         // v-columns' labels
    CHECK(elementValue(*screen, "list", "visibleColumns") == R"(["name"])");  // `id` is v-hidden: hidden, not gone
    CHECK(kindOfColumn(*screen, "id") == table::ColumnKind::Integer);         // v-columns' kind, not the reply's
}

// Mutations: the loader accepting an action with no x-table member (the first fetch sends a bare body); accepting
// one with neither `columns` nor a view document; accepting two members marked x-table.
TEST_CASE("model table over an action: an action the table cannot page is refused at load", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    addAction(h, "Lims_TwoQueries",
              R"({"type":"object","properties":{"a":{"type":"object","x-table":true},"b":{"type":"object","x-table":true}}})",
              ActionInfo{.modelType = "Lims_SampleModel"});
    CHECK(refusalOf(h, kActionTableNoQuery) == "a table over an action needs a query member marked x-table");
    CHECK(refusalOf(h, kActionTableNoColumns) ==
          "a table over an action needs its columns, or a view document for the action");
    CHECK(refusalOf(h, withTableKey(kActionTable, R"("action":"Lims_TwoQueries")")) ==
          "a table over an action needs one query member marked x-table; `Lims_TwoQueries` marks 2");
}

// Mutation: no reconnect handler on a window that watches no record (the reconnect fetches nothing: 1 == 2).
TEST_CASE("model table over an action: a reconnect fetches the rows again with no record watched", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kActionTable);
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":1,"name":"a"}],"total":1})");
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);
    h.session.reconnected();  h.settle();
    CHECK(h.dispatcher.count("Lims_PageSamples") == 2);
}

// Mutation: only the first record of the action watched (the second watch is never answered, so no page is
// fetched: 0 == 1).
TEST_CASE("model table over an action: every record the action holds is watched", "[ui][table]") {
    TableHarness h;
    addAction(h, "Lims_PageSamples", std::string{kQuerySchema},
              ActionInfo{.modelType = "Lims_SampleModel",
                         .records = {morph::ui::ActionRecordInfo{.record = "Lims_SampleModel.samples"},
                                     morph::ui::ActionRecordInfo{.record = "Lims_Result"}}});
    auto screen = h.mount(kActionTable);
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);                     // both watches answered
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":1,"name":"a"}],"total":1})");
    morph::wire::ChangesBody body;
    body.records.push_back(morph::effects::RecordChange{.record = "Lims_Result", .keys = std::nullopt});
    h.session.applyChanges(body);  h.settle();
    CHECK(h.dispatcher.count("Lims_PageSamples") == 2);
}

// Mutations: the stored filter fitted to the placeholder kinds (`ratio` dropped: no second query); a held filter
// dropped without a report (no diagnostic for `name`).
TEST_CASE("model table over an action: a stored filter on an inferred column waits for its kind", "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(
        storeExecutor(),
        R"({"version":1,"filters":{"columns":{"ratio":{"include":[{"gt":"1"}]},"name":{"include":[{"gt":"b"}]}}}})");
    TableHarness h{store};  addPageSamples(h);
    auto screen = h.mount(kActionTable);  store->release();  h.settle();
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);
    CHECK_FALSE(lastBodyHas(h, "Lims_PageSamples", R"("gt")"));              // no kind yet: the filter waits
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a","ratio":0.5},{"id":2,"name":"b","ratio":2}],"total":2})");
    REQUIRE(kindOfColumn(*screen, "ratio") == table::ColumnKind::Number);
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 2);                     // the stored filter, now it applies
    CHECK(lastBodyHas(h, "Lims_PageSamples", R"({"gt":"1"})"));
    CHECK_FALSE(lastBodyHas(h, "Lims_PageSamples", R"({"gt":"b"})"));
    CHECK(diagnosticsMentioning(h, "the stored filter on `name` is not applied") == 1);   // Text takes no `gt`
    CHECK(diagnosticsMentioning(h, "`ratio`") == 0);
}

// Mutation: the view document's units not read (mass stays a Quantity column and lists neither kg nor g).
TEST_CASE("model table over an action: a view document's units read its measures", "[ui][table]") {
    TableHarness h;
    addAction(h, "Lims_WeighSamples", std::string{kQuerySchema}, ActionInfo{.modelType = "Lims_SampleModel"});
    addView(h, "Lims_WeighSamples",
            R"({"v-kind":"collection","v-query":"Lims_WeighSamples","v-rowKey":"id","v-columns":[)"
            R"({"field":"id","label":"ID","kind":"integer"},)"
            R"({"field":"mass","label":"Mass","kind":"quantity","x-decimalPlaces":3,)"
            R"("ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},)"
            R"("x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}]}]})");
    auto screen = h.mount(withTableKey(kActionTableNoColumns, R"("action":"Lims_WeighSamples")"));
    h.reply(h.dispatcher.last("Lims_WeighSamples"),
            R"({"rows":[{"id":1,"mass":{"num":5,"den":2,"dp":1,"unit":"kg"}},)"
            R"({"id":2,"mass":{"num":1500,"den":1,"dp":0,"unit":"g"}}],"total":2})");
    CHECK(kindOfColumn(*screen, "mass") == table::ColumnKind::Measure);
    CHECK(unitOf(*screen, "g").dimension == unitOf(*screen, "kg").dimension);
    CHECK(unitOf(*screen, "g").toBase ==
          morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000}, morph::math::DecimalPlaces{3}});
    for (std::int64_t row : {0, 1}) {
        auto const* const measure = std::get_if<table::Measure>(cellOf(*screen, row, "mass"));
        REQUIRE(measure != nullptr);
        CHECK(measure->unit != table::kUnknownUnit);
    }
}

// Mutation: a unit no description lists left unknown (the cell's unit is kUnknownUnit, an invalid cell).
TEST_CASE("model table over an action: a unit no one lists is its own canonical unit", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kActionTable);
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a","weight":{"num":5,"den":1,"dp":0,"unit":"furlong"}}],"total":1})");
    CHECK(kindOfColumn(*screen, "weight") == table::ColumnKind::Measure);
    auto const* const measure = std::get_if<table::Measure>(cellOf(*screen, 0, "weight"));
    REQUIRE(measure != nullptr);
    CHECK(measure->unit != table::kUnknownUnit);
    CHECK(unitOf(*screen, "furlong").dimension == "furlong");
}

// Mutations: the locate read sent to `Table.` and the table's id, as a model's table's (no call of the action); a reply
// that holds the row without `indices` read as the key's absence (an answer of nothing, not an error).
TEST_CASE("model table over an action: a row is located through the action, and a row without its index is unknown",
          "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kActionTable);
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":1,"name":"a"},{"id":2,"name":"b"}],"total":300})");
    std::vector<morph::ui::RowLocation> answers;
    auto const locate = [&] {
        std::size_t const sent = h.dispatcher.count("Lims_PageSamples");
        windowOf(*screen).locateRow(table::RowId{std::int64_t{2}}, {},
                                    [&answers](morph::ui::RowLocation const& found) { answers.push_back(found); });
        h.settle();
        REQUIRE(h.dispatcher.count("Lims_PageSamples") == sent + 1);
        INFO(h.dispatcher.calls.back().body);
        CHECK(lastBodyHas(h, "Lims_PageSamples", R"("keys":[2])"));            // the key in the x-table member
        CHECK(lastBodyHas(h, "Lims_PageSamples", R"("columns":[])"));
    };
    locate();
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":2,"name":"b"}],"indices":[41],"total":300})");
    REQUIRE(answers.size() == 1);
    REQUIRE(answers.back().has_value());
    CHECK(*answers.back() == std::optional<std::int64_t>{41});
    locate();
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":2,"name":"b"}],"total":300})");
    REQUIRE(answers.size() == 2);
    CHECK_FALSE(answers.back().has_value());                                  // where the row is is not known
    locate();
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[],"total":300})");
    REQUIRE(answers.size() == 3);
    REQUIRE(answers.back().has_value());
    CHECK_FALSE(answers.back()->has_value());                                 // no row: the view lacks the key
}

// Mutation: a typed list action's table taken as not described, its revision being empty (no keyed call is sent).
TEST_CASE("model table over an action: highlightKey locates its row through the action and marks it", "[ui][table]") {
    constexpr char const* kTypedHighlight =
        R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},)"
        R"("state":{"hk":{"init":{"lit":null}}},"view":{"kind":"column","children":[)"
        R"({"kind":"button","label":"2","testId":"to2","onClick":[{"set":"hk","value":2}]},)"
        R"({"kind":"table","id":"list","testId":"list","model":"lab","action":"Lims_PageSamples","key":{"row":"id"},)"
        R"("highlightKey":{"ref":"hk"},"columns":[{"id":"id"},{"id":"name"}]}]}})";
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kTypedHighlight);
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":1,"name":"a"},{"id":2,"name":"b"}],"total":300})");
    std::size_t const sent = h.dispatcher.count("Lims_PageSamples");
    click(h, "to2");
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == sent + 1);
    CHECK(lastBodyHas(h, "Lims_PageSamples", R"("keys":[2])"));
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":2,"name":"b"}],"indices":[41],"total":300})");
    CHECK(h.backend.prop(tableOf(h), "highlightedRow") == "41");
    CHECK(h.diagnostics.empty());
}


namespace {

/// `kActionTable` with the literal filter `ratio > 1`.
std::string literalRatioFilter() {
    return withTableKey(kActionTable, R"("filters":{"columns":{"ratio":{"include":[{"gt":"1"}]}}})");
}

/// The first reply of `kActionTable`: `ratio` a Number column, `name` a Text one.
constexpr std::string_view kRatioRows =
    R"({"rows":[{"id":1,"name":"a","ratio":0.5},{"id":2,"name":"b","ratio":2}],"total":2})";

/// A filter of one column's one entry.
table::FilterSpec filterOf(std::string const& column, table::FilterEntry entry) {
    table::FilterSpec filter;
    filter.columns[column].include.push_back(std::move(entry));
    return filter;
}

}  // namespace

// Mutations: the literal filter compiled against the placeholder kinds (refused unseen: no second query); the bound
// one likewise (refused and reported: no second query).
TEST_CASE("model table over an action: a literal or bound filter on an inferred column waits for its kind",
          "[ui][table]") {
    SECTION("literal") {
        TableHarness h;  addPageSamples(h);
        auto screen = h.mount(literalRatioFilter());
        REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);
        CHECK_FALSE(lastBodyHas(h, "Lims_PageSamples", R"("gt")"));
        h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
        REQUIRE(h.dispatcher.count("Lims_PageSamples") == 2);
        CHECK(lastBodyHas(h, "Lims_PageSamples", R"({"gt":"1"})"));
        CHECK(h.diagnostics.empty());
    }
    SECTION("bound") {
        TableHarness h;  addPageSamples(h);
        std::string text = withTableKey(kActionTable, R"("filters":{"ref":"f"})");
        text.replace(text.find(R"("view":)"), 7,
                     R"("state":{"f":{"init":{"lit":{"columns":{"ratio":{"include":[{"gt":"1"}]}}}}}},"view":)");
        auto screen = h.mount(text);
        REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);
        CHECK_FALSE(lastBodyHas(h, "Lims_PageSamples", R"("gt")"));
        h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
        REQUIRE(h.dispatcher.count("Lims_PageSamples") == 2);
        CHECK(lastBodyHas(h, "Lims_PageSamples", R"({"gt":"1"})"));
        CHECK(h.diagnostics.empty());
    }
}

// Mutation: the held literal filter applied over the user's filter (a third query, filtering `ratio`: 3 == 2).
TEST_CASE("model table over an action: the user's filter before the first reply survives it", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(literalRatioFilter());
    REQUIRE(elementOf(*screen, "list").tableRows->setFilter(filterOf("name", table::FilterEntry{.contains = "a"})));
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 2);                     // the user's filter is a new query
    h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
    CHECK(h.dispatcher.count("Lims_PageSamples") == 2);
    CHECK(lastBodyHas(h, "Lims_PageSamples", R"({"contains":"a"})"));
    CHECK(windowOf(*screen).filter() == filterOf("name", table::FilterEntry{.contains = "a"}));
}

// Mutation: the user's filter not fitted again once the kinds are fixed (no report, and `contains` stays on the
// Number column: 2 == 3).
TEST_CASE("model table over an action: a user's filter the inferred kind refuses is reported and dropped",
          "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kActionTable);
    REQUIRE(elementOf(*screen, "list").tableRows->setFilter(filterOf("ratio", table::FilterEntry{.contains = "5"})));
    h.settle();
    h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
    CHECK(diagnosticsMentioning(h, "the filter on `ratio` is not applied: it does not apply to the column's kind") == 1);
    CHECK(h.dispatcher.count("Lims_PageSamples") == 3);
    CHECK(windowOf(*screen).filter() == table::FilterSpec{});
}

// Mutations: each column's units in a dimension of its own (mg is not g's: "kg" == "g"); a factor of zero listed
// (no report).
TEST_CASE("model table over an action: columns that share a unit share its dimension in any order",
          "[ui][table]") {
    TableHarness h;
    addAction(h, "Lims_WeighSamples", std::string{kQuerySchema}, ActionInfo{.modelType = "Lims_SampleModel"});
    addView(h, "Lims_WeighSamples",
            R"({"v-kind":"collection","v-query":"Lims_WeighSamples","v-rowKey":"id","v-columns":[)"
            R"({"field":"id","label":"ID","kind":"integer"},)"
            R"({"field":"small","kind":"quantity","ExtUnits":{"unitAscii":"g"},)"
            R"("x-unitAlternatives":[{"id":"kg","num":1000,"den":1},{"id":"bad","num":0,"den":1}]},)"
            R"({"field":"large","kind":"quantity","ExtUnits":{"unitAscii":"kg"},)"
            R"("x-unitAlternatives":[{"id":"mg","num":1,"den":1000000}]}]})");
    auto screen = h.mount(withTableKey(kActionTableNoColumns, R"("action":"Lims_WeighSamples")"));
    CHECK(unitOf(*screen, "mg").dimension == unitOf(*screen, "g").dimension);
    CHECK(unitOf(*screen, "mg").toBase ==
          morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000}, morph::math::DecimalPlaces{3}});
    CHECK(diagnosticsMentioning(h, "the unit `bad` has a factor of zero or less") == 1);
}


// Mutation: a held stored filter giving way to any gesture (the sort and the scroll drop it: no `gt` in the last
// query).
TEST_CASE("model table over an action: a sort and a scroll before the first reply keep the stored filter",
          "[ui][table][state]") {
    auto store = std::make_shared<GatedTableStateStore>(
        storeExecutor(), R"({"version":1,"filters":{"columns":{"ratio":{"include":[{"gt":"1"}]}}}})");
    TableHarness h{store};  addPageSamples(h);
    auto screen = h.mount(kActionTable);  store->release();  h.settle();
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);
    h.backend.clickHeader(tableOf(h), 1);  h.settle();                        // a sort
    h.backend.scrollTable(tableOf(h), 0, 5);  h.advance(80ms);                 // a scroll
    h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
    CHECK(lastBodyHas(h, "Lims_PageSamples", R"({"gt":"1"})"));
    CHECK(lastBodyHas(h, "Lims_PageSamples", R"("sort":[{"column":"name")"));
    CHECK(h.diagnostics.empty());
}

// Mutation: the kinds a first reply that does not read voted kept (ratio is Number before any reply read).
TEST_CASE("model table over an action: a first reply that does not read fixes no kind", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kActionTable);
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"name":"a","ratio":0.5}],"total":1})");   // no key
    CHECK(kindOfColumn(*screen, "ratio") == table::ColumnKind::Text);
    CHECK(windowOf(*screen).inferring());
    windowOf(*screen).retry();  h.settle();
    h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
    CHECK(kindOfColumn(*screen, "ratio") == table::ColumnKind::Number);
    CHECK_FALSE(windowOf(*screen).inferring());
}

// Mutation: no report for a `key` that is not one row field.
TEST_CASE("model table over an action: a key that is not one row field is reported", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(withTableKey(kActionTable, R"("key":{"lit":"id"})"));
    CHECK(diagnosticsMentioning(h, "the key of a table over an action is one row field") == 1);
}

// Mutation: an integer beyond int64 read as its JSON text (a string cell).
TEST_CASE("model table over an action: an integer beyond int64 is a double", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kActionTable);
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a","ratio":18446744073709551615}],"total":1})");
    CHECK(kindOfColumn(*screen, "ratio") == table::ColumnKind::Number);
    CHECK(*cellOf(*screen, 0, "ratio") == table::Cell{18446744073709551615.0});
}

// Mutation: no bound on the units a window lists (the 65th unit is listed and its cell is valid).
TEST_CASE("model table over an action: a window lists at most 64 units no one declares", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kActionTable);
    std::string rows;
    for (int at = 0; at < 65; ++at) {
        rows += std::string{at == 0 ? "" : ","} + R"({"id":)" + std::to_string(at) +
                R"(,"weight":{"num":1,"den":1,"dp":0,"unit":"u)" + std::to_string(at) + R"("}})";
    }
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[)" + rows + R"(],"total":65})");
    REQUIRE(kindOfColumn(*screen, "weight") == table::ColumnKind::Measure);
    CHECK(windowOf(*screen).description()->units.size() == morph::ui::detail::kMaxUnlistedUnits);
    CHECK(std::get<table::Measure>(*cellOf(*screen, 63, "weight")).unit != table::kUnknownUnit);
    CHECK(std::get<table::Measure>(*cellOf(*screen, 64, "weight")).unit == table::kUnknownUnit);
}

// Mutation: the inference handler not guarded by the table's scope (it reaches the unmounted table's state).
TEST_CASE("model table over an action: a window that outlives its table reads its reply alone", "[ui][table]") {
    TableHarness h;  addPageSamples(h);
    auto screen = h.mount(kActionTable);
    std::shared_ptr<WindowRows> const window = elementOf(*screen, "list").tableWindow;
    std::size_t const pending = h.dispatcher.last("Lims_PageSamples");
    screen.reset();  h.settle();
    h.reply(pending, std::string{kRatioRows});
    CHECK_FALSE(window->inferring());                                         // the reply was read
    CHECK(h.diagnostics.empty());
}

// ── Configuring a table ─────────────────────────────────────────────────────

namespace {

using morph::ui::HeaderClick;
using morph::ui::MenuEntry;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::entryOf;
using morph::ui::testing::kConfigModelTable;
using morph::ui::testing::kNine;
using morph::ui::testing::labelsOf;
using morph::ui::testing::menuOf;
using morph::ui::testing::moveColumn;
using morph::ui::testing::resizeColumn;

// A document's text with a top-level `state` entry @p name initialised to @p init (JSON).
std::string withState(std::string_view text, std::string_view name, std::string_view init) {
    auto json = morph::ui::parseJson(text);
    REQUIRE(json.has_value());
    auto initJson = morph::ui::parseJson(R"({"init":{"lit":)" + std::string{init} + "}}");
    REQUIRE(initJson.has_value());
    auto& root = std::get<morph::ui::Json::object_t>(json->data);
    if (!root.contains("state")) {
        root.insert_or_assign("state", morph::ui::Json{morph::ui::Json::object_t{}});
    }
    std::get<morph::ui::Json::object_t>(root.at("state").data).insert_or_assign(std::string{name}, *initJson);
    return glz::write_json(*json).value_or("");
}

// A document's text whose table's first column part takes the member @p member, written `"key":value`.
std::string withFirstColumnKey(std::string_view text, std::string_view member) {
    auto json = morph::ui::parseJson(text);
    REQUIRE(json.has_value());
    auto field = morph::ui::parseJson("{" + std::string{member} + "}");
    REQUIRE(field.has_value());
    auto& view = std::get<morph::ui::Json::object_t>(std::get<morph::ui::Json::object_t>(json->data).at("view").data);
    auto& first = std::get<morph::ui::Json::object_t>(
        std::get<morph::ui::Json::array_t>(view.at("columns").data).front().data);
    for (auto& [key, value] : std::get<morph::ui::Json::object_t>(field->data)) {
        first.insert_or_assign(key, value);
    }
    return glz::write_json(*json).value_or("");
}

// A rows table `t` of @p count sortable columns c1… over one row, with the table node's members @p extra.
std::string sortableRows(int count, std::string_view extra) {
    std::string row = R"({"id":1)";
    std::string columns;
    for (int at = 1; at <= count; ++at) {
        row += ",\"c" + std::to_string(at) + "\":" + std::to_string(at);
        columns += std::string{at == 1 ? "" : ","} + R"({"id":"c)" + std::to_string(at) + R"(","sortable":true})";
    }
    row += "}";
    return R"({"vocab":"ui/1","id":"many","state":{"items":{"init":{"lit":[)" + row +
           R"(]}}},"view":{"kind":"table","id":"t","rows":{"ref":"items"},"columns":[)" + columns + "]," +
           std::string{extra} + "}}";
}

// The commands that count a table event's runs in `n` and keep its last `event` in `last`.
constexpr std::string_view kCount =
    R"([{"set":"n","value":{"fn":"add","args":[{"ref":"n"},1]}},{"set":"last","value":{"ref":"event"}}])";

// kNine with a counter `n` and `last`, and @p event running `kCount`.
std::string countingNine(std::string_view event, std::string_view text = kNine) {
    return withTableKey(withState(withState(text, "n", "0"), "last", "null"),
                        "\"" + std::string{event} + "\":" + std::string{kCount});
}

}  // namespace

// Mutation: the lowering mapping HeaderClick::Shift to Click.
TEST_CASE("sort chains: a plain click replaces, Shift adds and flips, a key past the bound is refused", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    h.backend.clickHeader(table, 0, HeaderClick::Plain);
    h.backend.clickHeader(table, 1, HeaderClick::Shift);
    h.backend.clickHeader(table, 0, HeaderClick::Shift);
    h.settle();
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c1","direction":"desc"},{"column":"c2","direction":"asc"}])");
    CHECK(h.backend.prop(table, "sort") == "0:Descending,1:Ascending");
    for (std::size_t column = 2; column < 9; ++column) {
        h.backend.clickHeader(table, column, HeaderClick::Shift);
    }
    h.settle();
    CHECK(h.backend.prop(table, "sort") ==
          "0:Descending,1:Ascending,2:Ascending,3:Ascending,4:Ascending,5:Ascending,6:Ascending,7:Ascending");
    CHECK(statusOf(h, table).message == "A table sorts by at most 8 columns");
    CHECK(statusOf(h, table).shown);
    h.backend.clickHeader(table, 4, HeaderClick::Plain);
    h.settle();
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c5","direction":"asc"}])");
    CHECK(statusOf(h, table).message.empty());
    CHECK_FALSE(statusOf(h, table).shown);
}

// Mutation: the client holding 8 whatever the description says.
TEST_CASE("sort chains: a table whose limits set 32 accepts 32 keys from the header and refuses the 33rd", "[ui][table][config]") {
    SECTION("a model's table, whose description carries the bound") {
        morph::ui::testing::TableHarness h;
        auto screen = h.mount(kConfigModelTable);
        table::TableDescription described = samplesDescription(40);
        described.capabilities.maxSortKeys = 32;
        replyDescribe(h, table::describe(described));
        int const table = h.backend.all("Table").front();
        for (std::size_t column = 0; column < 33; ++column) {
            h.backend.clickHeader(table, column, HeaderClick::Shift);
        }
        h.settle();
        Value const sort = screen->read(*morph::ui::parseJson(R"({"ref":"t.sort"})"));
        REQUIRE(sort.asList() != nullptr);
        CHECK(sort.asList()->size() == 32);
        CHECK(lastQuery(h).sort.size() == 32);
        CHECK(statusOf(h, table).message == "A table sorts by at most 32 columns");
    }
    SECTION("a rows table, whose node carries it") {
        morph::ui::testing::TableHarness h;
        auto screen = h.mount(sortableRows(13, R"("maxSortKeys":12)"));
        int const table = h.backend.all("Table").front();
        for (std::size_t column = 0; column < 13; ++column) {
            h.backend.clickHeader(table, column, HeaderClick::Shift);
        }
        h.settle();
        Value const sort = screen->read(*morph::ui::parseJson(R"({"ref":"t.sort"})"));
        REQUIRE(sort.asList() != nullptr);
        CHECK(sort.asList()->size() == 12);
        CHECK(statusOf(h, table).message == "A table sorts by at most 12 columns");
    }
}

// Mutation: "Add to sort" mapped to SortGesture::Click.
TEST_CASE("sort chains: the header menu builds a chain without a modifier", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(countingNine("onSortChange"));
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, 0, {"Sort descending"});
    chooseMenu(h, table, 2, {"Add to sort"});
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c1","direction":"desc"},{"column":"c3","direction":"asc"}])");
    chooseMenu(h, table, 0, {"Remove from sort"});
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c3","direction":"asc"}])");
    CHECK(readJson(*screen, R"({"ref":"n"})") == "3");
    CHECK(readJson(*screen, R"({"ref":"last"})") == R"([{"column":"c3","direction":"asc"}])");
}

// Mutations: the lowering applying a menu gesture without updating `visibleColumns`; a driver's column read as a
// visible place (the resize after the move lands on c1).
TEST_CASE("column layout: hiding, moving, pinning and resetting from the menus change the layout and the visible columns",
          "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(countingNine("onLayoutChange"));
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, 2, {"Hide column"});
    chooseMenu(h, table, 1, {"Move left"});
    chooseMenu(h, table, 1, {"Pin column"});
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c2","c1","c4","c5","c6","c7","c8","c9"])");
    CHECK(elementValue(*screen, "t", "hiddenCount") == "1");
    CHECK(elementValue(*screen, "t", "layout") ==
          R"({"hidden":["c3"],"order":["c2","c1","c3","c4","c5","c6","c7","c8","c9"],"pinned":1,"widths":{}})");
    CHECK_THROWS(h.backend.menuOf(table, 2));                                  // the mount draws no header for c3
    resizeColumn(h, table, 1, 20);                                             // index 1 is c2 wherever it stands
    CHECK(elementValue(*screen, "t", "layout") ==
          R"({"hidden":["c3"],"order":["c2","c1","c3","c4","c5","c6","c7","c8","c9"],"pinned":1,"widths":{"c2":20}})");
    resizeColumn(h, table, 1, std::nullopt);
    CHECK(elementValue(*screen, "t", "layout") ==
          R"({"hidden":["c3"],"order":["c2","c1","c3","c4","c5","c6","c7","c8","c9"],"pinned":1,"widths":{}})");
    chooseMenu(h, table, std::nullopt, {"Reset columns"});
    CHECK(elementValue(*screen, "t", "layout") ==
          R"({"hidden":[],"order":["c1","c2","c3","c4","c5","c6","c7","c8","c9"],"pinned":0,"widths":{}})");
    CHECK(readJson(*screen, R"({"ref":"n"})") == "6");
    CHECK(readJson(*screen, R"({"ref":"last"})") == elementValue(*screen, "t", "layout"));
}

// Mutation: `configurable` ignored for menus (the hide entry is present).
TEST_CASE("column layout: configurable and locked limit what the user can do, and each refusal is shown", "[ui][table][config]") {
    SECTION("hide only, the first column locked") {
        morph::ui::testing::TableHarness h;
        auto screen = h.mount(withFirstColumnKey(withTableKey(kNine, R"("configurable":{"lit":["hide"]})"), R"("locked":true)"));
        int const table = h.backend.all("Table").front();
        std::vector<std::string> const labels = labelsOf(menuOf(h, table, 1));
        CHECK(std::ranges::find(labels, "Move left") == labels.end());
        CHECK(std::ranges::find(labels, "Hide column") != labels.end());
        std::string const before = elementValue(*screen, "t", "visibleColumns");
        moveColumn(h, table, 1, 0);
        CHECK(elementValue(*screen, "t", "visibleColumns") == before);
        CHECK(statusOf(h, table).message == "This table's columns cannot be moved");
        CHECK_FALSE(entryOf(menuOf(h, table, 0), "Hide column").enabled);
        moveColumn(h, table, 0, 1);
        CHECK(statusOf(h, table).message == "C1 is locked");
        CHECK(elementValue(*screen, "t", "visibleColumns") == before);
    }
    SECTION("nothing, and no menu") {
        morph::ui::testing::TableHarness h;
        auto screen = h.mount(withTableKey(kNine, R"("configurable":false)"));
        int const table = h.backend.all("Table").front();
        CHECK(menuOf(h, table, 1).empty());
        CHECK(menuOf(h, table, std::nullopt).empty());
    }
}

// Mutation: a header click lowered without `tableMessage(table, "")` (the message outlives the gesture).
TEST_CASE("status line: a refusal is shown once, until the next gesture or six seconds", "[ui][table][config]") {
    auto const refuse = [](morph::ui::testing::TableHarness& h, int table) {
        for (std::size_t column = 0; column < 9; ++column) {
            h.backend.clickHeader(table, column, HeaderClick::Shift);
        }
        h.settle();
    };
    SECTION("in the status line") {
        morph::ui::testing::TableHarness h;
        auto screen = h.mount(kNine);
        int const table = h.backend.all("Table").front();
        refuse(h, table);
        CHECK(statusOf(h, table).message == "A table sorts by at most 8 columns");
        h.advance(5'999ms);
        CHECK(statusOf(h, table).message == "A table sorts by at most 8 columns");
        CHECK(statusOf(h, table).shown);
        h.advance(1ms);
        CHECK(statusOf(h, table).message.empty());
        CHECK_FALSE(statusOf(h, table).shown);                               // a rows table with nothing to say
        h.backend.clickHeader(table, 8, HeaderClick::Shift);
        h.settle();
        REQUIRE(statusOf(h, table).message == "A table sorts by at most 8 columns");
        h.backend.clickHeader(table, 0, HeaderClick::Plain);
        h.settle();
        CHECK(statusOf(h, table).message.empty());
    }
    SECTION("with `status: false`, as a notification") {
        morph::ui::testing::TableHarness h;
        auto screen = h.mount(withTableKey(kNine, R"("status":false)"));
        int const table = h.backend.all("Table").front();
        refuse(h, table);
        REQUIRE(h.host.notified.size() == 1);
        CHECK(h.host.notified.front() ==
              std::pair<std::string, std::string>{"A table sorts by at most 8 columns", "warning"});
        CHECK_FALSE(statusOf(h, table).shown);
    }
}

// Mutation: the lowering applying a stored layout after the first query.
TEST_CASE("persisted layout: a stored layout applies before the first query, a change saves once after the debounce",
          "[ui][table][config]") {
    constexpr std::string_view kLayout =
        R"({"version":1,"layout":{"order":["c2","c1"],"hidden":["c3"],"pinned":0,"widths":{"c2":20}},)"
        R"("sort":[{"column":"c4","direction":"desc"}]})";
    auto store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kLayout});
    morph::ui::testing::TableHarness h{store};
    SECTION("the table's own layout") {
        auto screen = h.mount(kNine);
        int const table = h.backend.all("Table").front();
        CHECK(morph::ui::testing::rowOrder(h).empty());                         // the rows wait for the stored state
        store->release();
        h.settle();
        // c3 to c9 are placed after their nearest declared predecessor the layout places, c2; c3 stays hidden.
        CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c2","c4","c5","c6","c7","c8","c9","c1"])");
        CHECK(elementValue(*screen, "t", "layout").find(R"("widths":{"c2":20})") != std::string::npos);
        CHECK_FALSE(morph::ui::testing::rowOrder(h).empty());
        chooseMenu(h, table, 0, {"Hide column"});
        h.advance(100ms);
        chooseMenu(h, table, 3, {"Hide column"});
        h.advance(100ms);
        chooseMenu(h, table, 4, {"Hide column"});
        CHECK(store->saves() == 0);
        h.advance(500ms);
        REQUIRE(store->saves() == 1);
        auto const saved = table::readState(store->lastSaved());
        REQUIRE(saved.has_value());
        REQUIRE(saved->layout.has_value());
        std::vector<std::string> hiddenColumns = saved->layout->hidden;
        std::ranges::sort(hiddenColumns);
        CHECK(hiddenColumns == std::vector<std::string>{"c1", "c3", "c4", "c5"});
    }
    SECTION("a model's table: the first query names only the columns the stored layout shows") {
        auto modelStore = std::make_shared<GatedTableStateStore>(
            storeExecutor(), R"({"version":1,"layout":{"order":["name","id","c3","c4","c5"],"hidden":["c3"],"pinned":0,"widths":{}}})");
        morph::ui::testing::TableHarness m{modelStore};
        auto screen = m.mount(kConfigModelTable);
        replyDescribe(m, samplesDescription(5));
        REQUIRE(m.dispatcher.count("Table.samples") == 0);                    // the first query waits for the state
        modelStore->release();
        m.settle();
        REQUIRE(m.dispatcher.count("Table.samples") >= 1);
        std::size_t firstCall = 0;
        while (m.dispatcher.calls.at(firstCall).action != "Table.samples") {
            ++firstCall;
        }
        table::TableQuery const first = queryOf(m, firstCall);
        REQUIRE(first.columns.has_value());
        CHECK_FALSE(std::ranges::contains(*first.columns, "c3"));
        CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["name","id","c4","c5"])");
    }
    SECTION("a bound layout") {
        auto screen = h.mount(withTableKey(withState(kNine, "lay", "null"), R"("layout":{"ref":"lay"})"));
        int const table = h.backend.all("Table").front();
        store->release();
        h.settle();
        CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1","c2","c3","c4","c5","c6","c7","c8","c9"])");
        CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c4","direction":"desc"}])");
        chooseMenu(h, table, 0, {"Hide column"});
        h.advance(500ms);
        CHECK(store->saves() == 0);                                            // the layout is the document's
        h.backend.clickHeader(table, 1, HeaderClick::Plain);
        h.advance(500ms);
        REQUIRE(store->saves() == 1);
        auto const saved = table::readState(store->lastSaved());
        REQUIRE(saved.has_value());
        CHECK(saved->sort == table::SortChain{{.column = "c2", .dir = table::SortDirection::Ascending}});
        INFO(store->lastSaved());
        REQUIRE(saved->layout.has_value());                                    // the stored one, as it was kept
        CHECK(saved->layout->order == std::vector<std::string>{"c2", "c1"});
        CHECK_FALSE(std::ranges::contains(saved->layout->hidden, "c1"));       // the user's hide is not saved
        CHECK(saved->layout->hidden == std::vector<std::string>{"c3"});        // a sort-only save keeps it whole
    }
}

// Review Focus 1. Mutation: hiding a column removing it from the query's sort.
TEST_CASE("hiding a sorted and filtered column changes no row", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(withTableKey(
        kConfigModelTable,
        R"("sort":{"lit":[{"column":"name","direction":"asc"}]},"filters":{"columns":{"name":{"include":[{"contains":"a"}]}}})"));
    replyDescribe(h, samplesDescription(5));
    table::TableQuery const first = lastQuery(h);
    REQUIRE(first.columns.has_value());
    replyPage(h, pageJson(0, 50, 50, *first.columns));
    int const table = h.backend.all("Table").front();
    std::string const rows = morph::ui::testing::rowOrder(h);
    REQUIRE_FALSE(rows.empty());
    std::size_t const calls = h.dispatcher.count("Table.samples");
    CHECK(h.backend.prop(table, "sort") == "1:Ascending");
    CHECK(h.backend.prop(table, "filtered") == "[1]");
    chooseMenu(h, table, 1, {"Hide column"});
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["id","c3","c4","c5"])");
    if (h.dispatcher.count("Table.samples") > calls) {                         // a column fetch of the rows held
        table::TableQuery const fetched = lastQuery(h);
        CHECK(fetched.sort == first.sort);
        CHECK(fetched.filters == first.filters);
        REQUIRE(fetched.columns.has_value());
        CHECK_FALSE(std::ranges::contains(*fetched.columns, "name"));
    }
    CHECK(morph::ui::testing::rowOrder(h) == rows);
    chooseMenu(h, table, std::nullopt, {"Reset columns"});
    CHECK(h.backend.prop(table, "sort") == "1:Ascending");
    CHECK(h.backend.prop(table, "filtered") == "[1]");
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["id","name","c3","c4","c5"])");
}

// Review Focus 2 (message half). Mutation: refusing Shift on a column already in the chain at the bound.
TEST_CASE("sort chains: at the bound a key in the chain still flips, with no message", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    for (std::size_t column = 0; column < 8; ++column) {
        h.backend.clickHeader(table, column, HeaderClick::Shift);
    }
    h.settle();
    REQUIRE(statusOf(h, table).message.empty());
    h.backend.clickHeader(table, 3, HeaderClick::Shift);
    h.settle();
    Value const sort = screen->read(*morph::ui::parseJson(R"({"ref":"t.sort"})"));
    REQUIRE(sort.asList() != nullptr);
    REQUIRE(sort.asList()->size() == 8);
    CHECK(*sort.asList()->at(3).field("direction") == Value{std::string{"desc"}});
    CHECK(statusOf(h, table).message.empty());
}

// ── The column chooser and the sort dialog ──────────────────────────────────

namespace {

// The column chooser's show boxes, in the order they were listed when it opened.
std::vector<std::string> chooserColumns(morph::ui::testing::TableHarness const& h) {
    std::vector<std::string> out;
    for (int const box : h.backend.all("Checkbox")) {
        if (std::string const id = h.backend.prop(box, "testId"); id.starts_with("table_columns_show_")) {
            out.push_back(id.substr(std::string_view{"table_columns_show_"}.size()));
        }
    }
    return out;
}

// Presses a dialog's button by its test id, then settles.
void press(morph::ui::testing::TableHarness& h, std::string_view testId) {
    h.backend.click(h.byTestId("Button", testId));
    h.settle();
}

// Whether a widget, found by its kind and test id, is enabled.
bool enabled(morph::ui::testing::TableHarness const& h, std::string_view kind, std::string_view testId) {
    return h.backend.prop(h.byTestId(kind, testId), "enabled") != "false";
}

// A rows table `t` of the Text columns a, b and c and the Integer column n.
constexpr std::string_view kKinds = R"({
  "vocab": "ui/1", "id": "kinds",
  "state": { "items": { "init": { "lit": [ { "id": 1, "a": "x", "b": "y", "c": "z", "n": 1 } ] } } },
  "view": { "kind": "table", "id": "t", "rows": { "ref": "items" },
            "columns": [ { "id": "a", "label": "A" }, { "id": "b", "label": "B" }, { "id": "c", "label": "C" },
                         { "id": "n", "label": "N", "kind": "integer" } ] } })";

}  // namespace

// Mutation: the chooser listing only the visible columns.
TEST_CASE("column chooser: every column in order, with show, move and reset", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, 2, {"Hide column"});
    REQUIRE(elementValue(*screen, "t", "hiddenCount") == "1");
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_columns"), "open") == "true");
    CHECK(chooserColumns(h) == std::vector<std::string>{"c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9"});
    int const c3 = h.byTestId("Checkbox", "table_columns_show_c3");
    CHECK(h.backend.prop(c3, "checked") == "false");
    CHECK(h.backend.prop(h.byTestId("Checkbox", "table_columns_show_c1"), "checked") == "true");
    h.backend.toggle(c3);
    h.settle();
    CHECK(elementValue(*screen, "t", "hiddenCount") == "0");
    CHECK(h.backend.prop(c3, "checked") == "true");
    CHECK_FALSE(enabled(h, "Button", "table_columns_up_c1"));
    press(h, "table_columns_down_c1");
    CHECK(elementValue(*screen, "t", "visibleColumns").starts_with(R"(["c2","c1",)"));
    press(h, "table_columns_reset");
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1","c2","c3","c4","c5","c6","c7","c8","c9"])");
    press(h, "table_columns_done");
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_columns"), "open") == "false");
}

// Mutations: group creation accepting columns of two kinds (n stays enabled once a is ticked); the Remove group
// button closing the line without applying RemoveGroup (the layout keeps g1).
TEST_CASE("column chooser: Group columns forms a group of one kind, which the layout keeps", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(countingNine("onLayoutChange", kKinds));
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    press(h, "table_columns_group");
    h.backend.toggle(h.byTestId("Checkbox", "table_group_column_a"));
    h.settle();
    CHECK_FALSE(enabled(h, "Checkbox", "table_group_column_n"));
    CHECK(enabled(h, "Checkbox", "table_group_column_b"));
    CHECK_FALSE(enabled(h, "Button", "table_group_create"));
    h.backend.edit(h.byTestId("TextInput", "table_group_label"), "Pair");
    h.settle();
    CHECK_FALSE(enabled(h, "Button", "table_group_create"));                 // a label and one column
    h.backend.toggle(h.byTestId("Checkbox", "table_group_column_b"));
    h.settle();
    REQUIRE(enabled(h, "Button", "table_group_create"));
    press(h, "table_group_create");
    CHECK(readJson(*screen, R"({"ref":"t.layout.groups"})") == R"([{"columns":["a","b"],"id":"g1","label":"Pair"}])");
    CHECK(std::ranges::contains(labelsOf(menuOf(h, table, 0)), "Filter Pair, 2 columns…"));
    CHECK(h.text("table_group_g1") == "Pair, 2 columns");
    CHECK(readJson(*screen, R"({"ref":"n"})") == "1");
    // A column already in a group is refused in the status line, and the form stays as the user left it.
    press(h, "table_columns_group");
    h.backend.toggle(h.byTestId("Checkbox", "table_group_column_a"));
    h.backend.toggle(h.byTestId("Checkbox", "table_group_column_c"));
    h.backend.edit(h.byTestId("TextInput", "table_group_label"), "More");
    h.settle();
    press(h, "table_group_create");
    CHECK(statusOf(h, table).message == "A is already in Pair");
    CHECK(h.backend.prop(h.byTestId("Checkbox", "table_group_column_c"), "checked") == "true");
    CHECK(readJson(*screen, R"({"ref":"n"})") == "1");
    press(h, "table_group_remove_g1");
    CHECK(readJson(*screen, R"({"ref":"t.layout.groups"})") == "null");
    CHECK_FALSE(std::ranges::contains(labelsOf(menuOf(h, table, 0)), "Filter Pair, 2 columns…"));
    CHECK(readJson(*screen, R"({"ref":"n"})") == "2");
}

// Mutation: the sort dialog's move applying to the wrong neighbour.
TEST_CASE("sort dialog: keys in order with direction, move and remove; Add key refuses at the bound",
          "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    h.backend.clickHeader(table, 0, HeaderClick::Plain);
    h.backend.clickHeader(table, 1, HeaderClick::Shift);
    h.backend.clickHeader(table, 1, HeaderClick::Shift);
    h.settle();
    REQUIRE(elementValue(*screen, "t", "sort") ==
            R"([{"column":"c1","direction":"asc"},{"column":"c2","direction":"desc"}])");
    chooseMenu(h, table, std::nullopt, {"Sort…"});
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_sort"), "open") == "true");
    press(h, "table_sort_up_1");
    CHECK(elementValue(*screen, "t", "sort") ==
          R"([{"column":"c2","direction":"desc"},{"column":"c1","direction":"asc"}])");
    h.backend.choose(h.byTestId("Select", "table_sort_direction_0"), morph::ui::Key{std::string{"ascending"}});
    h.settle();
    CHECK(elementValue(*screen, "t", "sort") ==
          R"([{"column":"c2","direction":"asc"},{"column":"c1","direction":"asc"}])");
    press(h, "table_sort_remove_1");
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c2","direction":"asc"}])");
    for (int key = 1; key < 8; ++key) {
        press(h, "table_sort_add");
    }
    Value sort = screen->read(*morph::ui::parseJson(R"({"ref":"t.sort"})"));
    REQUIRE(sort.asList() != nullptr);
    REQUIRE(sort.asList()->size() == 8);
    CHECK(statusOf(h, table).message.empty());
    press(h, "table_sort_add");
    sort = screen->read(*morph::ui::parseJson(R"({"ref":"t.sort"})"));
    REQUIRE(sort.asList() != nullptr);
    CHECK(sort.asList()->size() == 8);
    CHECK(statusOf(h, table).message == "A table sorts by at most 8 columns");
    press(h, "table_sort_done");
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_sort"), "open") == "false");
}

// Mutations: the move buttons read without the pin boundary (c3, the first unpinned column, can move up); the show
// box of the only visible column enabled; the chooser's dismissal leaving it open; a key's column select offering a
// column another key holds.
TEST_CASE("column chooser and sort dialog: locks, the pin boundary, a key's column and a dismissal", "[ui][table][config]") {
    {
        morph::ui::testing::TableHarness h;
        auto screen = h.mount(withFirstColumnKey(kNine, R"("locked":true)"));
        int const table = h.backend.all("Table").front();
        chooseMenu(h, table, 1, {"Pin column"});
        moveColumn(h, table, 2, 1);
        REQUIRE(elementValue(*screen, "t", "layout").find(R"("pinned":1)") != std::string::npos);
        REQUIRE(elementValue(*screen, "t", "visibleColumns").starts_with(R"(["c2","c3","c1",)"));
        chooseMenu(h, table, std::nullopt, {"Columns…"});
        CHECK_FALSE(enabled(h, "Checkbox", "table_columns_show_c1"));        // locked
        CHECK_FALSE(enabled(h, "Button", "table_columns_down_c1"));
        CHECK_FALSE(enabled(h, "Button", "table_columns_down_c2"));          // the last pinned column
        CHECK_FALSE(enabled(h, "Button", "table_columns_up_c3"));            // the first unpinned one
        CHECK(enabled(h, "Button", "table_columns_down_c3"));
        h.backend.dismiss(h.byTestId("Dialog", "table_columns"));
        h.settle();
        CHECK(h.backend.prop(h.byTestId("Dialog", "table_columns"), "open") == "false");
    }
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    for (std::string_view const column : {"c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9"}) {
        h.backend.toggle(h.byTestId("Checkbox", "table_columns_show_" + std::string{column}));
    }
    h.settle();
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1"])");
    CHECK_FALSE(enabled(h, "Checkbox", "table_columns_show_c1"));            // the only visible column
    CHECK(enabled(h, "Checkbox", "table_columns_show_c2"));
    press(h, "table_columns_done");

    h.backend.clickHeader(table, 0, HeaderClick::Plain);
    h.settle();
    REQUIRE(elementValue(*screen, "t", "sort") == R"([{"column":"c1","direction":"asc"}])");
    chooseMenu(h, table, std::nullopt, {"Sort…"});
    press(h, "table_sort_add");
    REQUIRE(elementValue(*screen, "t", "sort") ==
            R"([{"column":"c1","direction":"asc"},{"column":"c2","direction":"asc"}])");
    int const second = h.byTestId("Select", "table_sort_column_1");
    CHECK_THROWS(h.backend.choose(second, morph::ui::Key{std::string{"c1"}}));  // c1 is the first key's
    h.backend.choose(second, morph::ui::Key{std::string{"c5"}});
    h.settle();
    CHECK(elementValue(*screen, "t", "sort") ==
          R"([{"column":"c1","direction":"asc"},{"column":"c5","direction":"asc"}])");
    h.backend.dismiss(h.byTestId("Dialog", "table_sort"));
    h.settle();
    CHECK(h.backend.prop(h.byTestId("Dialog", "table_sort"), "open") == "false");
}

namespace {

// kNine's table in a row beside a text, the table's node taking the member @p member.
std::string nineInRow(std::string_view member) {
    auto json = morph::ui::parseJson(withTableKey(withTableKey(kNine, R"("testId":"t")"), member));
    REQUIRE(json.has_value());
    auto& root = std::get<morph::ui::Json::object_t>(json->data);
    std::string const table = glz::write_json(root.at("view")).value_or("");
    root.erase("view");
    std::string text = glz::write_json(*json).value_or("");
    text.pop_back();
    return text + R"(,"view":{"kind":"row","children":[)" + table +
           R"(,{"kind":"text","text":"beside","sizing":{"lit":{"stretch":1}}}]}})";
}

}  // namespace

// Mutation: the table inside the dialogs' column keeping its percent (the mount throws: a column's child takes no
// share).
TEST_CASE("table dialogs: a configurable table with a share of its row mounts, its column taking the share",
          "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(nineInRow(R"("sizing":{"lit":{"percent":40}})"));
    int const table = h.byTestId("Table", "t");
    CHECK(h.backend.prop(table, "layout") == "stretch(1)/stretch(1)");
    CHECK(h.backend.dump().contains("Column#2 gap=0 layout=percent(40)/content"));
    CHECK(h.diagnostics.empty());
}

// Mutation: the table inside the dialogs' column keeping a content height (the row's offer to fill its height reaches
// only the column, and the table stops at its own).
TEST_CASE("table dialogs: a content-height table in a row fills the column the row stretches", "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto screen = h.mount(nineInRow(R"("maxWidth":50)"));
    int const table = h.byTestId("Table", "t");
    CHECK(h.backend.prop(table, "layout") == "content max=50/stretch(1)");
    CHECK(h.backend.dump().contains("Column#2 gap=0 layout=content max=50/content"));
}

// ── The filter menu ─────────────────────────────────────────────────────────

namespace {

using morph::ui::testing::clickFilter;
using morph::ui::testing::clickGroupFilter;

// A rows table `t` of `status` (Open×3, Closed×2, Held×1 and one empty) and `site` (A×5, B×2), counting the runs of
// `onFiltersChange` in `n`.
constexpr std::string_view kStatusRows = R"({
  "vocab": "ui/1", "id": "status",
  "state": { "n": { "init": { "lit": 0 } }, "items": { "init": { "lit": [
    { "id": 1, "status": "Open", "site": "A" }, { "id": 2, "status": "Open", "site": "A" },
    { "id": 3, "status": "Open", "site": "B" }, { "id": 4, "status": "Closed", "site": "A" },
    { "id": 5, "status": "Closed", "site": "A" }, { "id": 6, "status": "Held", "site": "B" },
    { "id": 7, "site": "A" } ] } } },
  "view": { "kind": "table", "id": "t", "rows": { "ref": "items" },
            "columns": [ { "id": "status", "label": "Status" }, { "id": "site", "label": "Site" } ],
            "onFiltersChange": [ { "set": "n", "value": { "fn": "add", "args": [ { "ref": "n" }, 1 ] } } ] } })";

// Whether a widget of a kind with a test id is mounted.
bool mounted(TableHarness const& h, std::string_view kind, std::string_view testId) {
    return h.backend.find(kind, "testId", testId).has_value();
}

// A widget's property, found by its kind and test id.
std::string propOf(TableHarness const& h, std::string_view kind, std::string_view testId, std::string_view name) {
    return h.backend.prop(h.byTestId(kind, testId), name);
}

// Toggles a check box of the filter menu, then settles.
void toggleBox(TableHarness& h, std::string_view testId) {
    h.backend.toggle(h.byTestId("Checkbox", testId));
    h.settle();
}

// Ticks or unticks "(Select all)" as a user who wants every value under the search ticked, or unticked, does: a box
// that already shows the wanted state is toggled twice, since it shows only the values listed.
void selectAll(TableHarness& h, bool ticked) {
    toggleBox(h, "table_filter_all");
    if ((propOf(h, "Checkbox", "table_filter_all", "checked") == "true") != ticked) {
        toggleBox(h, "table_filter_all");
    }
}

// Chooses an option of one of the filter menu's selects by its label, then settles.
void chooseOption(TableHarness& h, std::string_view testId, std::string_view label) {
    h.backend.choose(h.byTestId("Select", testId), Key{std::string{label}});
    h.settle();
}

// Types into one of the filter menu's text inputs, then settles.
void typeInto(TableHarness& h, std::string_view testId, std::string_view text) {
    h.backend.edit(h.byTestId("TextInput", testId), std::string{text});
    h.settle();
}

// Closes the filter menu as a dismissal does.
void dismissMenu(TableHarness& h) {
    h.backend.dismiss(h.byTestId("Dialog", "table_filter"));
    h.settle();
}

// Presses one of the filter menu's buttons, then settles.
void pressButton(TableHarness& h, std::string_view testId) {
    h.backend.click(h.byTestId("Button", testId));
    h.settle();
}

// A select's `options` property for options whose keys are their labels.
std::string optionsOf(std::vector<std::string> const& labels) {
    std::vector<morph::ui::SelectOption> options;
    for (std::string const& label : labels) {
        options.push_back(morph::ui::SelectOption{.key = Key{label}, .label = label});
    }
    return morph::ui::testing::detail::formatList(options, [](morph::ui::SelectOption const& option) {
        return morph::ui::testing::detail::formatKey(option.key) + ":" + morph::ui::testing::detail::formatItem(option.label);
    });
}

// A distinct-values reply against @p revision: a page with no rows whose `distinct` reads @p values.
std::string distinctReply(std::string const& revision, std::string_view values) {
    table::GenericPage page;
    page.revision = revision;
    table::DistinctValues read;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(read, values));
    page.distinct = std::move(read);
    return glz::write_json(page).value_or("");
}

// The `Table.samples` calls that fetch a query's first page of rows: one per query the rows are given.
std::size_t pageCalls(TableHarness const& h) {
    std::size_t out = 0;
    for (std::size_t at = 0; at < h.dispatcher.calls.size(); ++at) {
        if (h.dispatcher.calls[at].action == "Table.samples" && !queryOf(h, at).distinct &&
            queryOf(h, at).page.offset == 0) {
            ++out;
        }
    }
    return out;
}

// The latest `Table.samples` call that asks for values.
std::size_t lastDistinct(TableHarness const& h) {
    for (std::size_t at = h.dispatcher.calls.size(); at-- > 0;) {
        if (h.dispatcher.calls[at].action == "Table.samples" && queryOf(h, at).distinct) {
            return at;
        }
    }
    FAIL("no distinct request");
    return 0;
}

// A column filter read back from the table's `filters` element value.
table::ColumnFilter columnFilterOf(morph::ui::Screen const& screen, std::string_view column) {
    table::ColumnFilter filter;
    std::string const text = readJson(screen, R"({"ref":"t.filters.columns.)" + std::string{column} + R"("})");
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(filter, text));
    return filter;
}

// The model table `t` over @p description, its first page answered.
std::unique_ptr<morph::ui::Screen> mountDescribed(TableHarness& h, table::TableDescription const& description,
                                                  std::string_view text = morph::ui::testing::kConfigModelTable) {
    auto screen = h.mount(text);
    replyDescribe(h, description);
    table::TableQuery const first = lastQuery(h);
    h.reply(oldestOpen(h, "Table.samples"),
            pageJsonAt(description.revision, 0, 0, 0, first.columns.value_or(std::vector<std::string>{})));
    return screen;
}

// `id`, then a column of each kind a condition is offered by: `n` Integer, `b` Bool, `code` Custom, `d` Date, `dec`
// Decimal and `weight` a Measure in g or kg.
table::TableDescription kindsDescription() {
    table::TableDescription d;
    d.id = "samples";
    d.label = "Samples";
    d.key = "id";
    auto const add = [&d](std::string id, table::ColumnKind kind) {
        d.columns.push_back(table::TableColumn{.info = {.id = std::move(id), .kind = kind}});
    };
    add("id", table::ColumnKind::Integer);
    add("n", table::ColumnKind::Integer);
    add("b", table::ColumnKind::Bool);
    add("code", table::ColumnKind::Custom);
    add("d", table::ColumnKind::Date);
    add("dec", table::ColumnKind::Decimal);
    add("weight", table::ColumnKind::Measure);
    d.columns.back().info.dimensions = {"mass"};
    d.units = {table::UnitInfo{.id = "g", .symbol = "g", .dimension = "mass", .toBase = *table::parseDecimal("0.001"),
                               .decimals = 0},
               table::UnitInfo{.id = "kg", .symbol = "kg", .dimension = "mass", .toBase = *table::parseDecimal("1"),
                               .decimals = 3},
               table::UnitInfo{.id = "%", .symbol = "%", .dimension = "ratio", .toBase = *table::parseDecimal("0.01"),
                               .decimals = 1}};
    return table::describe(std::move(d));
}

// The days of a month as a distinct reply's values: `YYYY-MM-DD`, each a bucket of one day.
std::string dayValues(std::string_view month, int days) {
    std::string out = R"({"values":[)";
    for (int day = 1; day <= days; ++day) {
        std::string const text = std::format("{}-{:02}", month, day);
        out += std::string{day == 1 ? "" : ","} + R"({"text":")" + text + R"(","operand":")" + text + ".." + text +
               R"(","count":1})";
    }
    return out + "],\"distinct\":" + std::to_string(days) + "}";
}

}  // namespace

// Mutation: Apply writing the side the plan did not choose (the ticked values where it chose the unticked ones).
TEST_CASE("filter menu: values are counted over what the other filters leave, and ticks build the smaller side",
          "[ui][table][config]") {
    TableHarness h;
    auto screen = h.mount(kStatusRows);
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    chooseOption(h, "table_filter_op_1", "equals");
    typeInto(h, "table_filter_operand_1", "A");
    h.advance(250ms);
    REQUIRE(readJson(*screen, R"({"ref":"t.filters.columns.site"})") ==
            R"({"combine":"any","exclude":[],"include":[{"eq":"A"}]})");
    dismissMenu(h);
    std::int64_t const runs = std::stoll(readJson(*screen, R"({"ref":"n"})"));
    clickFilter(h, table, 0);
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "label") == "Closed (2)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_1", "label") == "Open (2)");
    CHECK_FALSE(mounted(h, "Checkbox", "table_filter_value_2"));                       // no Held
    CHECK(propOf(h, "Checkbox", "table_filter_blanks", "label") == "(Blanks)");
    CHECK(propOf(h, "Checkbox", "table_filter_blanks", "visible") != "false");
    toggleBox(h, "table_filter_value_1");
    pressButton(h, "table_filter_apply");
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.status"})") ==
          R"({"combine":"any","exclude":[{"oneOf":["Open"]}],"include":[]})");
    CHECK(propOf(h, "Dialog", "table_filter", "open") == "false");
    CHECK(std::stoll(readJson(*screen, R"({"ref":"n"})")) == runs + 1);
    CHECK(h.backend.prop(table, "filtered") == "[0,1]");
    CHECK(elementValue(*screen, "t", "viewCount") == "3");
    clickFilter(h, table, 0);
    CHECK(propOf(h, "Checkbox", "table_filter_value_1", "label") == "Open (2)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_1", "checked") == "false");
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "checked") == "true");
    dismissMenu(h);
    // Hiding a filtered column keeps its filter, and its header says so again once it is shown.
    chooseMenu(h, table, 0, {"Hide column"});
    CHECK(elementValue(*screen, "t", "viewCount") == "3");
    CHECK(h.backend.prop(table, "filtered") == "[0,1]");
    chooseMenu(h, table, std::nullopt, {"Reset columns"});
    CHECK(h.backend.prop(table, "filtered") == "[0,1]");
    chooseMenu(h, table, std::nullopt, {"Clear all filters"});
    CHECK(elementValue(*screen, "t", "filters") == R"({"columns":{},"groups":[]})");
    CHECK(h.backend.prop(table, "filtered") == "[]");
    CHECK(std::stoll(readJson(*screen, R"({"ref":"n"})")) == runs + 2);
}

// Mutation: a condition's edit keeping the ticks (the values part stays beside the condition).
TEST_CASE("filter menu: values and conditions are exclusive, and reopening shows the stored part", "[ui][table][config]") {
    SECTION("a text column") {
        TableHarness h;
        auto screen = h.mount(kStatusRows);
        int const table = h.backend.all("Table").front();
        clickFilter(h, table, 0);
        selectAll(h, false);
        toggleBox(h, "table_filter_blanks");
        pressButton(h, "table_filter_apply");
        CHECK(readJson(*screen, R"({"ref":"t.filters.columns.status"})") ==
              R"({"combine":"any","exclude":[],"include":[{"isEmpty":true}]})");
        clickFilter(h, table, 0);
        CHECK(propOf(h, "Checkbox", "table_filter_blanks", "checked") == "true");
        for (std::string_view const value : {"table_filter_value_0", "table_filter_value_1", "table_filter_value_2"}) {
            CHECK(propOf(h, "Checkbox", value, "checked") == "false");
        }
        CHECK(propOf(h, "Select", "table_filter_op_1", "selected") == "none");
        chooseOption(h, "table_filter_op_1", "contains");
        typeInto(h, "table_filter_operand_1", "pe");
        h.advance(250ms);
        CHECK(readJson(*screen, R"({"ref":"t.filters.columns.status"})") ==
              R"({"combine":"any","exclude":[],"include":[{"contains":"pe"}]})");
        CHECK(propOf(h, "Checkbox", "table_filter_blanks", "checked") == "true");   // the ticks start again
        CHECK(propOf(h, "Checkbox", "table_filter_all", "checked") == "true");
        dismissMenu(h);
        clickFilter(h, table, 0);
        CHECK(propOf(h, "Select", "table_filter_op_1", "selected") == R"("contains")");
        CHECK(propOf(h, "TextInput", "table_filter_operand_1", "text") == "pe");
        CHECK(propOf(h, "Checkbox", "table_filter_all", "checked") == "true");
    }
    SECTION("a custom column has conditions alone") {
        TableHarness h;
        auto screen = mountDescribed(h, kindsDescription(),
                                     withTableKey(morph::ui::testing::kConfigModelTable,
                                                  R"("filters":{"columns":{"code":{"include":[{"isEmpty":true}]}}})"));
        int const table = h.backend.all("Table").front();
        std::size_t const calls = h.dispatcher.count("Table.samples");
        clickFilter(h, table, 3);
        CHECK(propOf(h, "Select", "table_filter_op_1", "selected") == R"("is empty")");
        CHECK_FALSE(mounted(h, "Checkbox", "table_filter_value_0"));
        CHECK_FALSE(mounted(h, "TextInput", "table_filter_search"));
        CHECK(h.dispatcher.count("Table.samples") == calls);                           // no values asked for
    }
}

// Mutation: the search sent on every keystroke (no debounce).
TEST_CASE("filter menu: a search typed quickly is one distinct request after the pause", "[ui][table][config]") {
    TableHarness h;
    auto screen = mountDescribed(h, samplesDescription(5));
    int const table = h.backend.all("Table").front();
    std::size_t const before = h.dispatcher.count("Table.samples");
    clickFilter(h, table, 1);
    REQUIRE(h.dispatcher.count("Table.samples") == before + 1);
    REQUIRE(lastQuery(h).distinct.has_value());
    h.reply(lastDistinct(h), distinctReply(h.described.revision, R"({"values":[{"text":"a","operand":"a","count":1}],"distinct":1})"));
    typeInto(h, "table_filter_search", "a");
    h.advance(50ms);
    typeInto(h, "table_filter_search", "ab");
    h.advance(50ms);
    typeInto(h, "table_filter_search", "abc");
    h.advance(249ms);
    CHECK(h.dispatcher.count("Table.samples") == before + 1);
    h.advance(1ms);
    CHECK(h.dispatcher.count("Table.samples") == before + 2);
    REQUIRE(lastQuery(h).distinct.has_value());
    CHECK(lastQuery(h).distinct->search == "abc");
}

// Mutation: the condition sent on every keystroke, or a half-typed between sent.
TEST_CASE("filter menu: a condition applies once after typing pauses, and an incomplete between is not sent",
          "[ui][table][config]") {
    TableHarness h;
    auto screen = mountDescribed(h, samplesDescription(5));
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    chooseOption(h, "table_filter_op_1", "contains");
    std::string const letters = "abcdefghij";
    for (std::size_t at = 1; at <= letters.size(); ++at) {
        typeInto(h, "table_filter_operand_1", letters.substr(0, at));
        if (at < letters.size()) {
            h.advance(150ms);
        }
    }
    std::size_t const pages = pageCalls(h);
    h.advance(249ms);
    CHECK(pageCalls(h) == pages);
    h.advance(1ms);
    REQUIRE(pageCalls(h) == pages + 1);
    REQUIRE(lastQuery(h).filters.columns.contains("name"));
    CHECK(lastQuery(h).filters.columns.at("name").include.front().contains == "abcdefghij");
    dismissMenu(h);

    clickFilter(h, table, 2);
    chooseOption(h, "table_filter_op_1", "between");
    typeInto(h, "table_filter_operand_1", "2");
    std::size_t const waiting = pageCalls(h);
    h.advance(1s);
    CHECK(pageCalls(h) == waiting);                                                    // no second end
    typeInto(h, "table_filter_second_1", "5");
    h.advance(250ms);
    REQUIRE(pageCalls(h) == waiting + 1);
    REQUIRE(lastQuery(h).filters.columns.contains("c3"));
    CHECK(lastQuery(h).filters.columns.at("c3").include.front().between ==
          std::array<std::string, 2>{"2", "5"});
    typeInto(h, "table_filter_operand_1", "3");
    pressButton(h, "table_filter_apply");
    REQUIRE(pageCalls(h) == waiting + 2);                                              // at once
    CHECK(lastQuery(h).filters.columns.at("c3").include.front().between ==
          std::array<std::string, 2>{"3", "5"});
}

// Mutation: the note line ignoring `partial`.
TEST_CASE("filter menu: a partial list says it may grow and invalid cells are counted under it", "[ui][table][config]") {
    TableHarness h;
    auto screen = mountDescribed(h, samplesDescription(5));
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    h.reply(lastDistinct(h),
            distinctReply(h.described.revision, R"({"values":[{"text":"x","operand":"x","count":4}],"distinct":1,)"
                                                R"("blanks":0,"invalid":3,"partial":true})"));
    // The recording backend writes a line break as `\n`.
    CHECK(h.text("table_filter_note") == R"(The list may grow while data loads\n3 cells could not be read)");
    CHECK(mounted(h, "Checkbox", "table_filter_value_0"));
    CHECK_FALSE(mounted(h, "Checkbox", "table_filter_value_1"));
}

// Mutation: "(Select all)" under a search resolved to the first page of the values it shows.
TEST_CASE("filter menu: Select all under a search covers every match, and both sides over the bound refuse",
          "[ui][table][config]") {
    std::string rows;
    for (int at = 0; at < 25'000; ++at) {
        rows += std::format(R"({}{{"id":{},"code":"c{:05}"}})", at == 0 ? "" : ",", at, at);
    }
    std::string const text = R"({"vocab":"ui/1","id":"codes","state":{"items":{"init":{"lit":[)" + rows +
                             R"(]}}},"view":{"kind":"table","id":"t","rows":{"ref":"items"},)"
                             R"("columns":[{"id":"code","label":"Code"}]}})";
    TableHarness h;
    auto screen = h.mount(text);
    int const table = h.backend.all("Table").front();
    auto const search = [&h](std::string_view typed) {
        typeInto(h, "table_filter_search", typed);
        h.advance(250ms);
    };
    auto const members = [](table::FilterEntry const& entry) {
        return entry.oneOf.value_or(std::vector<std::string>{});
    };

    clickFilter(h, table, 0);
    selectAll(h, false);
    search("c0");
    selectAll(h, true);
    pressButton(h, "table_filter_apply");
    table::ColumnFilter filter = columnFilterOf(*screen, "code");
    REQUIRE(filter.include.size() == 1);
    CHECK(filter.exclude.empty());
    std::vector<std::string> named = members(filter.include.front());
    REQUIRE(named.size() == 10'000);
    CHECK(named.front() == "c00000");
    CHECK(named.back() == "c09999");

    clickFilter(h, table, 0);
    selectAll(h, false);
    search("c1");
    selectAll(h, true);
    search("c2");
    selectAll(h, true);
    pressButton(h, "table_filter_apply");
    filter = columnFilterOf(*screen, "code");
    CHECK(filter.include.empty());
    REQUIRE(filter.exclude.size() == 1);
    named = members(filter.exclude.front());
    REQUIRE(named.size() == 10'000);
    CHECK(named.front() == "c00000");
    CHECK(named.back() == "c09999");

    std::string const before = elementValue(*screen, "t", "filters");
    clickFilter(h, table, 0);
    selectAll(h, false);
    for (std::string_view const typed : {"c1", "c00", "c01"}) {
        search(typed);
        selectAll(h, true);
    }
    pressButton(h, "table_filter_apply");
    CHECK(h.text("table_filter_note") == "Too many values to list; filter with a condition");
    CHECK(elementValue(*screen, "t", "filters") == before);
}

// Mutation: conditions offered by Text for every kind; an operand sent as typed.
TEST_CASE("filter menu: conditions follow the column's kind, and typed operands are canonical", "[ui][table][config]") {
    TableHarness h;
    auto screen = mountDescribed(
        h, kindsDescription(),
        R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
        R"("view":{"kind":"localeScope","locale":"de","children":[)"
        R"({"kind":"table","id":"t","model":"sample","table":"samples"}]}})");
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    CHECK(propOf(h, "Select", "table_filter_op_1", "options") == optionsOf({"=", "≠", "<", "≤", ">", "≥", "between"}));
    dismissMenu(h);
    clickFilter(h, table, 2);
    CHECK_FALSE(mounted(h, "Select", "table_filter_op_1"));
    dismissMenu(h);
    clickFilter(h, table, 3);
    CHECK(propOf(h, "Select", "table_filter_op_1", "options") == optionsOf({"is empty", "is not empty"}));
    dismissMenu(h);
    clickFilter(h, table, 4);
    CHECK(propOf(h, "DateTimeInput", "table_filter_operand_1", "mode") == "Date");
    dismissMenu(h);

    clickFilter(h, table, 5);
    chooseOption(h, "table_filter_op_1", ">");
    typeInto(h, "table_filter_operand_1", "1.234,5");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.dec.include"})") == R"([{"gt":"1234.5"}])");
    dismissMenu(h);

    clickFilter(h, table, 6);
    CHECK(propOf(h, "Select", "table_filter_unit_1", "options") == optionsOf({"g", "kg"}));
    chooseOption(h, "table_filter_op_1", ">");
    typeInto(h, "table_filter_operand_1", "0,3 kg");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.weight.include"})") == R"([{"gt":"0.3 kg"}])");
    typeInto(h, "table_filter_operand_1", "250");
    chooseOption(h, "table_filter_unit_1", "g");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.weight.include"})") == R"([{"gt":"250 g"}])");
    typeInto(h, "table_filter_operand_1", "12,5 xx");
    h.advance(250ms);
    CHECK(propOf(h, "TextInput", "table_filter_operand_1", "errors") != "[]");
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.weight.include"})") == R"([{"gt":"250 g"}])");
}

// Mutation: the Measure list drawn without dimension headings.
TEST_CASE("filter menu: a Measure column's values are grouped under a heading per dimension", "[ui][table][config]") {
    TableHarness h;
    auto screen = mountDescribed(h, kindsDescription());
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 6);
    h.reply(lastDistinct(h),
            distinctReply(h.described.revision,
                          R"({"values":[{"text":"0.3 kg","operand":"0.3 kg","count":2,"dimension":"mass","unit":"kg"},)"
                          R"({"text":"0.5 %","operand":"0.5 %","count":1,"dimension":"ratio","unit":"%"}],"distinct":2})"));
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "label").ends_with("(2)"));   // 300 g counted with 0.3 kg
    std::string const dump = h.backend.dump();
    std::size_t const mass = dump.find("testId=table_filter_heading_mass");
    std::size_t const first = dump.find("testId=table_filter_value_0");
    std::size_t const ratio = dump.find("testId=table_filter_heading_ratio");
    std::size_t const second = dump.find("testId=table_filter_value_1");
    REQUIRE(mass != std::string::npos);
    REQUIRE(ratio != std::string::npos);
    CHECK(mass < first);
    CHECK(first < ratio);
    CHECK(ratio < second);
}

// Mutation: the group menu sending `together` as one oneOf.
TEST_CASE("filter menu: a group's values are pooled and the match line builds the quantifier", "[ui][table][config]") {
    constexpr std::string_view kCompanies = R"({
      "vocab": "ui/1", "id": "companies",
      "state": { "items": { "init": { "lit": [
        { "id": 1, "companyA": "F", "companyB": "S", "companyC": "X" },
        { "id": 2, "companyA": "F", "companyB": "F", "companyC": "X" },
        { "id": 3, "companyA": "S", "companyB": "F", "companyC": "F" },
        { "id": 4, "companyA": "F", "companyB": "X", "companyC": "Y" },
        { "id": 5, "companyA": "X", "companyB": "Y", "companyC": "Z" } ] } } },
      "view": { "kind": "table", "id": "t", "rows": { "ref": "items" },
                "columns": [ { "id": "companyA" }, { "id": "companyB" }, { "id": "companyC" } ],
                "groups": [ { "id": "company", "label": "Company", "columns": { "lit": ["companyA", "companyB", "companyC"] } } ] } })";
    TableHarness h;
    auto screen = h.mount(kCompanies);
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.prop(table, "columnGroups") == "[Company:0+3]");
    clickGroupFilter(h, table, 0);
    CHECK(propOf(h, "Dialog", "table_filter", "open") == "true");
    dismissMenu(h);
    moveColumn(h, table, 1, 2);
    CHECK(h.backend.prop(table, "columnGroups") == "[]");
    CHECK(std::ranges::contains(labelsOf(menuOf(h, table, 0)), "Filter Company, 3 columns…"));
    chooseMenu(h, table, 0, {"Filter Company, 3 columns…"});
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "label") == "F (4)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_1", "label") == "S (2)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_2", "label") == "X (4)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_3", "label") == "Y (2)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_4", "label") == "Z (1)");
    toggleBox(h, "table_filter_value_0");
    toggleBox(h, "table_filter_value_1");
    chooseOption(h, "table_filter_match", "all of them, each in a column");
    pressButton(h, "table_filter_apply");
    CHECK(readJson(*screen, R"({"ref":"t.filters.groups"})") ==
          R"([{"combine":"any","exclude":[],"group":"company","include":[{"eq":"F"},{"eq":"S"}],"match":"any",)"
          R"("mode":"together"}])");
    CHECK(elementValue(*screen, "t", "viewCount") == "2");
    CHECK(morph::ui::testing::rowOrder(h) == "1 3");
    chooseMenu(h, table, 0, {"Filter Company, 3 columns…"});
    chooseOption(h, "table_filter_match", "in at least N columns");
    typeInto(h, "table_filter_count", "2");
    pressButton(h, "table_filter_apply");
    CHECK(readJson(*screen, R"({"ref":"t.filters.groups"})") ==
          R"([{"combine":"any","exclude":[],"group":"company","include":[{"oneOf":["F","S"]}],)"
          R"("match":{"atLeast":2},"mode":"columns"}])");
    CHECK(elementValue(*screen, "t", "viewCount") == "3");
    CHECK(morph::ui::testing::rowOrder(h) == "1 2 3");
}

// A user-formed group has no values list, and its filter names its columns. Mutation: removing the group leaving its
// filter held (the last CHECK: the groups still hold it).
TEST_CASE("filter menu: a user-formed group offers conditions and the match line, no values list", "[ui][table][config]") {
    TableHarness h;
    auto screen = h.mount(kKinds);
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    press(h, "table_columns_group");
    toggleBox(h, "table_group_column_a");
    toggleBox(h, "table_group_column_b");
    typeInto(h, "table_group_label", "Pair");
    press(h, "table_group_create");
    press(h, "table_columns_done");
    chooseMenu(h, table, 0, {"Filter Pair, 2 columns…"});
    CHECK(propOf(h, "Dialog", "table_filter", "open") == "true");
    CHECK_FALSE(mounted(h, "TextInput", "table_filter_search"));
    CHECK_FALSE(mounted(h, "Checkbox", "table_filter_all"));
    CHECK_FALSE(mounted(h, "Checkbox", "table_filter_value_0"));
    CHECK(propOf(h, "Select", "table_filter_op_1", "options") ==
          optionsOf({"equals", "does not equal", "contains", "begins with", "ends with"}));
    CHECK(propOf(h, "Select", "table_filter_match", "options") ==
          optionsOf({"in any column", "in every column", "in at least N columns", "in exactly N columns"}));
    chooseOption(h, "table_filter_op_1", "equals");
    typeInto(h, "table_filter_operand_1", "F");
    chooseOption(h, "table_filter_match", "in every column");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.groups"})") ==
          R"([{"columns":["a","b"],"combine":"any","exclude":[],"include":[{"eq":"F"}],"match":"all","mode":"columns"}])");
    dismissMenu(h);
    chooseMenu(h, table, 0, {"Filter Pair, 2 columns…"});
    CHECK(propOf(h, "Select", "table_filter_op_1", "selected") == R"("equals")");
    CHECK(propOf(h, "TextInput", "table_filter_operand_1", "text") == "F");
    CHECK(propOf(h, "Select", "table_filter_match", "selected") == R"("in every column")");
    dismissMenu(h);
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    press(h, "table_group_remove_g1");
    CHECK(readJson(*screen, R"({"ref":"t.filters.groups"})") == "[]");
}

// Mutation: adjacent buckets sent as one entry each (the touching days' section).
TEST_CASE("filter menu: a date column lists years, then a year's months, then a month's days", "[ui][table][config]") {
    table::TableDescription described = samplesDescription(5);
    described.columns.at(2).info.kind = table::ColumnKind::Date;
    described = table::describe(std::move(described));
    std::string const years = R"({"values":[{"text":"2025","operand":"2025-01-01..2025-12-31","count":3},)"
                              R"({"text":"2026","operand":"2026-01-01..2026-12-31","count":5}],"distinct":2})";
    std::string const months = R"({"values":[{"text":"2026-01","operand":"2026-01-01..2026-01-31","count":1},)"
                               R"({"text":"2026-03","operand":"2026-03-01..2026-03-31","count":3},)"
                               R"({"text":"2026-04","operand":"2026-04-01..2026-04-30","count":1},)"
                               R"({"text":"2026-05","operand":"2026-05-01..2026-05-31","count":1}],"distinct":4})";
    TableHarness h;
    auto screen = mountDescribed(h, described);
    int const table = h.backend.all("Table").front();
    auto const answer = [&h](std::string const& values) {
        h.reply(lastDistinct(h), distinctReply(h.described.revision, values));
    };
    // Opens the menu, then 2026 and its March; the list is 2025, 2026, its four months, March's days after March.
    auto const openTree = [&] {
        clickFilter(h, table, 2);
        REQUIRE(lastQuery(h).distinct->bucket == table::DateBucket::Year);
        answer(years);
        pressButton(h, "table_filter_open_1");
        REQUIRE(lastQuery(h).distinct->bucket == table::DateBucket::Month);
        CHECK(lastQuery(h).distinct->search == "2026-");
        answer(months);
        pressButton(h, "table_filter_open_3");
        REQUIRE(lastQuery(h).distinct->bucket == table::DateBucket::Day);
        CHECK(lastQuery(h).distinct->search == "2026-03-");
        answer(dayValues("2026-03", 31));
    };
    SECTION("buckets apply in ascending order, and reopen ticked") {
        openTree();
        CHECK_FALSE(mounted(h, "Checkbox", "table_filter_all"));
        CHECK(propOf(h, "Checkbox", "table_filter_value_0", "checked") == "false");
        CHECK(propOf(h, "Checkbox", "table_filter_value_1", "checked") == "false");
        toggleBox(h, "table_filter_value_3");                                          // March
        CHECK(propOf(h, "Checkbox", "table_filter_value_4", "checked") == "true");     // its first day, covered
        CHECK(propOf(h, "Checkbox", "table_filter_value_4", "enabled") == "false");
        toggleBox(h, "table_filter_value_0");                                          // 2025
        pressButton(h, "table_filter_apply");
        CHECK(readJson(*screen, R"({"ref":"t.filters.columns.c3"})") ==
              R"({"combine":"any","exclude":[],"include":[{"between":["2025-01-01","2025-12-31"]},)"
              R"({"between":["2026-03-01","2026-03-31"]}]})");
        clickFilter(h, table, 2);
        answer(years);
        pressButton(h, "table_filter_open_1");
        answer(months);
        CHECK(propOf(h, "Checkbox", "table_filter_value_0", "checked") == "true");     // 2025
        CHECK(propOf(h, "Checkbox", "table_filter_value_1", "checked") == "false");    // 2026
        CHECK(propOf(h, "Checkbox", "table_filter_value_3", "checked") == "true");     // March
    }
    // Mutation: a bucket a merged range covers shown disabled (the range is no listed bucket's own tick).
    SECTION("buckets Apply merged come back ticked and can be unticked") {
        openTree();
        toggleBox(h, "table_filter_value_3");                                          // March
        toggleBox(h, "table_filter_value_35");                                         // April, after March's days
        pressButton(h, "table_filter_apply");
        REQUIRE(readJson(*screen, R"({"ref":"t.filters.columns.c3.include"})") ==
                R"([{"between":["2026-03-01","2026-04-30"]}])");
        clickFilter(h, table, 2);
        answer(years);
        pressButton(h, "table_filter_open_1");
        answer(months);
        CHECK(propOf(h, "Checkbox", "table_filter_value_3", "checked") == "true");
        CHECK(propOf(h, "Checkbox", "table_filter_value_4", "checked") == "true");
        CHECK(propOf(h, "Checkbox", "table_filter_value_4", "enabled") != "false");
        toggleBox(h, "table_filter_value_4");                                          // untick April
        pressButton(h, "table_filter_apply");
        CHECK(readJson(*screen, R"({"ref":"t.filters.columns.c3.include"})") ==
              R"([{"between":["2026-03-01","2026-03-31"]}])");
    }
    SECTION("touching days are one entry") {
        openTree();
        pressButton(h, "table_filter_open_35");                                        // April, after March's days
        answer(dayValues("2026-04", 30));
        for (int at = 4; at <= 34; ++at) {
            toggleBox(h, "table_filter_value_" + std::to_string(at));
        }
        for (int at = 36; at <= 44; ++at) {
            toggleBox(h, "table_filter_value_" + std::to_string(at));
        }
        pressButton(h, "table_filter_apply");
        CHECK(readJson(*screen, R"({"ref":"t.filters.columns.c3"})") ==
              R"({"combine":"any","exclude":[],"include":[{"between":["2026-03-01","2026-04-09"]}]})");
    }
    SECTION("more than 32 entries apply nothing") {
        openTree();
        pressButton(h, "table_filter_open_35");
        answer(dayValues("2026-04", 30));
        pressButton(h, "table_filter_open_66");                                        // May, after April's days
        answer(dayValues("2026-05", 31));
        std::string const before = elementValue(*screen, "t", "filters");
        // Every second day from 2026-03-01: March's odd days, April's even days, then 2 and 4 May.
        for (int at = 4; at <= 34; at += 2) {
            toggleBox(h, "table_filter_value_" + std::to_string(at));
        }
        for (int at = 37; at <= 65; at += 2) {
            toggleBox(h, "table_filter_value_" + std::to_string(at));
        }
        toggleBox(h, "table_filter_value_68");
        toggleBox(h, "table_filter_value_70");
        pressButton(h, "table_filter_apply");
        CHECK(h.text("table_filter_note") == "Too many dates to list; filter with a condition");
        CHECK(elementValue(*screen, "t", "filters") == before);
    }
}

// Mutation: choosing an unchecked unit clearing it, as choosing the checked one does.
TEST_CASE("display unit: the header menu writes the layout's units", "[ui][table][config]") {
    TableHarness h;
    auto screen = mountDescribed(h, kindsDescription());
    int const table = h.backend.all("Table").front();
    std::size_t const pages = pageCalls(h);
    chooseMenu(h, table, 6, {"Display unit", "mass: g"});
    CHECK(readJson(*screen, R"({"ref":"t.layout.units"})") == R"({"weight":{"mass":"g"}})");
    CHECK(pageCalls(h) == pages);
    chooseMenu(h, table, 6, {"Display unit", "mass: g"});                              // the checked one clears it
    CHECK(readJson(*screen, R"({"ref":"t.layout.units"})") == "null");
}

// Mutation: the entry labelled with the reply's `text`.
TEST_CASE("filter menu: the values list labels by the column's display", "[ui][table][config]") {
    TableHarness h;
    auto screen = mountDescribed(h, companionDescription(), doc(kTable));
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    h.reply(lastDistinct(h), distinctReply(companionDescription().revision,
                                           R"({"values":[{"text":"x","operand":"0.124","count":1},)"
                                           R"({"text":"y","operand":"0.123","count":2},)"
                                           R"({"text":"z","operand":"0.5","count":1}],"distinct":3})"));
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "label") == "0.124 (1)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_1", "label") == "0.123 (2)");
    CHECK(propOf(h, "Checkbox", "table_filter_value_2", "label") == "0.50 (1)");
}

// Mutation: filter buttons offered only by a table that has menus (`configurable: false` returning before them).
TEST_CASE("filter menu: a table with configurable false keeps its filter buttons", "[ui][table][config]") {
    TableHarness h;
    auto screen = h.mount(withTableKey(kStatusRows, R"("configurable":false)"));
    int const table = h.backend.all("Table").front();
    CHECK(menuOf(h, table, 0).empty());
    CHECK(h.backend.prop(table, "columns").contains("filterable=true"));
    clickFilter(h, table, 0);
    CHECK(propOf(h, "Dialog", "table_filter", "open") == "true");
    toggleBox(h, "table_filter_value_2");
    pressButton(h, "table_filter_apply");
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.status.exclude"})") == R"([{"oneOf":["Open"]}])");
}

// Mutation: the counts grouped with "," whatever the session's locale.
TEST_CASE("filter menu: counts are grouped with the session's separator", "[ui][table][config]") {
    TableHarness h;
    auto screen = mountDescribed(
        h, samplesDescription(5),
        R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
        R"("view":{"kind":"localeScope","locale":"de","children":[)"
        R"({"kind":"table","id":"t","model":"sample","table":"samples"}]}})");
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    h.reply(lastDistinct(h), distinctReply(h.described.revision,
                                           R"({"values":[{"text":"x","operand":"x","count":1234}],"distinct":1,)"
                                           R"("invalid":1234})"));
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "label") == "x (1.234)");
    CHECK(h.text("table_filter_note") == "1.234 cells could not be read");
}

// Mutation: a reply asked for before the record changed kept as fresh (the reopened menu sends nothing).
TEST_CASE("filter menu: a values list asked for before a change is not kept", "[ui][table][config]") {
    TableHarness h;
    auto screen = mountDescribed(h, samplesDescription(5));
    int const table = h.backend.all("Table").front();
    std::string const values = R"({"values":[{"text":"a","operand":"a","count":1}],"distinct":1})";
    clickFilter(h, table, 1);
    std::size_t const asked = lastDistinct(h);
    morph::ui::testing::change(h, {});
    h.reply(asked, distinctReply(h.described.revision, values));
    dismissMenu(h);
    std::size_t const calls = h.dispatcher.count("Table.samples");
    clickFilter(h, table, 1);
    REQUIRE(h.dispatcher.count("Table.samples") > calls);
    std::size_t const again = lastDistinct(h);
    CHECK(again > asked);
    h.reply(again, distinctReply(h.described.revision, values));
    dismissMenu(h);
    std::size_t const kept = h.dispatcher.count("Table.samples");
    clickFilter(h, table, 1);
    CHECK(h.dispatcher.count("Table.samples") == kept);                              // a reply after the change is kept
}

// Mutation: a user-formed group's menu asking for its values (a distinct request with the group's layout id).
TEST_CASE("filter menu: a user-formed group of a model's table sends no values request and its filter names its columns",
          "[ui][table][config]") {
    table::TableDescription pair;
    pair.id = "samples";
    pair.label = "Samples";
    pair.key = "id";
    pair.columns = {table::TableColumn{.info = {.id = "id", .kind = table::ColumnKind::Integer}},
                    table::TableColumn{.info = {.id = "a", .kind = table::ColumnKind::Text}},
                    table::TableColumn{.info = {.id = "b", .kind = table::ColumnKind::Text}}};
    pair = table::describe(std::move(pair));
    REQUIRE(pair.capabilities.distinct);
    TableHarness h;
    auto screen = mountDescribed(h, pair);
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, std::nullopt, {"Columns…"});
    press(h, "table_columns_group");
    toggleBox(h, "table_group_column_a");
    toggleBox(h, "table_group_column_b");
    typeInto(h, "table_group_label", "Pair");
    press(h, "table_group_create");
    press(h, "table_columns_done");
    std::size_t const calls = h.dispatcher.count("Table.samples");
    chooseMenu(h, table, 1, {"Filter Pair, 2 columns…"});
    CHECK(h.dispatcher.count("Table.samples") == calls);                             // no values request
    chooseOption(h, "table_filter_op_1", "equals");
    typeInto(h, "table_filter_operand_1", "F");
    chooseOption(h, "table_filter_match", "in every column");
    h.advance(250ms);
    REQUIRE(h.dispatcher.count("Table.samples") > calls);
    table::TableQuery const sent = lastQuery(h);
    REQUIRE(sent.filters.groups.size() == 1);
    table::GroupFilter const& group = sent.filters.groups.front();
    CHECK(group.group.empty());
    CHECK(group.columns == std::vector<std::string>{"a", "b"});
    CHECK(group.match.kind == table::ColumnCount::Kind::All);
    REQUIRE(group.include.size() == 1);
    CHECK(group.include.front().eq == "F");
    dismissMenu(h);
    // A column's "(Select all)" under a search names its values by pages of `maxLimit`.
    clickFilter(h, table, 1);
    h.reply(lastDistinct(h), distinctReply(pair.revision, R"({"values":[{"text":"x","operand":"x","count":1}],"distinct":1})"));
    selectAll(h, false);
    typeInto(h, "table_filter_search", "x");
    h.advance(250ms);
    h.reply(lastDistinct(h), distinctReply(pair.revision, R"({"values":[{"text":"x","operand":"x","count":1}],"distinct":1})"));
    selectAll(h, true);
    pressButton(h, "table_filter_apply");
    REQUIRE(lastQuery(h).distinct.has_value());
    CHECK(lastQuery(h).distinct->limit == 1000);
    CHECK(lastQuery(h).distinct->search == "x");
}

// Mutation: the one-kind refusal dropped (a group of a text and an integer column loads).
TEST_CASE("ui document: a rows table's groups are 2 to 16 of its column parts, of one kind, each in one group",
          "[ui][document]") {
    auto const with = [](std::string_view groups) { return withTableKey(kKinds, R"("groups":)" + std::string{groups}); };
    CHECK(loads(with(R"([{"id":"g","label":"G","columns":{"lit":["a","b"]}}])"), morph::ui::testing::tableCatalog()));
    CHECK(refusalOf(withTableKey(morph::ui::testing::kConfigModelTable,
                                 R"("groups":[{"id":"g","columns":{"lit":["a","b"]}}])")) ==
          "a model's table declares its column groups in its description; `groups` is for a table over `rows`");
    CHECK(refusalOf(with(R"([{"id":"g","columns":{"lit":["a","b"]}},{"id":"g","columns":{"lit":["c","n"]}}])")) ==
          "the group `g` is declared twice");
    CHECK(refusalOf(with(R"([{"id":"g","columns":{"ref":"items"}}])")) ==
          "a group's `columns` is a literal list of its columns' ids");
    CHECK(refusalOf(with(R"([{"id":"g","columns":{"lit":["a"]}}])")) == "a group has 2 to 16 columns");
    CHECK(refusalOf(with(R"([{"id":"g","columns":{"lit":["a","zz"]}}])")) ==
          "a group's columns are the ids of the table's column parts");
    CHECK(refusalOf(with(R"([{"id":"g","columns":{"lit":["a","b"]}},{"id":"h","columns":{"lit":["b","c"]}}])")) ==
          "the column `b` is in another group");
    CHECK(refusalOf(with(R"([{"id":"g","columns":{"lit":["a","n"]}}])")) == "a group's columns hold one kind of value");
}

// Mutation: WindowRows compiling a filter without the description's units (the measure operand is refused).
TEST_CASE("window rows: a filter on a measure reads its operand with the description's units", "[ui][table]") {
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
    table::FilterEntry above;
    above.gt = "0.3 kg";
    table::FilterSpec filter;
    filter.columns.insert_or_assign("weight", table::ColumnFilter{.combine = table::Combine::Any, .include = {above}, .exclude = {}});
    auto const set = rows->setFilter(filter);
    INFO((set ? std::string{} : set.error().message));
    CHECK(set.has_value());
}

// Mutation: the filter menu holding the mount's filter path strongly (the menu, kept by the element, keeps the
// mount's scope alive, and the runtime aborts at teardown: "Runtime destroyed while nodes are alive").
TEST_CASE("filter menu: a table whose menu set a filter tears down cleanly", "[ui][table][config]") {
    {
        TableHarness h;
        auto screen = h.mount(kStatusRows);
        int const table = h.backend.all("Table").front();
        clickFilter(h, table, 0);
        toggleBox(h, "table_filter_value_2");
        pressButton(h, "table_filter_apply");
        clickFilter(h, table, 1);
        REQUIRE(elementValue(*screen, "t", "filters").contains("Open"));
    }
    SUCCEED("the screen, the session and the runtime went without an abort");
}

// Mutation: the values lists kept without a bound (the first list, past 64 others, is still served from the cache).
TEST_CASE("filter menu: a table keeps at most 64 values lists, the oldest going first", "[ui][table][config]") {
    TableHarness h;
    auto screen = mountDescribed(h, samplesDescription(5));
    int const table = h.backend.all("Table").front();
    std::string const values = R"({"values":[{"text":"a","operand":"a","count":1}],"distinct":1})";
    clickFilter(h, table, 1);
    h.reply(lastDistinct(h), distinctReply(h.described.revision, values));
    for (int at = 0; at < 64; ++at) {
        typeInto(h, "table_filter_search", "s" + std::to_string(at));
        h.advance(250ms);
        h.reply(lastDistinct(h), distinctReply(h.described.revision, values));
    }
    std::size_t const calls = h.dispatcher.count("Table.samples");
    typeInto(h, "table_filter_search", "s0");                                          // the oldest but one: kept
    h.advance(250ms);
    CHECK(h.dispatcher.count("Table.samples") == calls);
    typeInto(h, "table_filter_search", "");                                            // the oldest: gone
    h.advance(250ms);
    CHECK(h.dispatcher.count("Table.samples") == calls + 1);
}
