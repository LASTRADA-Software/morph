// SPDX-License-Identifier: Apache-2.0

// Key changes, retained rows and busy rows.
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

using morph::ui::EngineRows;
using morph::ui::Value;
using morph::ui::testing::describeJson;
using morph::ui::testing::RowsFixture;
using morph::ui::testing::TestRowsSource;
using namespace std::chrono_literals;
using morph::ui::RowMarks;
using morph::ui::TableStatus;
using morph::ui::testing::addAction;
using morph::ui::testing::clipboardText;
using morph::ui::testing::elementOf;
using morph::ui::testing::elementValue;
using morph::ui::testing::lastQuery;
using morph::ui::testing::openCalls;
using morph::ui::testing::pressCopy;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::replyPage;
using morph::ui::testing::statusOf;
using morph::ui::testing::TableHarness;
using morph::ui::testing::value;

// Mutation: copying the held rows only, which writes rows 0 and 2.

TEST_CASE("table rows: a new description revision drops a view selection that names a removed column",
          "[ui][table][selection]") {
    using namespace morph::table;
    TableDescription withC = selectionDescription();
    withC.columns.push_back(TableColumn{.info = {.id = "c", .kind = ColumnKind::Text}, .label = "C"});
    withC = describe(std::move(withC));
    TableDescription const withoutC = selectionDescription();
    auto const run = [&](std::string_view filters, bool dropped) {
        TableHarness h;
        addAction(h, "Lims_TagRows", std::string{kSelectionSchema}, samplesAction());
        std::string const tableNode =
            R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples","selectionMode":"multiple",)"
            R"("filters":)" +
            std::string{filters} + "}";
        auto const screen = h.mount(selectionDoc(tableNode, kSelectionMutations, h));
        replyDescribe(h, describeJson(withC));
        replyPage(h, selectionReply(openRows(), 6, "v1", {}, false, {}, withC));
        int const table = h.byTestId("Table", "t");
        h.backend.selectAll(table);
        h.settle();
        h.backend.ctrlClickRow(table, std::int64_t{3});
        h.settle();
        REQUIRE(elementValue(h, "t", "selectionSet").field("view") != nullptr);
        // A page names revision B; the describe reply B lacks c.
        fireEvent(h, "Lims_SampleModel.samples", {});
        replyPage(h, selectionReply(openRows(), 6, "v1", {}, false, {}, withoutC));
        if (openCalls(h, "Table.samples.describe") > 0) {
            replyDescribe(h, describeJson(withoutC));
        }
        while (openCalls(h, "Table.samples") > 0) {
            replyPage(h, selectionPage(openRows(), 6, "v1"));
        }
        if (dropped) {
            CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[]})"));
            CHECK_FALSE(lastQuery(h).filters.columns.contains("c"));
            runMutation(h, "tag");
            CHECK(!h.dispatcher.calls.at(h.dispatcher.last("Lims_TagRows")).body.contains("\"c\""));
        } else {
            Value const selection = elementValue(h, "t", "selectionSet");
            REQUIRE(selection.field("view") != nullptr);
            CHECK(*selection.field("except") == value("[3]"));
            CHECK(*selection.field("view")->field("filters") ==
                  value(R"({"columns":{"status":{"include":[{"eq":"open"}]}}})"));
        }
    };
    SECTION("a filter on c") {
        run(R"({"columns":{"status":{"include":[{"eq":"open"}]},"c":{"include":[{"eq":"x"}]}}})", true);
    }
    SECTION("a filter on status only") { run(R"({"columns":{"status":{"include":[{"eq":"open"}]}}})", false); }
}

namespace {

// kSelectionTable with `retainChanged`, and the mutation `close`, an action that changes the table's rows.

inline constexpr std::string_view kRetainTable =
    R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples","retainChanged":true,
    "selectionMode":"multiple","filters":{"columns":{"status":{"include":[{"eq":"open"}]}}}})";

inline constexpr std::string_view kCloseMutations = R"({
    "close":{"model":"sample","action":"Lims_CloseRows","body":{"obj":{"rows":{"ref":"t.selectionSet"}}}}})";

// A reply of `Lims_CloseRows` naming @p keys as changed.

std::string closedReply(std::vector<std::int64_t> const& keys) {
    std::string rows;
    for (auto const key : keys) {
        rows += (rows.empty() ? "" : ",") + std::string{R"({"key":)"} + std::to_string(key) + "}";
    }
    return R"({"rows":{"rows":[)" + rows + R"(],"failed":[]}})";
}

// The keys of a list.

std::vector<morph::table::RowId> idsOf(std::initializer_list<std::int64_t> keys) {
    std::vector<morph::table::RowId> out;
    for (auto const key : keys) {
        out.emplace_back(key);
    }
    return out;
}

// The keys @p first to @p first + @p count - 1.

std::vector<std::int64_t> keyRange(std::int64_t first, std::int64_t count) {
    std::vector<std::int64_t> out;
    for (std::int64_t key = first; key < first + count; ++key) {
        out.push_back(key);
    }
    return out;
}

// Mounts kRetainTable (or @p table) with `close`, answering the describe request and the first page.

std::unique_ptr<morph::ui::Screen> mountRetaining(TableHarness& h, std::string_view table = kRetainTable) {
    addAction(h, "Lims_CloseRows", std::string{kSelectionSchema}, samplesAction());
    return mountSelection(h, table, kCloseMutations);
}

}  // namespace

TEST_CASE("retained rows: the rows an action changed stay in view", "[ui][table][retain]") {
    TableHarness h;
    auto screen = mountRetaining(h);
    int const table = h.byTestId("Table", "t");
    runMutation(h, "close");
    settleMutation(h, "close", closedReply({1, 2, 3}));
    REQUIRE(openCalls(h, "Table.samples") == 1);
    CHECK(lastQuery(h).retain == idsOf({1, 2, 3}));
    // The keyed refetch of 1, 2 and 3 (and the probe of the first held row): they no longer match, and stay.
    replyPage(h, selectionReply({{.key = 0},
                                 {.key = 1, .status = "closed"},
                                 {.key = 2, .status = "closed"},
                                 {.key = 3, .status = "closed"}},
                                6, "v2", {1, 2, 3}, true));
    RowMarks const unmatched{.unmatched = true, .tooltip = "This row no longer matches the filter"};
    CHECK(rowMarks(h, std::int64_t{1}) == unmatched);
    CHECK(rowMarks(h, std::int64_t{3}) == unmatched);
    CHECK(rowMarks(h, std::int64_t{4}) == RowMarks{});
    CHECK(elementOf(*screen, "t").tableRows->counts().retained == 3);
    auto const standing = [&h, table] {
        TableStatus const status = statusOf(h, table);
        CHECK(status.standing == "3 rows shown that no longer match");
        REQUIRE(status.standingEntries.size() == 1);
        CHECK(status.standingEntries.front().label == "Show only matching rows");
        return status.standingEntries.front().id;
    };
    standing();
    h.advance(std::chrono::milliseconds{7000});
    h.backend.clickRow(table, std::int64_t{4});
    h.settle();
    standing();
    // A later query still carries them.
    fireEvent(h, "Lims_SampleModel.samples", {4});
    CHECK(lastQuery(h).retain == idsOf({1, 2, 3}));
    // The keyed reply of 4 and the probe 0, at their view rows.
    replyPage(h, R"({"revision":")" + selectionDescription().revision +
                     R"(","columns":["id","status","lockedBy"],"keys":[0,4],"rows":[[0,"open",""],[4,"open",""]],)"
                     R"("total":6,"offset":0,"viewRevision":"v2","indices":[0,4]})");
    CHECK(rowMarks(h, std::int64_t{2}) == unmatched);
    // A selection of the view names the retained rows, and the revision of the view they are in.
    h.backend.selectAll(table);
    h.settle();
    REQUIRE(elementValue(h, "t", "selectionSet").field("view") != nullptr);
    CHECK(*elementValue(h, "t", "selectionSet").field("view")->field("retained") == value("[1,2,3]"));
    CHECK(*elementValue(h, "t", "selectionSet").field("view")->field("revision") == Value{std::string{"v2"}});
    SECTION("Show only matching rows") {
        h.backend.chooseStatusEntry(table, standing());
        h.settle();
        CHECK(lastQuery(h).retain.empty());
        CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[]})"));
        replyPage(h, selectionPage({{.key = 0}, {.key = 4}, {.key = 5}}, 3, "v3"));
        CHECK(statusOf(h, table).standing.empty());
        CHECK(rowMarks(h, std::int64_t{4}) == RowMarks{});
    }
    SECTION("a filter change") {
        REQUIRE(elementOf(*screen, "t").tableRows->setFilter(morph::table::FilterSpec{}));
        h.settle();
        CHECK(lastQuery(h).retain.empty());
        CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[]})"));
        CHECK(statusOf(h, table).standing.empty());
    }
}

// Mutation: retained keys dropped when the next page arrives, which loses them from the second query.

TEST_CASE("retained rows: an action over the bound retains none", "[ui][table][retain]") {
    TableHarness h;
    auto screen = mountRetaining(h);
    int const table = h.byTestId("Table", "t");
    auto const retainedNow = [&screen] { return elementOf(*screen, "t").tableWindow->retained().size(); };
    SECTION("9,999 retained") {
        runMutation(h, "close");
        settleMutation(h, "close", closedReply(keyRange(100, 9'999)));
        REQUIRE(retainedNow() == 9'999);
        runMutation(h, "close");
        settleMutation(h, "close", closedReply({1, 2}));
        CHECK(retainedNow() == 9'999);
        CHECK(statusOf(h, table).message == "The changed rows are not kept in view: a table keeps at most 10,000");
    }
    SECTION("9,998 retained") {
        runMutation(h, "close");
        settleMutation(h, "close", closedReply(keyRange(100, 9'998)));
        REQUIRE(retainedNow() == 9'998);
        runMutation(h, "close");
        settleMutation(h, "close", closedReply({1, 2}));
        CHECK(retainedNow() == 10'000);
        CHECK(statusOf(h, table).message.empty());
    }
}

// Mutation: a partial add up to the bound, which retains one of the two.

TEST_CASE("retained rows: an event before its action's reply is held", "[ui][table][retain]") {
    TableHarness h;
    SECTION("with retainChanged") {
        auto const screen = mountRetaining(h);
        runMutation(h, "close");
        std::size_t const sent = h.dispatcher.count("Table.samples");
        fireEvent(h, "Lims_SampleModel.samples", {1});
        CHECK(h.dispatcher.count("Table.samples") == sent);  // held: no query
        settleMutation(h, "close", closedReply({1}));
        REQUIRE(h.dispatcher.count("Table.samples") == sent + 1);  // the event's and the reply's, as one
        table::TableQuery const refetch = lastQuery(h);
        CHECK(refetch.retain == idsOf({1}));
        REQUIRE(refetch.keys.has_value());
        CHECK(std::ranges::contains(*refetch.keys, table::RowId{std::int64_t{1}}));
        replyPage(h, selectionReply({{.key = 0}, {.key = 1, .status = "closed"}}, 6, "v2", {1}, true));

        // A run whose reply is slower than the hold: the event's query goes when the hold runs out.
        runMutation(h, "close");
        fireEvent(h, "Lims_SampleModel.samples", {2});
        CHECK(h.dispatcher.count("Table.samples") == sent + 1);
        h.advance(std::chrono::milliseconds{1999});
        CHECK(h.dispatcher.count("Table.samples") == sent + 1);
        h.advance(std::chrono::milliseconds{1});
        CHECK(h.dispatcher.count("Table.samples") == sent + 2);
        CHECK(lastQuery(h).retain == idsOf({1}));
        // The reply then sends only its retained rows' refetch.
        settleMutation(h, "close", closedReply({2}));
        CHECK(h.dispatcher.count("Table.samples") == sent + 3);
        CHECK(lastQuery(h).retain == idsOf({1, 2}));
    }
    SECTION("without retainChanged the event's query goes at once") {
        auto const screen = mountRetaining(h, kSelectionTable);
        runMutation(h, "close");
        std::size_t const sent = h.dispatcher.count("Table.samples");
        fireEvent(h, "Lims_SampleModel.samples", {1});
        CHECK(h.dispatcher.count("Table.samples") == sent + 1);
        settleMutation(h, "close", closedReply({1}));
        CHECK(h.dispatcher.count("Table.samples") == sent + 1);
        CHECK(lastQuery(h).retain.empty());
    }
}

// Mutation: no hold, under which row 1 leaves the view and comes back.

TEST_CASE("retained rows: a reply to a query of the old retained set names no revision", "[ui][table][retain]") {
    TableHarness h;
    auto screen = mountRetaining(h);
    int const table = h.byTestId("Table", "t");
    // A page refetch goes with no retained row, and is still in flight when the action's reply retains row 1.
    fireEvent(h, "Lims_SampleModel.samples", {});
    REQUIRE(openCalls(h, "Table.samples") == 1);
    REQUIRE(lastQuery(h).retain.empty());
    runMutation(h, "close");
    settleMutation(h, "close", closedReply({1}));
    REQUIRE(openCalls(h, "Table.samples") == 2);
    CHECK(lastQuery(h).retain == idsOf({1}));
    // The stale page answers: its revision and its marks are of the view without row 1.
    replyPage(h, selectionPage(openRows(), 6, "old", {1}));
    CHECK(elementOf(*screen, "t").tableRows->counts().retained == 0);
    h.backend.selectAll(table);
    h.settle();
    CHECK(statusOf(h, table).message == "Select all is available once the rows have loaded");
    CHECK(elementValue(h, "t", "selectionSet").field("view") == nullptr);
    // The reply to the query that carried row 1 names the view it is in.
    while (openCalls(h, "Table.samples") > 0) {
        replyPage(h, selectionReply({{.key = 0}, {.key = 1, .status = "closed"}}, 6, "new", {1}, true));
    }
    CHECK(rowMarks(h, std::int64_t{1}) ==
          RowMarks{.unmatched = true, .tooltip = "This row no longer matches the filter"});
    h.backend.selectAll(table);
    h.settle();
    Value const selection = elementValue(h, "t", "selectionSet");
    REQUIRE(selection.field("view") != nullptr);
    CHECK(*selection.field("view")->field("revision") == Value{std::string{"new"}});
    CHECK(*selection.field("view")->field("retained") == value("[1]"));
}

// Mutation: the check removed (a reply's revision and marks taken whatever retained set its query carried), which
// selects the view under the revision "old".

TEST_CASE("retained rows: rows over the engine retain too", "[ui][table][retain]") {
    RowsFixture h;
    auto const source = std::make_shared<TestRowsSource>(10);
    EngineRows rows{source, morph::ui::testing::TestRowsSource::recordOf(), table::EngineOptions{.owner = &h.owner}};
    table::FilterSpec only1;
    only1.columns["name"].include.push_back(table::FilterEntry{.eq = "row-1"});
    REQUIRE(rows.setFilter(only1));
    h.settle();
    REQUIRE(rows.total().rows == 1);
    std::vector<table::RowId> const three{std::int64_t{3}};
    CHECK(rows.retain(three) == morph::ui::RetainResult::Retained);
    h.settle();
    CHECK(rows.total().rows == 2);
    CHECK(rows.unmatched(table::RowId{std::int64_t{3}}));
    CHECK_FALSE(rows.unmatched(table::RowId{std::int64_t{1}}));
    CHECK_FALSE(rows.unmatched(table::RowId{std::int64_t{7}}));  // not in the view
    CHECK(rows.counts().retained == 1);
    CHECK(rows.selectView()->retained == three);
    std::vector<table::RowId> over;
    for (std::int64_t key = 100; key < 100 + 10'000; ++key) {
        over.emplace_back(key);
    }
    CHECK(rows.retain(over) == morph::ui::RetainResult::OverBound);
    CHECK(rows.counts().retained == 1);
    REQUIRE(rows.setFilter(table::FilterSpec{}));
    h.settle();
    CHECK(rows.counts().retained == 0);
    CHECK(rows.engine().retained().empty());
}

namespace {

// The table the acting cases mount: the ten rows, unfiltered.

inline constexpr std::string_view kActingTable =
    R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples","selectionMode":"multiple"})";

// The mutations `save`, over the table's selection, and `touch`, over the row the state `picked` names.

inline constexpr std::string_view kActingMutations = R"({
    "save":{"model":"sample","action":"Lims_SaveAs","body":{"obj":{"rows":{"ref":"t.selectionSet"}}}},
    "touch":{"model":"sample","action":"Lims_TouchRow","body":{"obj":{"row":{"ref":"picked"}}}}})";

// The same, with `save` running in parallel with itself.

inline constexpr std::string_view kParallelMutations = R"({
    "save":{"model":"sample","action":"Lims_SaveAs","concurrency":"parallel",
            "body":{"obj":{"rows":{"ref":"t.selectionSet"}}}},
    "touch":{"model":"sample","action":"Lims_TouchRow","body":{"obj":{"row":{"ref":"picked"}}}}})";

inline constexpr std::string_view kLockText = "Being edited by another user";

// The ten rows of selectionDescription(): "open" for 0..5, "closed" for 6..9; row 6 is locked.

std::vector<SelRow> tenRows() {
    std::vector<SelRow> rows;
    for (std::int64_t key = 0; key < 10; ++key) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `rows` is a fixture list built once; sizing it first would not change what the case checks
        rows.push_back(SelRow{.key = key,
                              .status = key < 6 ? "open" : "closed",
                              .lockedBy = key == 6 ? std::string{kLockText} : std::string{}});
    }
    return rows;
}

// tenRows() without @p gone, and with @p added last.

std::vector<SelRow> tenRowsMoved(std::int64_t gone, std::optional<std::int64_t> added) {
    std::vector<SelRow> rows = tenRows();
    std::erase_if(rows, [gone](SelRow const& row) { return row.key == gone; });
    if (added) {
        rows.push_back(SelRow{.key = *added});
    }
    return rows;
}

// A reply of selectionDescription() naming each row at the view row given: keyed (with `indices`), or a page from
// @p offset.

std::string keyedAt(std::vector<std::pair<std::int64_t, SelRow>> const& placed, std::int64_t total,
                    std::string_view viewRevision, std::optional<std::int64_t> offset = std::nullopt) {
    std::string keys;
    std::string cells;
    std::string indices;
    for (auto const& [at, row] : placed) {
        std::string const comma = keys.empty() ? "" : ",";
        keys += comma + std::to_string(row.key);
        cells += comma + "[" + std::to_string(row.key) + ",\"" + row.status + "\",\"" + row.lockedBy + "\"]";
        indices += comma + std::to_string(at);
    }
    return R"({"revision":")" + selectionDescription().revision +
           R"(","columns":["id","status","lockedBy"],"keys":[)" + keys + R"(],"rows":[)" + cells + R"(],"total":)" +
           std::to_string(total) + R"(,"offset":)" + std::to_string(offset.value_or(0)) + R"(,"viewRevision":")" +
           std::string{viewRevision} + "\"" + (offset ? std::string{} : R"(,"indices":[)" + indices + "]") + "}";
}

// The document: @p table, a button `run-<name>` for each mutation, and buttons `pick-<key>` setting `picked`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `actingDoc(h, table, mutations)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::string actingDoc(TableHarness& h, std::string_view table, std::string_view mutations) {
    h.mutationActions.insert_or_assign("save", "Lims_SaveAs");
    h.mutationActions.insert_or_assign("touch", "Lims_TouchRow");
    std::string children{table};
    for (std::string const name : {"save", "touch"}) {
        // NOLINTBEGIN(performance-inefficient-string-concatenation): builds the case's small document text once; the temporaries cost nothing next to the mount it feeds
        children += R"(,{"kind":"button","testId":"run-)" + name + R"(","label":")" + name +
                    R"(","onClick":[{"run":")" + name + R"("}]})";
        // NOLINTEND(performance-inefficient-string-concatenation)
    }
    for (int const key : {2, 4, 5}) {
        std::string const text = std::to_string(key);
        // NOLINTBEGIN(performance-inefficient-string-concatenation): builds the case's small document text once; the temporaries cost nothing next to the mount it feeds
        children += R"(,{"kind":"button","testId":"pick-)" + text + R"(","label":"pick )" + text +
                    R"(","onClick":[{"set":"picked","value":{"lit":)" + text + "}}]}";
        // NOLINTEND(performance-inefficient-string-concatenation)
    }
    return R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
           R"("state":{"picked":{"init":{"lit":null}}},"mutations":)" +
           std::string{mutations} + R"(,"view":{"kind":"column","children":[)" + children + "]}}";
}

// Mounts @p table with `save` and `touch`, answering the describe request and a first page of the ten rows (or of
// @p first, total and revision "v1").

std::unique_ptr<morph::ui::Screen> mountActing(TableHarness& h, std::string_view table = kActingTable,
                                               std::string_view mutations = kActingMutations,
                                               std::vector<SelRow> const& first = tenRows()) {
    addAction(h, "Lims_SaveAs", std::string{kSelectionSchema}, samplesAction());
    addAction(h, "Lims_TouchRow", std::string{kRowSchema}, samplesAction());
    auto screen = h.mount(actingDoc(h, table, mutations));
    replyDescribe(h, describeJson(selectionDescription()));
    replyPage(h, selectionPage(first, static_cast<std::int64_t>(first.size()), "v1"));
    return screen;
}

// Sets `picked` to @p key through its button.

void pick(TableHarness& h, int key) {
    h.backend.click(h.byTestId("Button", "pick-" + std::to_string(key)));
    h.settle();
}

// Moves focus to view row @p row from the first.

void focusRow(TableHarness& h, int table, std::size_t row) {
    h.backend.focusMove(table, morph::ui::FocusMove::First);
    for (std::size_t at = 0; at < row; ++at) {
        h.backend.focusMove(table, morph::ui::FocusMove::Down);
    }
    h.settle();
    REQUIRE(h.backend.focusCell(table).first == row);
}

// The key of the row the mount's focus is on, as the backend was last told it.

std::optional<morph::table::RowId> focusedKey(morph::ui::Screen& screen, TableHarness& h, int table) {
    auto const at = static_cast<std::int64_t>(h.backend.focusCell(table).first);
    auto const row = elementOf(screen, "t").tableRows->row(at);
    return row ? std::optional{row->key} : std::nullopt;
}

// Answers every open query of the table from the view @p view: a keyed one with the rows it names at their view rows,
// a page one with every row.

void answerView(TableHarness& h, std::vector<SelRow> const& view, std::string_view revision = "v2") {
    auto const total = static_cast<std::int64_t>(view.size());
    for (int round = 0; round < 8 && openCalls(h, "Table.samples") > 0; ++round) {
        std::size_t const index = morph::ui::testing::oldestOpen(h, "Table.samples");
        table::TableQuery const query = morph::ui::testing::queryOf(h, index);
        if (!query.keys) {
            std::vector<std::pair<std::int64_t, SelRow>> page;
            for (std::int64_t at = query.page.offset; at < std::min(total, query.page.offset + query.page.limit);
                 ++at) {
                page.emplace_back(at, view.at(static_cast<std::size_t>(at)));
            }
            h.reply(index, keyedAt(page, total, revision, query.page.offset));
            continue;
        }
        std::vector<std::pair<std::int64_t, SelRow>> placed;
        for (std::size_t at = 0; at < view.size(); ++at) {
            if (std::ranges::contains(*query.keys, table::RowId{view.at(at).key})) {
                placed.emplace_back(static_cast<std::int64_t>(at), view.at(at));
            }
        }
        h.reply(index, keyedAt(placed, total, revision));
    }
}

// Answers the describe request and the open queries from @p view, of revision "v1".

std::unique_ptr<morph::ui::Screen> mountActingOver(TableHarness& h, std::vector<SelRow> const& view) {
    addAction(h, "Lims_SaveAs", std::string{kSelectionSchema}, samplesAction());
    addAction(h, "Lims_TouchRow", std::string{kRowSchema}, samplesAction());
    auto screen = h.mount(actingDoc(h, kActingTable, kActingMutations));
    replyDescribe(h, describeJson(selectionDescription()));
    answerView(h, view, "v1");
    return screen;
}

// The JSON of a ChangedRows reply under `rows`.

std::string savedReply(std::string_view rows, std::string_view failed = "") {
    return R"({"rows":{"rows":[)" + std::string{rows} + R"(],"failed":[)" + std::string{failed} + "]}}";
}

morph::table::RowId rid(std::int64_t key) { return morph::table::RowId{key}; }

// The view index of each key in tenRows().

std::int64_t indexOf(std::int64_t key) { return key; }

}  // namespace

TEST_CASE("key change: the session that ran the action moves what it holds", "[ui][table][keys]") {
    TableHarness h;
    auto screen = mountActing(h);
    int const table = h.byTestId("Table", "t");
    morph::ui::TableRows& rows = *elementOf(*screen, "t").tableRows;
    // Rows 2 and 5 selected (keys), focus on row 2, row 2 retained.
    h.backend.clickRow(table, std::int64_t{2});
    h.backend.ctrlClickRow(table, std::int64_t{5});
    h.settle();
    focusRow(h, table, 2);
    REQUIRE(elementValue(h, "t", "selectionSet") == value(R"({"keys":[2,5]})"));
    REQUIRE(rows.retain(idsOf({2})) == morph::ui::RetainResult::Retained);
    h.settle();
    answerView(h, tenRows(), "v1");
    // Row 2 failed from an earlier run: `touch` on row 2 started before `save`, and fails it while `save` runs.
    pick(h, 2);
    runMutation(h, "touch");
    runMutation(h, "save");
    settleMutation(h, "touch", savedReply("", R"({"key":2,"message":"Locked"})"));
    REQUIRE(rows.states().failure(rid(2)) == std::optional<std::string>{"Locked"});
    std::size_t const told = h.backend.focusCells(table).size();

    settleMutation(h, "save", savedReply(R"({"key":10,"from":2})"));
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[10,5]})"));
    CHECK(rows.states().failure(rid(10)) == std::optional<std::string>{"Locked"});
    CHECK_FALSE(rows.states().failure(rid(2)).has_value());
    auto const retained = elementOf(*screen, "t").tableWindow->retained();
    CHECK(std::ranges::contains(retained, rid(10)));
    CHECK_FALSE(std::ranges::contains(retained, rid(2)));

    std::size_t const sent = h.dispatcher.count("Table.samples");
    fireEvent(h, "Lims_SampleModel.samples", {10, 2});
    REQUIRE(h.dispatcher.count("Table.samples") == sent + 1);
    REQUIRE(lastQuery(h).keys.has_value());
    CHECK(std::ranges::contains(*lastQuery(h).keys, rid(10)));
    CHECK(std::ranges::contains(*lastQuery(h).keys, rid(2)));
    // Row 2 is gone and row 10 is last.
    answerView(h, tenRowsMoved(2, 10));
    CHECK(focusedKey(*screen, h, table) == std::optional{rid(10)});
    std::vector<std::pair<std::size_t, std::size_t>> const cells = h.backend.focusCells(table);
    REQUIRE(cells.size() > told);
    std::vector<std::optional<morph::table::RowId>> const named = h.backend.focusKeys(table);
    for (std::size_t at = told; at < cells.size(); ++at) {
        CHECK(cells.at(at).first == 9);                 // key 10's view row; no other row was focused meanwhile
        CHECK(named.at(at) == std::optional{rid(10)});  // and the row there held key 10 when it was told
    }
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[10,5]})"));
    // The mount's own focus is on key 10's row: a move up lands on key 9's.
    h.backend.focusMove(table, morph::ui::FocusMove::Up);
    h.settle();
    CHECK(focusedKey(*screen, h, table) == std::optional{rid(9)});
}

// Mutations: the client ignoring `from`, which leaves 2 selected and 10 not; a located index told while the rows held
// another key there, which tells key 9's row and leaves the mount on it.

TEST_CASE("key change: the action's event before its reply", "[ui][table][keys]") {
    TableHarness h;
    auto screen = mountActing(h);
    int const table = h.byTestId("Table", "t");
    h.backend.clickRow(table, std::int64_t{2});
    h.backend.ctrlClickRow(table, std::int64_t{5});
    h.settle();
    focusRow(h, table, 2);
    runMutation(h, "save");
    fireEvent(h, "Lims_SampleModel.samples", {10, 2});
    answerView(h, tenRowsMoved(2, 10));
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[2,5]})"));
    SECTION("the reply names 2 as 10's old key") {
        settleMutation(h, "save", savedReply(R"({"key":10,"from":2})"));
        CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[10,5]})"));
        CHECK(focusedKey(*screen, h, table) == std::optional{rid(10)});
    }
    SECTION("the reply names no from") {
        settleMutation(h, "save", savedReply(R"({"key":10})"));
        CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[5]})"));
        CHECK(h.backend.focusCell(table).first == 2);
        CHECK(focusedKey(*screen, h, table) == std::optional{rid(3)});
    }
}

// Mutation: no deferral while the run is in flight, which drops 2 before `from` names it and selects [5].

TEST_CASE("key change: onto a key already held", "[ui][table][keys][review]") {
    TableHarness h;
    auto const screen = mountActing(h);
    int const table = h.byTestId("Table", "t");
    // Rows 3 and 7 held; 7 and 3 selected.
    h.backend.clickRow(table, std::int64_t{7});
    h.backend.ctrlClickRow(table, std::int64_t{3});
    h.settle();
    REQUIRE(elementValue(h, "t", "selectionSet") == value(R"({"keys":[7,3]})"));
    runMutation(h, "save");
    settleMutation(h, "save", savedReply(R"({"key":7,"from":3})"));
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[7]})"));
    fireEvent(h, "Lims_SampleModel.samples", {7, 3});
    REQUIRE(lastQuery(h).keys.has_value());
    CHECK(std::ranges::contains(*lastQuery(h).keys, rid(3)));
    CHECK(std::ranges::contains(*lastQuery(h).keys, rid(7)));
}

// Mutation: a move that appends `to` without checking it is held, which selects 7 twice.

TEST_CASE("key change: a from the client does not hold", "[ui][table][keys]") {
    TableHarness h;
    auto screen = mountActing(h);
    int const table = h.byTestId("Table", "t");
    morph::ui::TableRows& rows = *elementOf(*screen, "t").tableRows;
    h.backend.clickRow(table, std::int64_t{2});
    h.settle();
    focusRow(h, table, 4);
    std::size_t const told = h.backend.focusCells(table).size();
    runMutation(h, "save");
    settleMutation(h, "save", savedReply(R"({"key":10,"from":99})"));
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[2]})"));
    CHECK(h.backend.focusCells(table).size() == told);
    CHECK(h.backend.focusCell(table).first == 4);
    CHECK_FALSE(rows.states().isBusy(rid(10)));
    CHECK_FALSE(rows.states().failure(rid(10)).has_value());
    CHECK(elementOf(*screen, "t").tableWindow->retained().empty());
    fireEvent(h, "Lims_SampleModel.samples", {10, 99});
    REQUIRE(lastQuery(h).keys.has_value());
    CHECK(std::ranges::contains(*lastQuery(h).keys, rid(10)));
}

// Mutation: a move that selects `to` whatever `from` was, which adds 10 to the selection.

TEST_CASE("key change: another session's event drops the old key", "[ui][table][keys]") {
    TableHarness h;
    auto screen = mountActing(h);
    int const table = h.byTestId("Table", "t");
    // Row 2 selected and focused.
    h.backend.clickRow(table, std::int64_t{2});
    h.settle();
    focusRow(h, table, 2);
    fireEvent(h, "Lims_SampleModel.samples", {10, 2});
    answerView(h, tenRowsMoved(2, 10));
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[]})"));
    CHECK(h.backend.focusCell(table).first == 2);
    CHECK(focusedKey(*screen, h, table) == std::optional{rid(3)});
}

// Mutation: a selection kept for keys that left the view, which keeps 2.

TEST_CASE("busy rows: a run's rows are busy until it settles", "[ui][table][busy]") {
    TableHarness h;
    morph::ui::RowMarks const busy{.busy = true};
    SECTION("a selection of keys, then a row") {
        auto screen = mountActing(h);
        int const table = h.byTestId("Table", "t");
        morph::ui::TableRows const& rows = *elementOf(*screen, "t").tableRows;
        table::TableDescription const& description = *elementOf(*screen, "t").tableWindow->description();
        h.backend.clickRow(table, std::int64_t{1});
        h.backend.ctrlClickRow(table, std::int64_t{2});
        h.settle();
        runMutation(h, "save");
        CHECK(rowMarks(h, std::int64_t{1}) == busy);
        CHECK(rowMarks(h, std::int64_t{2}) == busy);
        CHECK(rowMarks(h, std::int64_t{3}) == morph::ui::RowMarks{});
        CHECK(elementValue(h, "t", "busyCount") == Value{std::int64_t{2}});
        CHECK(statusOf(h, table).standing == "Saving 2 rows");
        CHECK(morph::ui::refusesEdit(rows, indexOf(1), description));
        CHECK(morph::ui::refusesEdit(rows, indexOf(2), description));
        CHECK_FALSE(morph::ui::refusesEdit(rows, indexOf(3), description));
        refuseMutation(h, "save", "lims.rows.denied");
        CHECK(rowMarks(h, std::int64_t{1}) == morph::ui::RowMarks{});
        CHECK(rowMarks(h, std::int64_t{2}) == morph::ui::RowMarks{});
        CHECK(elementValue(h, "t", "busyCount") == Value{std::int64_t{0}});
        CHECK(statusOf(h, table).standing.empty());
        CHECK_FALSE(morph::ui::refusesEdit(rows, indexOf(1), description));
        pick(h, 4);
        runMutation(h, "touch");
        CHECK(rowMarks(h, std::int64_t{4}) == busy);
        CHECK(elementValue(h, "t", "busyCount") == Value{std::int64_t{1}});
        CHECK(statusOf(h, table).standing == "Saving 1 row");
    }
    SECTION("a selection of the view") {
        // A view selection of 6 rows less 3 held as except.
        auto const screen = mountActing(h, kSelectionTable, kActingMutations, openRows());
        int const table = h.byTestId("Table", "t");
        h.backend.selectAll(table);
        h.settle();
        h.backend.ctrlClickRow(table, std::int64_t{3});
        h.settle();
        runMutation(h, "save");
        for (std::int64_t const key : {0, 1, 2, 4, 5}) {
            CHECK(rowMarks(h, key) == busy);
        }
        CHECK(rowMarks(h, std::int64_t{3}) == morph::ui::RowMarks{});
        CHECK(elementValue(h, "t", "busyCount") == Value{std::int64_t{5}});
    }
}

// Mutation: busy state cleared only on success, which leaves rows 1 and 2 busy after the refusal.

TEST_CASE("busy rows: overlapping runs", "[ui][table][busy][review]") {
    TableHarness h;
    auto screen = mountActing(h, kActingTable, kParallelMutations);
    int const table = h.byTestId("Table", "t");
    morph::ui::RowStates const& states = elementOf(*screen, "t").tableRows->states();
    // Run A over keys {1, 2}, run B over {2, 3}.
    h.backend.clickRow(table, std::int64_t{1});
    h.backend.ctrlClickRow(table, std::int64_t{2});
    h.settle();
    runMutation(h, "save");
    std::size_t const a = mutationCall(h, "save");
    h.backend.clickRow(table, std::int64_t{2});
    h.backend.ctrlClickRow(table, std::int64_t{3});
    h.settle();
    runMutation(h, "save");
    std::size_t const b = mutationCall(h, "save");
    REQUIRE(a != b);
    CHECK(elementValue(h, "t", "busyCount") == Value{std::int64_t{3}});
    h.reply(a, savedReply(R"({"key":1},{"key":2})"));
    CHECK(states.isBusy(rid(2)));
    CHECK_FALSE(states.isBusy(rid(1)));
    CHECK(rowMarks(h, std::int64_t{2}) == morph::ui::RowMarks{.busy = true});
    CHECK(elementValue(h, "t", "busyCount") == Value{std::int64_t{2}});
    h.reply(b, savedReply(R"({"key":2},{"key":3})"));
    CHECK(elementValue(h, "t", "busyCount") == Value{std::int64_t{0}});
    CHECK_FALSE(states.isBusy(rid(2)));
}

// Mutation: a busy set keyed by row only, which frees row 2 when A settles.

TEST_CASE("busy rows: two mutations' runs do not share an id", "[ui][table][busy]") {
    TableHarness h;
    auto screen = mountActing(h);
    int const table = h.byTestId("Table", "t");
    morph::ui::RowStates const& states = elementOf(*screen, "t").tableRows->states();
    // Mutations `save` and `touch`, each running for the first time: save over {1, 2}, touch {"row": 5}.
    h.backend.clickRow(table, std::int64_t{1});
    h.backend.ctrlClickRow(table, std::int64_t{2});
    h.settle();
    runMutation(h, "save");
    pick(h, 5);
    runMutation(h, "touch");
    CHECK(elementValue(h, "t", "busyCount") == Value{std::int64_t{3}});
    settleMutation(h, "touch", savedReply(R"({"key":5})"));
    CHECK(states.isBusy(rid(1)));
    CHECK(states.isBusy(rid(2)));
    CHECK_FALSE(states.isBusy(rid(5)));
    CHECK(elementValue(h, "t", "busyCount") == Value{std::int64_t{2}});
    settleMutation(h, "save", savedReply(R"({"key":1},{"key":2})"));
    CHECK(elementValue(h, "t", "busyCount") == Value{std::int64_t{0}});
}

// Mutation: run ids taken from MutationRuntime's own counter, under which settling touch (run 1) frees save's rows.

TEST_CASE("failed rows: marked until the row's next change", "[ui][table][failed]") {
    TableHarness h;
    auto screen = mountActing(h);
    int const table = h.byTestId("Table", "t");
    morph::ui::RowStates const& states = elementOf(*screen, "t").tableRows->states();
    std::string const failedReply = savedReply(R"({"key":5})", R"({"key":4,"message":"Locked"})");
    auto const failAgain = [&] {
        h.backend.clickRow(table, std::int64_t{4});
        h.settle();
        runMutation(h, "save");
        settleMutation(h, "save", failedReply);
        REQUIRE(states.failure(rid(4)) == std::optional<std::string>{"Locked"});
    };
    // run save over {4, 5}.
    h.backend.clickRow(table, std::int64_t{4});
    h.backend.ctrlClickRow(table, std::int64_t{5});
    h.settle();
    runMutation(h, "save");
    settleMutation(h, "save", failedReply);
    CHECK(rowMarks(h, std::int64_t{4}) == morph::ui::RowMarks{.tooltip = "Locked"});
    CHECK(h.backend.rowTone(table, std::int64_t{4}) == "Err");
    CHECK(h.backend.rowTone(table, std::int64_t{5}) == "none");
    TableStatus const status = statusOf(h, table);
    CHECK(status.standing == "1 row could not be saved");
    REQUIRE(status.standingEntries.size() == 2);
    CHECK(status.standingEntries.at(0).label == "Select them");
    CHECK(status.standingEntries.at(1).label == "Clear marks");
    // "Select them".
    h.backend.chooseStatusEntry(table, status.standingEntries.at(0).id);
    h.settle();
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[4]})"));
    // An event naming 5, the action's own: 4 stays marked. One naming 4 after the run settled: the mark goes.
    fireEvent(h, "Lims_SampleModel.samples", {5});
    CHECK(states.failure(rid(4)) == std::optional<std::string>{"Locked"});
    CHECK(rowMarks(h, std::int64_t{4}) == morph::ui::RowMarks{.tooltip = "Locked"});
    fireEvent(h, "Lims_SampleModel.samples", {4});
    CHECK_FALSE(states.failure(rid(4)).has_value());
    CHECK(rowMarks(h, std::int64_t{4}) == morph::ui::RowMarks{});
    CHECK(h.backend.rowTone(table, std::int64_t{4}) == "none");
    CHECK(statusOf(h, table).standing.empty());
    // Again failed; "Clear marks": the mark goes.
    failAgain();
    h.backend.chooseStatusEntry(table, statusOf(h, table).standingEntries.at(1).id);
    h.settle();
    CHECK_FALSE(states.failure(rid(4)).has_value());
    CHECK(rowMarks(h, std::int64_t{4}) == morph::ui::RowMarks{});
    // Again failed; a new run naming row 4: the mark goes when the run starts.
    failAgain();
    runMutation(h, "save");
    CHECK_FALSE(states.failure(rid(4)).has_value());
    settleMutation(h, "save", savedReply(R"({"key":4})"));
    // Again failed; an event with no keys, as a row-scoped table's are, leaves the mark.
    failAgain();
    fireEvent(h, "Lims_SampleModel.samples", {});
    CHECK(states.failure(rid(4)) == std::optional<std::string>{"Locked"});
}

// Mutation: a failed mark cleared by any event, which clears it on the action's own event.

TEST_CASE("locked rows: a locked row draws locked and refuses an edit", "[ui][table][locked]") {
    TableHarness h;
    auto screen = mountActing(h);
    int const table = h.byTestId("Table", "t");
    morph::ui::TableRows const& rows = *elementOf(*screen, "t").tableRows;
    table::TableDescription const& description = *elementOf(*screen, "t").tableWindow->description();
    // Row 6's lockedBy is "Being edited by another user".
    CHECK(rowMarks(h, std::int64_t{6}) == morph::ui::RowMarks{.locked = true, .tooltip = std::string{kLockText}});
    std::optional<morph::ui::TableRowRef> const six = rows.row(indexOf(6));
    REQUIRE(six.has_value());
    CHECK(morph::ui::lockText(*six, description) == std::optional<std::string>{std::string{kLockText}});
    CHECK(morph::ui::refusesEdit(rows, indexOf(6), description));
    // It can be selected and copied.
    h.backend.clickRow(table, std::int64_t{6});
    h.settle();
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[6]})"));
    pressCopy(h, table);
    CHECK(clipboardText(h) == "6\tclosed");
    // Row 5's lockedBy is "": not locked.
    CHECK(rowMarks(h, std::int64_t{5}) == morph::ui::RowMarks{});
    std::optional<morph::ui::TableRowRef> const five = rows.row(indexOf(5));
    REQUIRE(five.has_value());
    CHECK_FALSE(morph::ui::lockText(*five, description).has_value());
    CHECK_FALSE(morph::ui::refusesEdit(rows, indexOf(5), description));
}

// Mutation: lockText reading `rowTone`'s column, which locks no row.

TEST_CASE("row states: a move keeps the new key's own state", "[ui][table][keys]") {
    morph::ui::RowStates states;
    states.busyKeys(1, idsOf({2, 7}));
    states.busyView(2, idsOf({2}), 9);
    states.fail(
        std::vector<morph::table::FailedRow>{{.key = rid(2), .message = "two"}, {.key = rid(7), .message = "seven"}});
    states.move(rid(2), rid(7));
    CHECK(states.failure(rid(7)) == std::optional<std::string>{"seven"});
    CHECK_FALSE(states.failure(rid(2)).has_value());
    CHECK(states.failedKeys() == idsOf({7}));
    CHECK(states.isBusy(rid(7)));
    CHECK_FALSE(states.isBusy(rid(2)));
    states.settle(1);
    states.settle(2);
    CHECK_FALSE(states.isBusy(rid(7)));
    CHECK(states.busyCount() == 0);
}

// Mutation: a move that overwrites the new key's failed mark with the old key's, which leaves "two" on 7.

TEST_CASE("key change: focus alone follows a from whose event came first", "[ui][table][keys][review]") {
    TableHarness h;
    auto screen = mountActing(h);
    int const table = h.byTestId("Table", "t");
    SECTION("nothing selected") {}
    SECTION("a selection of the view, which leaves 2 in") {
        h.backend.selectAll(table);
        h.settle();
        REQUIRE(elementValue(h, "t", "selectionSet").field("view") != nullptr);
    }
    // Focus on row 2, which no selected key, retained row or failed mark holds.
    focusRow(h, table, 2);
    runMutation(h, "save");
    fireEvent(h, "Lims_SampleModel.samples", {10, 2});
    answerView(h, tenRowsMoved(2, 10));
    REQUIRE(focusedKey(*screen, h, table) == std::optional{rid(3)});  // the row now at 2's index took focus
    settleMutation(h, "save", savedReply(R"({"key":10,"from":2})"));
    answerView(h, tenRowsMoved(2, 10));
    CHECK(h.backend.focusCell(table).first == 9);
    CHECK(focusedKey(*screen, h, table) == std::optional{rid(10)});
}

// Mutation: moves told to the mount only for a `from` the lowering holds, which leaves focus on key 3.

TEST_CASE("key change: focus follows a new key outside the rows in view", "[ui][table][keys][review]") {
    TableHarness h;
    std::vector<SelRow> rows = tenRows();
    for (std::int64_t key = 10; key < 300; ++key) {
        rows.push_back(SelRow{.key = key});
    }
    auto screen = mountActingOver(h, rows);
    int const table = h.byTestId("Table", "t");
    focusRow(h, table, 2);
    runMutation(h, "save");
    settleMutation(h, "save", savedReply(R"({"key":1000,"from":2})"));
    // Row 2 is gone and row 1000 is the view's last, far from the rows in view.
    std::vector<SelRow> after = rows;
    std::erase_if(after, [](SelRow const& row) { return row.key == 2; });
    after.push_back(SelRow{.key = 1000});
    std::size_t const told = h.backend.focusKeys(table).size();
    fireEvent(h, "Lims_SampleModel.samples", {1000, 2});
    answerView(h, after);
    CHECK(h.backend.focusCell(table).first == 299);
    CHECK(focusedKey(*screen, h, table) == std::optional{rid(1000)});
    // No focus told named a row that held another key when it was told.
    std::vector<std::optional<morph::table::RowId>> const named = h.backend.focusKeys(table);
    for (std::size_t at = told; at < named.size(); ++at) {
        CHECK((!named.at(at) || *named.at(at) == rid(1000)));
    }
    // The mount's own focus is on key 1000's row: a move up lands on key 299's.
    h.backend.focusMove(table, morph::ui::FocusMove::Up);
    h.settle();
    CHECK(h.backend.focusCell(table).first == 298);
    CHECK(focusedKey(*screen, h, table) == std::optional{rid(299)});
}

// NOLINTBEGIN(misc-use-internal-linkage)
namespace edit_rows {

// A model whose table `specimens` the edit actions below name: its record is `T_EditRows.specimens`.

struct EditRowsModel {
    morph::table::ServedTable specimens(morph::table::TableContext const& context) {
        static_cast<void>(context);
        return {};
    }
};

}  // namespace edit_rows

// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(edit_rows::EditRowsModel, "T_EditRows")
BRIDGE_REGISTER_TABLE(edit_rows::EditRowsModel, "specimens", &edit_rows::EditRowsModel::specimens)

namespace edit_rows {

using SpecimensModel = EditRowsModel;
struct EditSpecimenCount : morph::table::CellEdit<&SpecimensModel::specimens, std::int64_t> {};
struct SpecimenValues {
    std::optional<std::string> label;
    std::optional<std::string> remark;
};
struct EditSpecimenRow : morph::table::RowEdit<&SpecimensModel::specimens, SpecimenValues> {};

}  // namespace edit_rows

// Mutation: annotateSchemaRoot left out of mergeSchemaExtras (no root keyword; tableInputsOf finds no input).

TEST_CASE("cell edits: an edit action reads, writes and names its table at the schema root", "[ui][table][edit]") {
    using edit_rows::EditSpecimenCount;
    using edit_rows::EditSpecimenRow;
    static_assert(glz::reflect<EditSpecimenCount>::size == 2);
    EditSpecimenCount edit{};
    REQUIRE_FALSE(glz::read_json(edit, R"({"key":4,"value":9})"));
    CHECK(edit.value == 9);
    CHECK(edit.key == table::RowId{std::int64_t{4}});
    CHECK(glz::write_json(edit).value_or("") == R"({"key":4,"value":9})");
    auto dom = glz::read_json<glz::generic_u64>(morph::forms::schemaJson<EditSpecimenCount>()).value();
    CHECK(glz::write_json(dom["x-cellEditOf"]).value_or("") == R"("T_EditRows.specimens")");
    CHECK_FALSE(dom["properties"]["key"].contains("x-cellEditOf"));
    auto const inputs = morph::ui::tableInputsOf(morph::forms::schemaJson<EditSpecimenRow>());
    REQUIRE(inputs.size() == 1);
    CHECK(inputs.at(0).member == "key");
    CHECK(inputs.at(0).record == "T_EditRows.specimens");
    CHECK(inputs.at(0).kind == morph::ui::TableInputKind::CellEdit);
}
