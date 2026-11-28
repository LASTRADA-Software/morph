// SPDX-License-Identifier: Apache-2.0

// The model-backed `table` node on a screen: mounting, scrolling and focus.
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

using morph::ui::testing::describeJson;
using morph::ui::testing::keyedJson;
using morph::ui::testing::keyedJsonAt;
using morph::ui::testing::samplesDescription;
using namespace std::chrono_literals;
using morph::ui::CellLoad;
using morph::ui::Key;
using morph::ui::MountStats;
using morph::ui::testing::answerPages;
using morph::ui::testing::change;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::coreOf;
using morph::ui::testing::diagnosticsMentioning;
using morph::ui::testing::elementOf;
using morph::ui::testing::elementValue;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::headerLabels;
using morph::ui::testing::kModelTable;
using morph::ui::testing::kNoPersist;
using morph::ui::testing::kNoStatus;
using morph::ui::testing::kStored;
using morph::ui::testing::labelsOf;
using morph::ui::testing::lastQuery;
using morph::ui::testing::loads;
using morph::ui::testing::menuOf;
using morph::ui::testing::mountShown;
using morph::ui::testing::oldestOpen;
using morph::ui::testing::openCalls;
using morph::ui::testing::pageJsonRenaming;
using morph::ui::testing::pageJsonUnloaded;
using morph::ui::testing::queryOf;
using morph::ui::testing::refusalOf;
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

// Mutation: the row-scope index never pruned (it holds an entry for every row scrolled past).

TEST_CASE("model table: scrolling 10,000 rows keeps one row scope per realized row", "[ui][table]") {
    TableHarness h;
    auto screen = mountShown(h, withTableKey(kModelTable, R"("rowLabel":{"lit":"a row"})"), 100'000);
    for (std::size_t first = 0; first < 10'000; first += 30) {
        h.backend.scrollTable(tableOf(h), first, 30);
        h.settle();
        answerPages(h, 100'000);
    }
    // The realized rows, and at most one scroll's worth that left and are pruned when the next row comes.
    CHECK(morph::ui::detail::ScreenCore::tableRowScopes(elementOf(*screen, "list")) <= std::size_t{2} * 50);
}

// Mutation: a new revision clearing the element's selection (`selectedCount` is 0 after it).

TEST_CASE("model table: a new revision keeps the selection and the message", "[ui][table]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    auto const screen = h.mount(withTableKey(kModelTable, R"("selectionMode":"multiple")"));
    replyDescribe(h);
    store->release();
    h.settle();
    answerPages(h, 9093);
    h.backend.selectRows(tableOf(h), {Key{std::int64_t{1}}, Key{std::int64_t{2}}});
    h.settle();
    store->refuseSaves("over the table-state quota");
    h.backend.clickHeader(tableOf(h), 1);
    h.advance(500ms);
    REQUIRE(statusOf(h).message == "The table's arrangement could not be saved: over the table-state quota");
    table::TableDescription const b = samplesDescription(2);
    h.reply(oldestOpen(h, "Table.samples"), morph::ui::testing::pageJsonAt(b.revision, 0, 200, 9093, h.columns));
    replyDescribe(h, b);
    REQUIRE(headerLabels(h) == std::vector<std::string>{"Id", "Name"});  // mounted again from revision B
    CHECK(elementValue(*screen, "list", "selectedCount") == "2");
    CHECK(statusOf(h).message == "The table's arrangement could not be saved: over the table-state quota");
}

// Mutation: the body's `query` members replaced without a report (no diagnostic names `filters`).

TEST_CASE("model table: a body's query member the table owns is reported", "[ui][table]") {
    TableHarness h;
    auto const screen =
        mountShown(h, withTableKey(kModelTable, R"("body":{"lit":{"query":{"filters":{"columns":{}},"limit":3}}})"));
    CHECK(diagnosticsMentioning(h, "`filters` is the table's own; the body's is not used") == 1);
    CHECK(diagnosticsMentioning(h, "limit") == 0);
}

namespace {

// `kModelTable` with `"selectionMode": "multiple"`.

std::string selectTable() { return withTableKey(kModelTable, R"("selectionMode":"multiple")"); }

// `kModelTable` with a state `hk`, null, as the table's `highlightKey`, and three buttons that set it to 5012, 77 and
// null.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization, cert-err58-cpp): `kHighlight` is a test constant built from literals; if building it threw, the run would fail at load, which is what a broken fixture should do
std::string const kHighlight =
    R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},)"
    R"("state":{"hk":{"init":{"lit":null}}},)"
    R"("view":{"kind":"column","children":[)"
    R"({"kind":"button","label":"5012","testId":"to5012","onClick":[{"set":"hk","value":5012}]},)"
    R"({"kind":"button","label":"77","testId":"to77","onClick":[{"set":"hk","value":77}]},)"
    R"({"kind":"button","label":"none","testId":"toNone","onClick":[{"set":"hk","value":{"lit":null}}]},)"
    R"({"kind":"table","id":"list","testId":"list","model":"lab","table":"samples","highlightKey":{"ref":"hk"},)"
    R"("empty":[{"kind":"text","text":"No samples"}]}]}})";

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

}  // namespace

// Mutation: `keysOf` fetching the visible columns (a call's body lacks `"columns":[]`).

TEST_CASE("model table: a range over rows not held resolves through pages that name no columns", "[ui][table]") {
    TableHarness h;
    auto const screen = mountShown(h, selectTable(), 100'000);  // rows 0 to 399 held
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
    auto const screen = mountShown(h, selectTable(), 100'000);
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
    auto const screen = mountShown(h, kHighlight, 9'093);
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
    CHECK(askedForPage(h, marked, 5'000));                         // the page that holds view row 5,011
    CHECK(elementValue(*screen, "list", "selectedCount") == "0");  // never the selection
    click(h, "to77");
    replyKeyed(h, {});  // filtered out or gone: no row
    CHECK(h.backend.prop(table, "highlightedRow") == "none");
    CHECK(h.backend.prop(table, "scrolledToRow") == "5011");  // nothing scrolled
    std::size_t const first = h.dispatcher.calls.size();
    click(h, "to5012");
    click(h, "to77");          // two keys before a reply
    CHECK(stopped(h, first));  // the first key's fetch is stopped
    click(h, "toNone");
    CHECK(h.backend.prop(table, "highlightedRow") == "none");
}

namespace {

// A model-backed table whose `name` column's cell is a button `pick`, and a button that focuses `pick` in row 250.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization, cert-err58-cpp): `kPick` is a test constant built from literals; if building it threw, the run would fail at load, which is what a broken fixture should do
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
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
    std::iota(keys.begin(), keys.end(), std::int64_t{0});
    std::swap(keys.at(static_cast<std::size_t>(a)), keys.at(static_cast<std::size_t>(b)));
    return keys;
}

}  // namespace

// Mutation: focus not looked for by key after a reset (it stays on view row 7, which another row now holds).

TEST_CASE("model table: after a new sort, focus is found again by key once its page arrives", "[ui][table]") {
    TableHarness h;
    auto const screen = mountShown(h, kModelTable, 9'093);
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
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
    std::iota(keys.begin(), keys.end(), std::int64_t{0});
    keys.at(7) = 9'093;
    return keys;
}

}  // namespace

// Mutations: the located index ignored, the fallback taken instead (focus stays on view row 7, which another key now
// holds); the located row not scrolled to when the rows do not hold it (its page is never asked for).

TEST_CASE("model table: a new sort that moves the focused key off-screen keeps focus on it", "[ui][table]") {
    TableHarness h;
    auto const screen = mountShown(h, kModelTable, 9'093);
    int const table = focusSevenThenSort(h);
    // The first page of the new order lacks key 7, and every row in view is fetched: still no answer, no move.
    replyFirstPage(h, pageWithoutSeven());
    CHECK(h.backend.focusCell(table).first == 7);
    std::size_t const located = h.dispatcher.calls.size();
    replyKeyed(h, {{7, 5'000}});  // key 7 is at view row 5,000
    CHECK(h.backend.focusCell(table).first == 5'000);
    CHECK(h.backend.prop(table, "scrolledToRow") == "5000");  // not held: scrolled to, so the window fetches it
    h.advance(80ms);                                          // the window's settle after a scroll
    CHECK(askedForPage(h, located, 5'000));
    answerPages(h, 9'093);
    CHECK(h.backend.focusCell(table).first == 5'000);
}

// Mutation: the fallback's clamp applied while the count is not known (focus goes to view row 0).

TEST_CASE("model table: an answer that the focused key is gone, before the first page, keeps focus at its place",
          "[ui][table]") {
    TableHarness h;
    auto const screen = mountShown(h, kModelTable, 9'093);
    int const table = focusSevenThenSort(h);
    replyKeyed(h, {});  // key 7 is gone; no page of the new order has arrived, so the count is not known
    replyFirstPage(h, pageWithoutSeven());
    answerPages(h, 9'093);
    CHECK(h.backend.focusCell(table).first == 7);  // the row now at its place, key 9,093
}

// Mutation: the backend told of a located row before the count is known (focus is on view row 5,000 of a view with no
// rows yet).

TEST_CASE("model table: a located row answered before the first page is told once the count is known", "[ui][table]") {
    TableHarness h;
    auto const screen = mountShown(h, kModelTable, 9'093);
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
    auto const screen = mountShown(h, kModelTable, 9'093);
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
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
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
    auto const screen = mountShown(h, kHighlight, 9'093);
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
    auto const screen = mountShown(h, kPick, 9'093);
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
    TableHarness h;
    auto const screen = mountShown(h);
    h.backend.scrollTable(tableOf(h), 0, 30);
    h.settle();  // rows 0 to 39 realized (overscan 10)
    MountStats const before = statsOf(*screen);
    change(h, std::vector<std::int64_t>{});  // a change without keys: the pages again
    // the same page 0, with rows 0 to 19 renamed and rows 20 to 199 as they were
    h.reply(oldestOpen(h, "Table.samples"), pageJsonRenaming(revisionOf(h), 0, 200, 9093, h.columns, 0, 20));
    CHECK(statsOf(*screen).rowBinds - before.rowBinds == 20);  // K5
    CHECK(statsOf(*screen).cellsCreated == before.cellsCreated);
    CHECK(h.backend.find("Text", "text", "renamed-0").has_value());
}

// Review Focus 1. Mutation: the fitting of a new revision not reporting the dropped filter (the count is 0).

TEST_CASE("table rows: a new description revision drops what names a removed column", "[ui][table]") {
    auto const store = std::make_shared<GatedTableStateStore>(
        storeExecutor(),
        R"({"version":1,"sort":[{"column":"c3","direction":"asc"}],"filters":{"columns":{"c3":{"include":[{"gt":"1"}]}}}})");
    TableHarness h{store};
    auto const screen = h.mount(kModelTable);
    replyDescribe(h);
    store->release();
    h.settle();  // revision A: id, name, c3
    REQUIRE(lastQuery(h).sort.at(0).column == "c3");
    table::TableDescription const b = samplesDescription(2);  // revision B: id, name
    h.reply(oldestOpen(h, "Table.samples"),
            morph::ui::testing::pageJsonAt(b.revision, 0, 200, 9093, h.columns));  // a reply names another revision
    CHECK(h.dispatcher.count("Table.samples.describe") == 2);                      // exactly one more describe
    replyDescribe(h, samplesDescription(2));
    table::TableQuery const q = lastQuery(h);
    CHECK(std::ranges::none_of(q.sort, [](auto const& key) { return key.column == "c3"; }));
    CHECK_FALSE(q.filters.columns.contains("c3"));
    REQUIRE(q.columns.has_value());
    CHECK(std::ranges::find(*q.columns, "c3") == q.columns->end());
    CHECK(diagnosticsMentioning(h, "c3") == 1);                        // the filter; a sort key is not reported
    CHECK(headerLabels(h) == std::vector<std::string>{"Id", "Name"});  // mounted again from revision B
    CHECK(h.dispatcher.count("Table.samples.describe") == 2);          // once per revision
}

// Review Focus 2. Mutation: the load completion run with no liveness check (a query is sent, or the test crashes
// under the sanitizer build).

TEST_CASE("model table: unmounted while its state load is held", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kStored});
    TableHarness h{store};
    auto screen = h.mount(kModelTable);
    replyDescribe(h);
    h.backend.clickHeader(tableOf(h), 0);
    h.settle();  // nothing loaded yet: the change is held, not saved
    CHECK(store->saves() == 0);
    screen.reset();
    h.settle();
    store->release();
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == 0);          // nothing applied, no query
    REQUIRE(store->saves() == 1);                             // the held change, over the loaded document
    CHECK(store->lastSaved().contains(R"("contains":"a")"));  // the stored filter kept
    CHECK(table::readState(store->lastSaved())->sort->at(0).column == "id");
}

// Review Focus 4. Mutation: `empty` shown while `total().known` is false, or while rows are undecided.

TEST_CASE("model table: empty waits for the first reply and for undecided rows", "[ui][table]") {
    TableHarness h;
    auto const screen = h.mount(kModelTable);
    CHECK_FALSE(shownText(h, "No samples"));  // before the description
    replyDescribe(h);
    CHECK_FALSE(shownText(h, "No samples"));  // before any page
    CHECK(statusOf(h).counts.empty());
    CHECK(statusOf(h).shown);
    replyPage(h, morph::ui::testing::pageJsonAt(revisionOf(h), 0, 0, 0, h.columns, table::TotalKind::AtLeast, 40));
    CHECK_FALSE(shownText(h, "No samples"));
    CHECK(statusOf(h).counts == "0 so far, 40 loading");
    change(h, std::vector<std::int64_t>{});  // the level arrived: the page again,
    h.advance(80ms);                         // which holds no row, after tableSettle
    replyPage(h, morph::ui::testing::pageJsonAt(revisionOf(h), 0, 0, 0, h.columns));
    CHECK(shownText(h, "No samples"));
    CHECK(statusOf(h).counts == "0 rows");
}

// Mutations: a status line built without `failed` (no state, no retry); Retry sending the query without `retry`.

TEST_CASE("model table: a failed level offers Retry, which sends `retry`", "[ui][table]") {
    TableHarness h;
    auto const screen = h.mount(kModelTable);
    replyDescribe(h, withLevel(samplesDescription(3), "c3", "recipe"));
    replyPage(h, pageJsonUnloaded(revisionOf(h), 0, 200, 9093, h.columns,
                                  {{.index = 1, .levels = {"recipe"}, .failed = {"recipe"}}}, /*failed*/ {"recipe"}));
    CHECK(statusOf(h).state == "Recipe data could not be loaded.");
    REQUIRE(statusOf(h).retry);
    std::size_t const sent = h.dispatcher.count("Table.samples");
    h.backend.retryTable(tableOf(h));
    h.settle();
    REQUIRE(h.dispatcher.count("Table.samples") > sent);
    CHECK(lastQuery(h).retry);
}

// Mutation: `WindowRows` not telling of a failed page fetch (no view change follows a failure, so the status line
// keeps its empty state).

TEST_CASE("model table: a failed page fetch is said in the status line", "[ui][table]") {
    TableHarness h;
    auto const screen = mountShown(h);
    CHECK(statusOf(h).state.empty());
    change(h, std::vector<std::int64_t>{});  // the pages again
    h.fail(oldestOpen(h, "Table.samples"));
    CHECK(statusOf(h).state == "The rows could not be loaded: it failed");
    CHECK(statusOf(h).retry);
}

// Mutation: a page refetch per event (the call names no `keys`, or twenty calls go out).  Gate E3 on a screen.

TEST_CASE("model table: an arrival is one keyed request", "[ui][table]") {
    TableHarness h;
    auto const screen = mountShown(h);
    std::size_t const sent = h.dispatcher.count("Table.samples");
    std::vector<std::int64_t> keys(20);
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
    std::iota(keys.begin(), keys.end(), std::int64_t{0});
    change(h, keys);
    REQUIRE(h.dispatcher.count("Table.samples") == sent + 1);  // exactly one more call
    REQUIRE(lastQuery(h).keys.has_value());
    CHECK(lastQuery(h).keys->size() == 20);  // the twenty keys; the run's probe is key 0
}

// Mutation: the mount ignoring unloadedLevels (every cell Loaded).  Cross-part decision 29.

TEST_CASE("model table: a cell of an unloaded level is a placeholder, of a failed one an error mark", "[ui][table]") {
    TableHarness h;
    auto const screen = h.mount(kModelTable);
    replyDescribe(h, withLevel(samplesDescription(3), "c3", "recipe"));  // column 2 has the level
    replyPage(h, pageJsonUnloaded(revisionOf(h), 0, 200, 9093, h.columns,
                                  {{.index = 0, .levels = {"recipe"}, .failed = {}},
                                   {.index = 1, .levels = {"recipe"}, .failed = {"recipe"}}},
                                  /*failed*/ {"recipe"}));
    int const table = tableOf(h);
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{0}}, 2) == CellLoad::Unloaded);
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{0}}, 1) == CellLoad::Loaded);  // name has no level
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{1}}, 2) == CellLoad::Failed);
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{2}}, 2) == CellLoad::Loaded);
    auto const created = statsOf(*screen).cellsCreated;
    change(h, std::vector<std::int64_t>{0});  // the level arrives for row 0
    REQUIRE(lastQuery(h).keys.has_value());
    h.reply(h.dispatcher.last("Table.samples"), keyedJsonAt(revisionOf(h), {{0, 0}}, 9093, h.columns));
    CHECK(h.backend.cellLoad(table, Key{std::int64_t{0}}, 2) == CellLoad::Loaded);
    CHECK(statsOf(*screen).cellsCreated == created);  // the same cell, bound again
}

// Mutations: the miss fetched inside the binding (a signal write is refused there: the debug build aborts);
// `WindowRows::visible` keeping an id the description lacks (the query names "nosuch").

TEST_CASE("model table: a row field the window lacks is fetched once, after the binding", "[ui][table]") {
    // `c7` is described and not laid out, and read out of the whole row with `get`, which no column names; so is
    // `nosuch`, which no column of the description has. Each is its own text, so each read is made.
    std::string const readsAcross =
        withTableKey(kModelTable, R"("columns":[{"id":"id"},{"id":"name","cell":[)"
                                  R"({"kind":"text","text":{"get":{"row":""},"path":"c7"}},)"
                                  R"({"kind":"text","text":{"get":{"row":""},"path":"nosuch"}}]}])");
    TableHarness h;
    auto const screen = h.mount(readsAcross);
    replyDescribe(h, samplesDescription(8));
    std::vector<std::string> const laidOut{"id", "name"};
    h.dispatcher.reply(oldestOpen(h, "Table.samples"),
                       morph::ui::testing::pageJsonAt(revisionOf(h), 0, 200, 9093, laidOut));  // no settle yet
    std::size_t const replied = h.dispatcher.calls.size();
    h.owner.runOne();  // the reply is accepted, cells bind
    h.settle();
    CHECK(h.diagnostics.empty());           // no write refused inside a binding
    std::vector<table::TableQuery> naming;  // the calls since that name `c7`
    for (std::size_t index = replied; index < h.dispatcher.calls.size(); ++index) {
        table::TableQuery const query = queryOf(h, index);
        if (query.columns && std::ranges::contains(*query.columns, "c7")) {
            naming.push_back(query);
        }
    }
    REQUIRE(naming.size() == 1);             // one fetch for every row's miss
    CHECK(naming.front().page.offset == 0);  // the page in view
    CHECK_FALSE(std::ranges::contains(*naming.front().columns, "nosuch"));
}

// Mutations: no hold timer (the message is still there after six seconds); `status: false` still filling
// `message`; an empty text notifying.

TEST_CASE("model table: a message lasts six seconds or until the next gesture", "[ui][table]") {
    SECTION("in the status line") {
        TableHarness h;
        auto screen = mountShown(h);
        morph::ui::detail::ScreenCore& core = coreOf(*screen);
        morph::ui::detail::ElementState& list = elementOf(*screen, "list");
        core.tableMessage(list, "Refused");
        h.settle();
        CHECK(statusOf(h).message == "Refused");
        h.advance(5'999ms);
        CHECK(statusOf(h).message == "Refused");
        h.advance(1ms);
        CHECK(statusOf(h).message.empty());
        core.tableMessage(list, "Again");
        h.settle();
        h.backend.clickHeader(tableOf(h), 1);
        h.settle();
        CHECK(statusOf(h).message.empty());  // the next gesture
        core.tableMessage(list, "");
        h.settle();
        CHECK(h.host.notified.empty());  // notifies nothing
    }
    SECTION("status: false") {
        TableHarness h;
        auto screen = mountShown(h, kNoStatus);
        coreOf(*screen).tableMessage(elementOf(*screen, "list"), "Refused");
        h.settle();
        CHECK(h.host.notified == std::vector<std::pair<std::string, std::string>>{{"Refused", "warning"}});
        CHECK(statusOf(h).message.empty());
        CHECK_FALSE(statusOf(h).shown);
    }
}

// Mutation: the mount leaving a realized row's cells unset on a change that names the row (no cell is bound again:
// `cellBinds` grows by 0).  Gate F6, on the recording backend.

TEST_CASE("model table: an arrival for 50 rows re-evaluates their cells and no other", "[ui][table]") {
    TableHarness h;
    auto const screen = mountShown(h);
    h.backend.scrollTable(tableOf(h), 10, 30);
    h.settle();  // rows 0 to 49 realized
    std::vector<std::int64_t> keys(50);
    // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from libc++.
    std::iota(keys.begin(), keys.end(), std::int64_t{0});
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
    CHECK(statsOf(*screen).cellBinds - before.cellBinds == std::size_t{50} * 3);  // every cell of the 50 rows
    CHECK(statsOf(*screen).rowBinds - before.rowBinds == 50);
    CHECK(statsOf(*screen).cellsCreated == before.cellsCreated);  // nothing rebuilt
    MountStats const again = statsOf(*screen);
    arrive({keys.begin(), keys.begin() + 10});             // the same values for ten rows
    CHECK(statsOf(*screen).cellBinds == again.cellBinds);  // equal records: no bind
    CHECK(statsOf(*screen).rowBinds == again.rowBinds);
}

// Mutations: `table` left out of kTableKeys (the first REQUIRE: "names its `action`"); no catalog check of
// `Table.<id>` (the "nosuch" CHECK loads).

TEST_CASE("ui document: a model-backed table names a table of its model", "[ui][document]") {
    using morph::ui::testing::kModelTable;
    using morph::ui::testing::kNoPersist;
    using morph::ui::testing::kNoStatus;
    using morph::ui::testing::tableCatalog;
    using morph::ui::testing::withTableKey;
    REQUIRE(refusalOf(kModelTable).empty());
    CHECK(loads(kNoPersist, tableCatalog()));
    CHECK(loads(kNoStatus, tableCatalog()));
    CHECK(loads(withTableKey(kModelTable, R"("persist":"wide")"), tableCatalog()));
    CHECK(refusalOf(withTableKey(kModelTable, R"("table":"nosuch")")) == "the table `nosuch` is not in the catalog");
    CHECK(refusalOf(withTableKey(kModelTable, R"("action":"Lims_ListSamples")")) ==
          "a table names a `table` or an `action`, not both");
    CHECK(refusalOf(withTableKey(kModelTable, R"("model":"Lims_SampleModel")")) ==
          "`Lims_SampleModel` is not a model alias of this scope");  // a type is not an alias
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

// Mutation: the default cell's text through `textOf`, which shows "0.125".

TEST_CASE("table display: a model-backed table draws its description's display", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto const screen = h.mount(doc(kTable));
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
    auto const screen = h.mount(doc(kTable));
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
        auto const screen = h.mount(doc(
            R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples","columns":[{"id":"name"}]})"));
        replyDescribe(h, describeJson(companionDescription()));
        CHECK(lastQuery(h).columns == std::vector<std::string>{"id", "name", "lockedBy"});
    }
    {
        TableHarness h;
        auto const screen = h.mount(doc(R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
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
        auto const screen = h.mount(doc(kTable));
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
        auto const screen = h.mount(doc(kTable));
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
                    CHECK(!label.contains(companion));
                }
            }
        }
        // Mutation: the chooser listing every column of the description.
        chooseMenu(h, table, std::nullopt, {"Columns…"});
        for (std::string_view const column : {"id", "mass", "name", "count"}) {
            CHECK(h.backend.find("Checkbox", "testId", "table_columns_show_" + std::string{column}).has_value());
        }
        for (std::string_view const companion : {"massTone", "massTip", "lockedBy"}) {
            CHECK_FALSE(
                h.backend.find("Checkbox", "testId", "table_columns_show_" + std::string{companion}).has_value());
        }
    }
    TableHarness h;  // a companion the document names is reported and not shown
    auto const screen = h.mount(doc(R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
                                  "columns":[{"id":"name"},{"id":"massTip"}]})"));
    replyDescribe(h, describeJson(companionDescription()));
    CHECK(headerLabels(h, "t") == std::vector<std::string>{"Name"});
    CHECK(diagnosticsMentioning(h, "`massTip` is a companion column of `samples`") == 1);
}

// Mutation: document parts read only on a `rows` table, which keeps "0.13".

TEST_CASE("table display: a document's parts override the description", "[ui][table][display]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto const screen = h.mount(doc(R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
                                  "columns":[{"id":"mass","decimals":1,"rounding":"halfEven","empty":"n/a"}]})"));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, companionPage(h));
    CHECK(cellText(h, 1, "mass") == "0.1");
    CHECK(columnOf(h, "mass").unit == "kg");  // a part that gives no `unit` keeps the description's
}
