// SPDX-License-Identifier: Apache-2.0

// Tables over an action's rows, their filters and sort chains.
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

using morph::ui::FormatContext;
using morph::ui::Value;
using morph::ui::WindowRows;
using morph::ui::testing::formatFor;
using morph::ui::testing::RowsFixture;
using morph::ui::testing::samplesDescription;
using namespace std::chrono_literals;
using morph::ui::ActionInfo;
using morph::ui::HeaderClick;
using morph::ui::MountStats;
using morph::ui::Refusal;
using morph::ui::ValueList;
using morph::ui::testing::addAction;
using morph::ui::testing::addPageSamples;
using morph::ui::testing::cellOf;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::diagnosticsMentioning;
using morph::ui::testing::elementOf;
using morph::ui::testing::elementValue;
using morph::ui::testing::engineOf;
using morph::ui::testing::entryOf;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::headerLabels;
using morph::ui::testing::idRows;
using morph::ui::testing::kActionTable;
using morph::ui::testing::kActionTableNoColumns;
using morph::ui::testing::kActionTableNoQuery;
using morph::ui::testing::kConfigModelTable;
using morph::ui::testing::kindOfColumn;
using morph::ui::testing::kNine;
using morph::ui::testing::kThree;
using morph::ui::testing::labelsOf;
using morph::ui::testing::lastQuery;
using morph::ui::testing::loads;
using morph::ui::testing::menuOf;
using morph::ui::testing::moveColumn;
using morph::ui::testing::readJson;
using morph::ui::testing::refusalOf;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::resizeColumn;
using morph::ui::testing::rowOrder;
using morph::ui::testing::rowsDoc;
using morph::ui::testing::rowsDocNoId;
using morph::ui::testing::shownText;
using morph::ui::testing::statsOf;
using morph::ui::testing::statusOf;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;
using morph::ui::testing::tableOf;
using morph::ui::testing::unitOf;
using morph::ui::testing::windowOf;
using morph::ui::testing::withTableKey;

// Mutations: no report of an invalid sort cell (the count is 0); one per view change (two, once a second column's
// invalid cell words the report apart).

TEST_CASE("rows table: a cell not of its column's kind is reported once at the sort path", "[ui][table]") {
    TableHarness h;
    auto const screen =
        h.mount(rowsDoc(R"([{"id":"a","weight":1},{"id":"b","weight":"heavy"},{"id":"c","weight":2}])"));
    auto const sortReports = [&h] {
        return std::ranges::count_if(h.diagnostics, [](Refusal const& r) { return r.path.ends_with("/sort"); });
    };
    h.backend.clickHeader(tableOf(h), 2);
    h.settle();
    CHECK(rowOrder(h) == "a c b");  // the invalid cell sorts last
    CHECK(sortReports() == 1);
    h.backend.clickHeader(tableOf(h), 2);
    h.settle();
    CHECK(rowOrder(h) == "c a b");
    CHECK(sortReports() == 1);  // last in either direction, once
    TableHarness two;
    auto const both = two.mount(rowsDoc(R"([{"id":"a","name":"x","weight":1},{"id":"b","name":2,"weight":"heavy"},)"
                                        R"({"id":"c","name":"y","weight":2}])"));
    two.backend.clickHeader(tableOf(two), 2);
    two.settle();
    two.backend.clickHeader(tableOf(two), 1);
    two.settle();  // another column, another invalid cell
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
    TableHarness h;
    auto const screen = h.mount(kGroupedRows);
    h.backend.clickHeader(tableOf(h), 0);
    h.settle();
    REQUIRE(headingTexts(h) == std::vector<std::string>{"2", "1"});
    REQUIRE(shownText(h, "one"));
    h.backend.click(h.byTestId("Button", "note"));
    h.settle();  // the runs stay: an `Updated` row
    CHECK(shownText(h, "uno"));
    CHECK_FALSE(shownText(h, "one"));
    CHECK(headingTexts(h) == std::vector<std::string>{"2", "1"});
    h.backend.click(h.byTestId("Button", "join"));
    h.settle();  // row 3 joins the first run
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
    TableHarness h;
    auto const screen = h.mount(kDrafts);
    REQUIRE(shownText(h, "a"));
    h.backend.edit(h.byTestId("TextInput", "edit"), "edited");
    h.settle();                     // the first row's input: a's
    CHECK(shownText(h, "edited"));  // its text cell reads the same draft
    CHECK_FALSE(shownText(h, "a"));
    MountStats const before = statsOf(*screen);
    h.backend.click(h.byTestId("Button", "swap"));
    h.settle();  // c takes the place a left
    CHECK(shownText(h, "b"));
    CHECK(shownText(h, "c"));
    CHECK_FALSE(shownText(h, "edited"));
    CHECK(shownText(h, "C"));  // the plain cell, rebound to c
    CHECK_FALSE(shownText(h, "A"));
    CHECK(statsOf(*screen).cellsCreated - before.cellsCreated == 2);  // only the two scoped cells built
}

// Mutation: a page of focus taken from the reported range of a document's rows, empty before any report (PageDown
// moves one row).

TEST_CASE("rows table: before the backend reports a range, a page of focus is the overscan's rows", "[ui][table]") {
    TableHarness h;
    auto const screen = h.mount(rowsDoc(idRows(50)));
    int const table = tableOf(h);
    h.backend.focusMove(table, morph::ui::FocusMove::PageDown);
    h.settle();
    CHECK(h.backend.focusCell(table).first == 10);  // Virtualization::overscan
}

// Mutation: `setGroup` given an empty unit table (1 kg and 1000 g are two runs).

TEST_CASE("value rows source: a group's quantities are compared through the screen's units", "[ui][table]") {
    RowsFixture h;
    auto const src = sourceOf(h, {col("id")});
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
    auto const screen = h.mount(rowsDoc(R"([{"id":"a","weight":{"num":3,"den":1,"dp":0,"unit":"m"}},)"
                                        R"({"id":"b","weight":{"num":1,"den":1,"dp":0,"unit":"m"}},)"
                                        R"({"id":"c","weight":{"num":2,"den":1,"dp":0,"unit":"m"}}])"));
    h.backend.clickHeader(tableOf(h), 2);
    h.settle();
    CHECK(rowOrder(h) == "b c a");
    CHECK(std::ranges::none_of(h.diagnostics, [](Refusal const& r) { return r.path.ends_with("/sort"); }));
}

// Mutation: the lowering grouping with an empty unit table (1 kg and 1000 g are two runs: headings 1, 1, 1).

TEST_CASE("rows table: a group's quantities are compared through the screen's units", "[ui][table]") {
    TableHarness h;
    FormatContext format = formatFor("en");
    format.units.insert_or_assign(
        "g", morph::ui::UnitInfo{.canonical = "g", .toCanonical = *table::parseDecimal("1"), .display = {}});
    format.units.insert_or_assign(
        "kg", morph::ui::UnitInfo{.canonical = "g", .toCanonical = *table::parseDecimal("1000"), .display = {}});
    h.session.setFormatting(format);
    auto const screen = h.mount(R"({"vocab":"ui/1","id":"samples",
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
    TableHarness h;
    auto const screen = h.mount(rowsDoc(kThree));
    h.backend.clickHeader(tableOf(h), 1);
    h.settle();
    REQUIRE(rowOrder(h) == "a b c");
    MountStats const before = statsOf(*screen);
    h.backend.click(h.byTestId("Button", "rename"));
    h.settle();
    CHECK(rowOrder(h) == "a c b");
    CHECK(statsOf(*screen).viewChanges - before.viewChanges == 1);
    CHECK(statsOf(*screen).viewResets == before.viewResets);
    CHECK(statsOf(*screen).cellsCreated == before.cellsCreated);  // b kept its widgets as it moved
}

// Mutation: the rebuild for a brought unit not carrying the engine's filter over (the new engine has none).

TEST_CASE("rows table: a unit the rows bring keeps the engine's filter", "[ui][table]") {
    std::string const quantities = R"([{"id":"a","name":"a","weight":{"num":1,"den":1,"dp":0,"unit":"m"}},)"
                                   R"({"id":"b","name":"b","weight":{"num":2,"den":1,"dp":0,"unit":"m"}}])";
    std::string text = rowsDoc(kThree);
    std::string const renamed = morph::ui::testing::renamedB(kThree);
    text.replace(text.find(renamed), renamed.size(), quantities);
    TableHarness h;
    auto screen = h.mount(text);
    table::FilterSpec filter;
    filter.columns["name"].include.push_back(table::FilterEntry{.contains = "a"});
    REQUIRE(engineOf(*screen, "list").setFilter(filter));
    table::Engine const* const first = &engineOf(*screen, "list");
    h.backend.click(h.byTestId("Button", "rename"));
    h.settle();                                    // the rows bring the unit `m`
    REQUIRE(&engineOf(*screen, "list") != first);  // a new engine
    CHECK(engineOf(*screen, "list").filter() == filter);
}

namespace {

/// The x-table request of the typed list actions these cases add.

constexpr std::string_view kQuerySchema =
    R"({"type":"object","properties":{"query":{"type":"object","x-table":true}}})";

/// Whether the body of the last call of @p action names @p text.

bool lastBodyHas(TableHarness const& h, std::string_view action, std::string_view text) {
    return h.dispatcher.calls.at(h.dispatcher.last(action)).body.contains(text);
}

}  // namespace

// Mutations: `{num, den, dp}` read as text (weight is Integer, from 3 alone, and not a Rational); a fractional number
// voted Decimal (ratio is Decimal); the query written at the body's top level (no "query" member in the body).

TEST_CASE("model table over an action: a typed list action's rows read into the window", "[ui][table]") {
    TableHarness h;
    addPageSamples(h);
    auto screen = h.mount(kActionTable);
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);            // no describe call: none exists
    CHECK(h.dispatcher.calls.back().body.contains(R"("query":{)"));  // the query in the x-table member
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a","weight":{"num":5,"den":2,"dp":1},"ratio":0.5,"day":"2026-11-17"},
                        {"id":2,"name":"b","weight":3,"ratio":2,"day":null}],"total":2})");
    using enum table::ColumnKind;
    CHECK(kindOfColumn(*screen, "id") == Integer);
    CHECK(kindOfColumn(*screen, "name") == Text);
    CHECK(kindOfColumn(*screen, "weight") == Decimal);  // {num,den,dp} with an integer
    CHECK(kindOfColumn(*screen, "ratio") == Number);    // a fraction with an integer
    CHECK(kindOfColumn(*screen, "day") == Date);        // the literal kind
    CHECK(*cellOf(*screen, 0, "ratio") == table::Cell{0.5});
    CHECK(*cellOf(*screen, 1, "ratio") == table::Cell{2.0});
    CHECK(std::holds_alternative<morph::math::Rational>(*cellOf(*screen, 1, "weight")));
    CHECK(*cellOf(*screen, 0, "day") == table::Cell{std::int64_t{20'774}});  // days since 1970-01-01
    CHECK(std::holds_alternative<std::monostate>(*cellOf(*screen, 1, "day")));
    CHECK(statusOf(h).counts == "2 rows");
    h.backend.clickHeader(tableOf(h), 1);
    h.settle();
    CHECK(h.dispatcher.calls.back().body.contains(R"("sort":[{"column":"name")"));
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":3,"name":"c","weight":"heavy","ratio":1}],"total":1})");
    CHECK(kindOfColumn(*screen, "weight") == Decimal);  // fixed by the first reply
}

// Mutation: the view document's `kind` ignored (id is inferred Text from a reply whose ids are strings).

TEST_CASE("model table over an action: columns and kinds come from the action's view document", "[ui][table]") {
    TableHarness h;  // Lims_ListSamples has a v-columns view
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
    TableHarness h;
    addPageSamples(h);
    addAction(
        h, "Lims_TwoQueries",
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
    TableHarness h;
    addPageSamples(h);
    auto const screen = h.mount(kActionTable);
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":1,"name":"a"}],"total":1})");
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);
    h.session.reconnected();
    h.settle();
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
    auto const screen = h.mount(kActionTable);
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);  // both watches answered
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"id":1,"name":"a"}],"total":1})");
    morph::wire::ChangesBody body;
    body.records.push_back(morph::effects::RecordChange{.record = "Lims_Result", .keys = std::nullopt});
    h.session.applyChanges(body);
    h.settle();
    CHECK(h.dispatcher.count("Lims_PageSamples") == 2);
}

// Mutations: the stored filter fitted to the placeholder kinds (`ratio` dropped: no second query); a held filter
// dropped without a report (no diagnostic for `name`).

TEST_CASE("model table over an action: a stored filter on an inferred column waits for its kind",
          "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(
        storeExecutor(),
        R"({"version":1,"filters":{"columns":{"ratio":{"include":[{"gt":"1"}]},"name":{"include":[{"gt":"b"}]}}}})");
    TableHarness h{store};
    addPageSamples(h);
    auto screen = h.mount(kActionTable);
    store->release();
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);
    CHECK_FALSE(lastBodyHas(h, "Lims_PageSamples", R"("gt")"));  // no kind yet: the filter waits
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a","ratio":0.5},{"id":2,"name":"b","ratio":2}],"total":2})");
    REQUIRE(kindOfColumn(*screen, "ratio") == table::ColumnKind::Number);
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 2);  // the stored filter, now it applies
    CHECK(lastBodyHas(h, "Lims_PageSamples", R"({"gt":"1"})"));
    CHECK_FALSE(lastBodyHas(h, "Lims_PageSamples", R"({"gt":"b"})"));
    CHECK(diagnosticsMentioning(h, "the stored filter on `name` is not applied") == 1);  // Text takes no `gt`
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
    CHECK(unitOf(*screen, "g").toBase == morph::math::Rational{morph::math::Numerator{1},
                                                               morph::math::Denominator{1000},
                                                               morph::math::DecimalPlaces{3}});
    for (std::int64_t row : {0, 1}) {
        auto const* const measure = std::get_if<table::Measure>(cellOf(*screen, row, "mass"));
        REQUIRE(measure != nullptr);
        CHECK(measure->unit != table::kUnknownUnit);
    }
}

// Mutation: a unit no description lists left unknown (the cell's unit is kUnknownUnit, an invalid cell).

TEST_CASE("model table over an action: a unit no one lists is its own canonical unit", "[ui][table]") {
    TableHarness h;
    addPageSamples(h);
    auto screen = h.mount(kActionTable);
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a","weight":{"num":5,"den":1,"dp":0,"unit":"furlong"}}],"total":1})");
    CHECK(kindOfColumn(*screen, "weight") == table::ColumnKind::Measure);
    auto const* const measure = std::get_if<table::Measure>(cellOf(*screen, 0, "weight"));
    REQUIRE(measure != nullptr);
    CHECK(measure->unit != table::kUnknownUnit);
    CHECK(unitOf(*screen, "furlong").dimension == "furlong");
}

// Mutations: the locate read sent to `Table.` and the table's id, as a model's table's (no call of the action); a
// reply that holds the row without `indices` read as the key's absence (an answer of nothing, not an error).

TEST_CASE("model table over an action: a row is located through the action, and a row without its index is unknown",
          "[ui][table]") {
    TableHarness h;
    addPageSamples(h);
    auto screen = h.mount(kActionTable);
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a"},{"id":2,"name":"b"}],"total":300})");
    std::vector<morph::ui::RowLocation> answers;
    auto const locate = [&] {
        std::size_t const sent = h.dispatcher.count("Lims_PageSamples");
        windowOf(*screen).locateRow(table::RowId{std::int64_t{2}}, {},
                                    [&answers](morph::ui::RowLocation const& found) { answers.push_back(found); });
        h.settle();
        REQUIRE(h.dispatcher.count("Lims_PageSamples") == sent + 1);
        INFO(h.dispatcher.calls.back().body);
        CHECK(lastBodyHas(h, "Lims_PageSamples", R"("keys":[2])"));  // the key in the x-table member
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
    CHECK_FALSE(answers.back().has_value());  // where the row is is not known
    locate();
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[],"total":300})");
    REQUIRE(answers.size() == 3);
    REQUIRE(answers.back().has_value());
    CHECK_FALSE(answers.back()->has_value());  // no row: the view lacks the key
}

// Mutation: a typed list action's table taken as not described, its revision being empty (no keyed call is sent).

TEST_CASE("model table over an action: highlightKey locates its row through the action and marks it", "[ui][table]") {
    constexpr char const* kTypedHighlight =
        R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},)"
        R"("state":{"hk":{"init":{"lit":null}}},"view":{"kind":"column","children":[)"
        R"({"kind":"button","label":"2","testId":"to2","onClick":[{"set":"hk","value":2}]},)"
        R"({"kind":"table","id":"list","testId":"list","model":"lab","action":"Lims_PageSamples","key":{"row":"id"},)"
        R"("highlightKey":{"ref":"hk"},"columns":[{"id":"id"},{"id":"name"}]}]}})";
    TableHarness h;
    addPageSamples(h);
    auto const screen = h.mount(kTypedHighlight);
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a"},{"id":2,"name":"b"}],"total":300})");
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
        TableHarness h;
        addPageSamples(h);
        auto const screen = h.mount(literalRatioFilter());
        REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);
        CHECK_FALSE(lastBodyHas(h, "Lims_PageSamples", R"("gt")"));
        h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
        REQUIRE(h.dispatcher.count("Lims_PageSamples") == 2);
        CHECK(lastBodyHas(h, "Lims_PageSamples", R"({"gt":"1"})"));
        CHECK(h.diagnostics.empty());
    }
    SECTION("bound") {
        TableHarness h;
        addPageSamples(h);
        std::string text = withTableKey(kActionTable, R"("filters":{"ref":"f"})");
        text.replace(text.find(R"("view":)"), 7,
                     R"("state":{"f":{"init":{"lit":{"columns":{"ratio":{"include":[{"gt":"1"}]}}}}}},"view":)");
        auto const screen = h.mount(text);
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
    TableHarness h;
    addPageSamples(h);
    auto screen = h.mount(literalRatioFilter());
    REQUIRE(elementOf(*screen, "list").tableRows->setFilter(filterOf("name", table::FilterEntry{.contains = "a"})));
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 2);  // the user's filter is a new query
    h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
    CHECK(h.dispatcher.count("Lims_PageSamples") == 2);
    CHECK(lastBodyHas(h, "Lims_PageSamples", R"({"contains":"a"})"));
    CHECK(windowOf(*screen).filter() == filterOf("name", table::FilterEntry{.contains = "a"}));
}

// Mutation: the user's filter not fitted again once the kinds are fixed (no report, and `contains` stays on the
// Number column: 2 == 3).

TEST_CASE("model table over an action: a user's filter the inferred kind refuses is reported and dropped",
          "[ui][table]") {
    TableHarness h;
    addPageSamples(h);
    auto screen = h.mount(kActionTable);
    REQUIRE(elementOf(*screen, "list").tableRows->setFilter(filterOf("ratio", table::FilterEntry{.contains = "5"})));
    h.settle();
    h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
    CHECK(diagnosticsMentioning(h, "the filter on `ratio` is not applied: it does not apply to the column's kind") ==
          1);
    CHECK(h.dispatcher.count("Lims_PageSamples") == 3);
    CHECK(windowOf(*screen).filter() == table::FilterSpec{});
}

// Mutations: each column's units in a dimension of its own (mg is not g's: "kg" == "g"); a factor of zero listed
// (no report).

TEST_CASE("model table over an action: columns that share a unit share its dimension in any order", "[ui][table]") {
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
    CHECK(unitOf(*screen, "mg").toBase == morph::math::Rational{morph::math::Numerator{1},
                                                                morph::math::Denominator{1000},
                                                                morph::math::DecimalPlaces{3}});
    CHECK(diagnosticsMentioning(h, "the unit `bad` has a factor of zero or less") == 1);
}

// Mutation: a held stored filter giving way to any gesture (the sort and the scroll drop it: no `gt` in the last
// query).

TEST_CASE("model table over an action: a sort and a scroll before the first reply keep the stored filter",
          "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(
        storeExecutor(), R"({"version":1,"filters":{"columns":{"ratio":{"include":[{"gt":"1"}]}}}})");
    TableHarness h{store};
    addPageSamples(h);
    auto const screen = h.mount(kActionTable);
    store->release();
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_PageSamples") == 1);
    h.backend.clickHeader(tableOf(h), 1);
    h.settle();  // a sort
    h.backend.scrollTable(tableOf(h), 0, 5);
    h.advance(80ms);  // a scroll
    h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
    CHECK(lastBodyHas(h, "Lims_PageSamples", R"({"gt":"1"})"));
    CHECK(lastBodyHas(h, "Lims_PageSamples", R"("sort":[{"column":"name")"));
    CHECK(h.diagnostics.empty());
}

// Mutation: the kinds a first reply that does not read voted kept (ratio is Number before any reply read).

TEST_CASE("model table over an action: a first reply that does not read fixes no kind", "[ui][table]") {
    TableHarness h;
    addPageSamples(h);
    auto screen = h.mount(kActionTable);
    h.reply(h.dispatcher.last("Lims_PageSamples"), R"({"rows":[{"name":"a","ratio":0.5}],"total":1})");  // no key
    CHECK(kindOfColumn(*screen, "ratio") == table::ColumnKind::Text);
    CHECK(windowOf(*screen).inferring());
    windowOf(*screen).retry();
    h.settle();
    h.reply(h.dispatcher.last("Lims_PageSamples"), std::string{kRatioRows});
    CHECK(kindOfColumn(*screen, "ratio") == table::ColumnKind::Number);
    CHECK_FALSE(windowOf(*screen).inferring());
}

// Mutation: no report for a `key` that is not one row field.

TEST_CASE("model table over an action: a key that is not one row field is reported", "[ui][table]") {
    TableHarness h;
    addPageSamples(h);
    auto const screen = h.mount(withTableKey(kActionTable, R"("key":{"lit":"id"})"));
    CHECK(diagnosticsMentioning(h, "the key of a table over an action is one row field") == 1);
}

// Mutation: an integer beyond int64 read as its JSON text (a string cell).

TEST_CASE("model table over an action: an integer beyond int64 is a double", "[ui][table]") {
    TableHarness h;
    addPageSamples(h);
    auto screen = h.mount(kActionTable);
    h.reply(h.dispatcher.last("Lims_PageSamples"),
            R"({"rows":[{"id":1,"name":"a","ratio":18446744073709551615}],"total":1})");
    CHECK(kindOfColumn(*screen, "ratio") == table::ColumnKind::Number);
    CHECK(*cellOf(*screen, 0, "ratio") == table::Cell{18446744073709551615.0});
}

// Mutation: no bound on the units a window lists (the 65th unit is listed and its cell is valid).

TEST_CASE("model table over an action: a window lists at most 64 units no one declares", "[ui][table]") {
    TableHarness h;
    addPageSamples(h);
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
    TableHarness h;
    addPageSamples(h);
    auto screen = h.mount(kActionTable);
    std::shared_ptr<WindowRows> const window = elementOf(*screen, "list").tableWindow;
    std::size_t const pending = h.dispatcher.last("Lims_PageSamples");
    screen.reset();
    h.settle();
    h.reply(pending, std::string{kRatioRows});
    CHECK_FALSE(window->inferring());  // the reply was read
    CHECK(h.diagnostics.empty());
}

namespace {

// A rows table `t` of @p count sortable columns c1… over one row, with the table node's members @p extra.

std::string sortableRows(int count, std::string_view extra) {
    std::string row = R"({"id":1)";
    std::string columns;
    for (int at = 1; at <= count; ++at) {
        row += ",\"c" + std::to_string(at) + "\":" + std::to_string(at);
        columns += std::string{at == 1 ? "" : ","} + R"({"id":"c)" + std::to_string(at) + R"(","sortable":true})";
    }
    row += '}';
    return R"({"vocab":"ui/1","id":"many","state":{"items":{"init":{"lit":[)" + row +
           R"(]}}},"view":{"kind":"table","id":"t","rows":{"ref":"items"},"columns":[)" + columns + "]," +
           std::string{extra} + "}}";
}

}  // namespace

// Mutation: the lowering mapping HeaderClick::Shift to Click.

TEST_CASE("sort chains: a plain click replaces, Shift adds and flips, a key past the bound is refused",
          "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto const screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    h.backend.clickHeader(table, 0, HeaderClick::Plain);
    h.backend.clickHeader(table, 1, HeaderClick::Shift);
    h.backend.clickHeader(table, 0, HeaderClick::Shift);
    h.settle();
    CHECK(elementValue(*screen, "t", "sort") ==
          R"([{"column":"c1","direction":"desc"},{"column":"c2","direction":"asc"}])");
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

TEST_CASE("sort chains: a table whose limits set 32 accepts 32 keys from the header and refuses the 33rd",
          "[ui][table][config]") {
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
    auto const screen = h.mount(countingNine("onSortChange"));
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, 0, {"Sort descending"});
    chooseMenu(h, table, 2, {"Add to sort"});
    CHECK(elementValue(*screen, "t", "sort") ==
          R"([{"column":"c1","direction":"desc"},{"column":"c3","direction":"asc"}])");
    chooseMenu(h, table, 0, {"Remove from sort"});
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c3","direction":"asc"}])");
    CHECK(readJson(*screen, R"({"ref":"n"})") == "3");
    CHECK(readJson(*screen, R"({"ref":"last"})") == R"([{"column":"c3","direction":"asc"}])");
}

// Mutations: the lowering applying a menu gesture without updating `visibleColumns`; a driver's column read as a
// visible place (the resize after the move lands on c1).

TEST_CASE(
    "column layout: hiding, moving, pinning and resetting from the menus change the layout and the visible columns",
    "[ui][table][config]") {
    morph::ui::testing::TableHarness h;
    auto const screen = h.mount(countingNine("onLayoutChange"));
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, 2, {"Hide column"});
    chooseMenu(h, table, 1, {"Move left"});
    chooseMenu(h, table, 1, {"Pin column"});
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c2","c1","c4","c5","c6","c7","c8","c9"])");
    CHECK(elementValue(*screen, "t", "hiddenCount") == "1");
    CHECK(elementValue(*screen, "t", "layout") ==
          R"({"hidden":["c3"],"order":["c2","c1","c3","c4","c5","c6","c7","c8","c9"],"pinned":1,"widths":{}})");
    CHECK_THROWS(h.backend.menuOf(table, 2));  // the mount draws no header for c3
    resizeColumn(h, table, 1, 20);             // index 1 is c2 wherever it stands
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

TEST_CASE("column layout: configurable and locked limit what the user can do, and each refusal is shown",
          "[ui][table][config]") {
    SECTION("hide only, the first column locked") {
        morph::ui::testing::TableHarness h;
        auto const screen =
            h.mount(withFirstColumnKey(withTableKey(kNine, R"("configurable":{"lit":["hide"]})"), R"("locked":true)"));
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
        auto const screen = h.mount(withTableKey(kNine, R"("configurable":false)"));
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
        auto const screen = h.mount(kNine);
        int const table = h.backend.all("Table").front();
        refuse(h, table);
        CHECK(statusOf(h, table).message == "A table sorts by at most 8 columns");
        h.advance(5'999ms);
        CHECK(statusOf(h, table).message == "A table sorts by at most 8 columns");
        CHECK(statusOf(h, table).shown);
        h.advance(1ms);
        CHECK(statusOf(h, table).message.empty());
        CHECK_FALSE(statusOf(h, table).shown);  // a rows table with nothing to say
        h.backend.clickHeader(table, 8, HeaderClick::Shift);
        h.settle();
        REQUIRE(statusOf(h, table).message == "A table sorts by at most 8 columns");
        h.backend.clickHeader(table, 0, HeaderClick::Plain);
        h.settle();
        CHECK(statusOf(h, table).message.empty());
    }
    SECTION("with `status: false`, as a notification") {
        morph::ui::testing::TableHarness h;
        auto const screen = h.mount(withTableKey(kNine, R"("status":false)"));
        int const table = h.backend.all("Table").front();
        refuse(h, table);
        REQUIRE(h.host.notified.size() == 1);
        CHECK(h.host.notified.front() ==
              std::pair<std::string, std::string>{"A table sorts by at most 8 columns", "warning"});
        CHECK_FALSE(statusOf(h, table).shown);
    }
}
