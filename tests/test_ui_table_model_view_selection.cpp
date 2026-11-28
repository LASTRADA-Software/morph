// SPDX-License-Identifier: Apache-2.0

// Saved views, selection and the table menus.
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

using morph::ui::Value;
using morph::ui::testing::describeJson;
using namespace std::chrono_literals;
using morph::ui::TableStatus;
using morph::ui::testing::addAction;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::clipboardText;
using morph::ui::testing::elementValue;
using morph::ui::testing::entryOf;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::kModelTable;
using morph::ui::testing::kSamplesCollection;
using morph::ui::testing::kThree;
using morph::ui::testing::lastQuery;
using morph::ui::testing::oldestOpen;
using morph::ui::testing::openCalls;
using morph::ui::testing::pressCopy;
using morph::ui::testing::queryOf;
using morph::ui::testing::readJson;
using morph::ui::testing::RecordingTableStateStore;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::replyPage;
using morph::ui::testing::rowsDoc;
using morph::ui::testing::statusOf;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;
using morph::ui::testing::tableOf;
using morph::ui::testing::value;
using morph::ui::testing::withLevel;

// Mutations: a `columns` point on a table over `rows` left a plain preference (no save goes, the columns keep their
// order); the stored layout not shown at the point (`cols.pinned` reads null).

TEST_CASE("rows table: a columns point naming it reads and arranges its layout", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(
        storeExecutor(), R"({"version":1,"layout":{"order":["id","name","weight"],"hidden":[],"pinned":1}})");
    TableHarness h{store};
    std::string text = rowsDoc(kThree);
    text.insert(text.find(R"("state":)"), R"("customize":{"cols":{"kind":"columns","of":"list"}},)");
    auto screen = h.mount(text);
    store->release();
    h.settle();
    CHECK(readJson(*screen, R"({"pref":"cols.pinned"})") == "1");
    auto const layout = morph::ui::parseJson(R"({"order":["name","id","weight"],"hidden":["weight"],"pinned":0})");
    REQUIRE(layout.has_value());
    CHECK(screen->setPreference("cols", *morph::ui::valueFromJson(*layout)));
    CHECK_FALSE(screen->setPreference("cols", morph::ui::Value{std::int64_t{7}}));  // not a layout
    h.advance(500ms);
    REQUIRE(store->saves() == 1);
    auto const saved = table::readState(store->lastSaved());
    REQUIRE(saved.has_value());
    REQUIRE(saved->layout.has_value());
    CHECK(saved->layout->order.front() == "name");
    CHECK(elementValue(*screen, "list", "visibleColumns") == R"(["name","id"])");
}

// Mutation: a point that finds no table keeping a state refused (`setPreference` answers false).

TEST_CASE("rows table: a columns point naming one that keeps no state is a preference", "[ui][table]") {
    TableHarness h;
    std::string text = rowsDoc(kThree);
    text.insert(text.find(R"("state":)"), R"("customize":{"cols":{"kind":"columns","of":"list"}},)");
    auto screen = h.mount(text);
    auto const layout = morph::ui::parseJson(R"({"order":["name","id","weight"],"hidden":[],"pinned":0})");
    REQUIRE(layout.has_value());
    CHECK(screen->setPreference("cols", *morph::ui::valueFromJson(*layout)));
    CHECK(readJson(*screen, R"({"pref":"cols.pinned"})") == "0");
    CHECK(elementValue(*screen, "list", "visibleColumns") == R"(["id","name","weight"])");
}

// Mutation: a point matched to any listed table by id (an empty `of` names the id-less collection, whose
// model-backed state is null: the run aborts).

TEST_CASE("collection: a saved point without of beside an id-less collection is a preference", "[ui][table]") {
    auto const store = std::make_shared<table::MemoryTableStateStore>(morph::ui::testing::storeExecutor());
    TableHarness h{store};
    std::string text{kSamplesCollection};
    text.insert(text.find(R"("models":)"), R"("customize":{"v":{"kind":"saved"}},)");
    auto screen = h.mount(text);
    h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
    CHECK(screen->setPreference("v", morph::ui::Value{std::string{"Mine"}}));
    CHECK(readJson(*screen, R"({"pref":"v"})") == R"("Mine")");
}

// Mutations: an empty `of` matching an id-less model table in `tablePoint` (its view is rewritten and saved);
// in `tablePoints` (a gesture writes the table's view, `null`, over the preference).

TEST_CASE("model table: a saved point without of leaves an id-less model table's state alone", "[ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), R"({"version":1})");
    TableHarness h{store};
    std::string text{kModelTable};
    text.erase(text.find(R"("id":"list",)"), std::string_view{R"("id":"list",)"}.size());
    text.insert(text.find(R"("models":)"), R"("customize":{"v":{"kind":"saved"}},)");
    auto screen = h.mount(text);
    replyDescribe(h);
    store->release();
    h.settle();
    CHECK(screen->setPreference("v", morph::ui::Value{std::string{"Mine"}}));
    h.advance(500ms);
    CHECK(store->saves() == 0);
    // A gesture on the table shows its state at the points that name it, and this one names none.
    chooseMenu(h, tableOf(h), 2, {"Hide column"});
    h.advance(500ms);
    CHECK(readJson(*screen, R"({"pref":"v"})") == R"("Mine")");
}

// Mutation: a collection's table building no `TableViews` (no `views` load goes out).

TEST_CASE("collection: its saved views load beside its state", "[ui][table][state]") {
    auto const store = std::make_shared<RecordingTableStateStore>(morph::ui::testing::storeExecutor());
    TableHarness h{store};
    auto const screen = h.mount(kSamplesCollection);
    std::vector<std::string> parts;
    for (auto const& call : store->calls) {
        if (call.op == "load") {
            parts.push_back(call.part);
        }
    }
    std::ranges::sort(parts);
    CHECK(parts == std::vector<std::string>{"state", "views"});
}

namespace {

// `id` (the key), `mass` (a decimal), and `companyA` and `companyB`, the declared group `company` ("Company").

table::TableDescription menuDescription() {
    table::TableDescription d;
    d.id = "samples";
    d.label = "Samples";
    d.key = "id";
    d.columns = {
        table::TableColumn{.info = {.id = "id", .kind = table::ColumnKind::Integer}, .label = "Id"},
        table::TableColumn{.info = {.id = "mass", .kind = table::ColumnKind::Decimal}, .label = "Mass"},
        table::TableColumn{
            .info = {.id = "companyA", .kind = table::ColumnKind::Text}, .label = "Company A", .group = "company"},
        table::TableColumn{
            .info = {.id = "companyB", .kind = table::ColumnKind::Text}, .label = "Company B", .group = "company"},
    };
    d.groups = {table::ColumnGroupInfo{.id = "company", .label = "Company", .columns = {"companyA", "companyB"}}};
    return table::describe(std::move(d));
}

using MenuRow = std::map<std::string, table::Cell, std::less<>>;

// Rows 1 to 4 weigh 0.1 to 0.4, row 5 has no mass; row 1 is of companies E and G, row 2 of F.

std::vector<MenuRow> menuRows() {
    auto const mass = [](std::string_view text) { return table::Cell{table::parseDecimal(text).value()}; };
    auto const text = [](std::string value) { return table::Cell{std::move(value)}; };
    return {
        MenuRow{{"id", table::Cell{std::int64_t{1}}},
                {"mass", mass("0.1")},
                {"companyA", text("E")},
                {"companyB", text("G")}},
        MenuRow{{"id", table::Cell{std::int64_t{2}}}, {"mass", mass("0.2")}, {"companyA", text("F")}},
        MenuRow{{"id", table::Cell{std::int64_t{3}}}, {"mass", mass("0.3")}},
        MenuRow{{"id", table::Cell{std::int64_t{4}}}, {"mass", mass("0.4")}},
        MenuRow{{"id", table::Cell{std::int64_t{5}}}},
    };
}

// The page answering the latest query with `menuRows()`, each holding the columns the query names, against
// @p description; the rows of @p unloaded have levels not loaded.

std::string menuPageOf(std::vector<std::string> columns, table::TableDescription const& description,
                       std::vector<table::UnloadedRow> unloaded) {
    std::vector<MenuRow> const rows = menuRows();
    table::GenericPage page;
    page.revision = description.revision;
    page.columns = std::move(columns);
    page.total = static_cast<std::int64_t>(rows.size());
    page.totalKind = table::TotalKind::Exact;
    page.offset = 0;
    for (MenuRow const& row : rows) {
        page.keys.emplace_back(std::get<std::int64_t>(row.at("id")));
        table::GenericRow cells;
        for (std::string const& column : page.columns) {
            auto const found = row.find(column);
            cells.push_back(found == row.end() ? table::Cell{} : found->second);
        }
        page.rows.push_back(std::move(cells));
    }
    page.unloadedRows = std::move(unloaded);
    return glz::write_json(page).value_or("");
}

std::string menuPage(TableHarness const& h, table::TableDescription const& description = menuDescription(),
                     std::vector<table::UnloadedRow> unloaded = {}) {
    return menuPageOf(morph::ui::testing::lastQuery(h).columns.value_or(std::vector<std::string>{}), description,
                      std::move(unloaded));
}

// Answers every open `Table.samples` call with `menuRows()`, each by the columns its own query names: the server
// answers a filtered query with every row, so each cell stays to be chosen.

void answerMenu(TableHarness& h) {
    using morph::ui::testing::oldestOpen;
    using morph::ui::testing::openCalls;
    using morph::ui::testing::queryOf;
    for (int round = 0; round < 16 && openCalls(h, "Table.samples") > 0; ++round) {
        std::size_t const index = oldestOpen(h, "Table.samples");
        h.reply(index,
                menuPageOf(queryOf(h, index).columns.value_or(std::vector<std::string>{}), menuDescription(), {}));
    }
    REQUIRE(openCalls(h, "Table.samples") == 0);
}

// A document whose table `t` over the samples selects several rows and carries @p tableKeys, beside a button
// `other` that sets `idle`; the state `picked`, `opened` and `idle` (false) is the cases' to set and read.

std::string menuDoc(std::string_view tableKeys) {
    std::string table = R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",)"
                        R"("selectionMode":"multiple")";
    if (!tableKeys.empty()) {
        table += "," + std::string{tableKeys};
    }
    table += '}';
    return R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
           R"("state":{"picked":{"init":{"lit":null}},"opened":{"init":{"lit":null}},"idle":{"init":{"lit":false}}},)"
           R"("view":{"kind":"column","children":[)" +
           table + R"(,{"kind":"button","testId":"other","label":"Other","onClick":[{"set":"idle","value":true}]}]}})";
}

// A menu's labels, a separator written `-`.

std::vector<std::string> menuLabels(std::vector<morph::ui::MenuEntry> const& entries) {
    std::vector<std::string> out;
    for (morph::ui::MenuEntry const& entry : entries) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `out` is a fixture list built once; sizing it first would not change what the case checks
        out.push_back(entry.separator ? std::string{"-"} : entry.label);
    }
    return out;
}

// A filter from its JSON.

table::FilterSpec filterSpec(std::string_view json) {
    table::FilterSpec out;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(out, json));
    return out;
}

// The table `t`'s filters, its element value read back.

table::FilterSpec filtersOf(morph::ui::Screen const& screen) {
    return filterSpec(elementValue(screen, "t", "filters"));
}

morph::ui::Key menuKey(std::int64_t key) { return morph::ui::Key{key}; }

}  // namespace

// Mutation: the cell menu read in the table's scope, which reads `row` as null.

TEST_CASE("table menus: the application's entries read the row, the column and the value", "[ui][table][menu]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto const screen = h.mount(menuDoc(
        R"("cellMenu":[{"label":{"fn":"format","args":["{0} = {1} of {2}",{"ref":"column"},{"ref":"value"},{"ref":"row.id"}]},)"
        R"("onClick":[{"set":"picked","value":{"ref":"row.id"}}]}],)"
        R"("headerMenu":[{"label":"Units…","visible":{"fn":"eq","args":[{"ref":"column"},"mass"]}}])"));
    REQUIRE(reportsOf(h).empty());
    replyDescribe(h, describeJson(menuDescription()));
    replyPage(h, menuPage(h));
    int const table = h.byTestId("Table", "t");
    std::size_t const mass = columnIndex(h, "mass");
    CHECK(menuLabels(h.backend.rightClickCell(table, menuKey(2), mass)) ==
          std::vector<std::string>{"Copy", "Copy with headers", "Filter by this value", "Exclude this value", "-",
                                   "mass = 0.2 of 2"});
    h.backend.chooseCellMenu(table, menuKey(2), mass, {"mass = 0.2 of 2"});
    CHECK(readJson(*screen, R"({"ref":"picked"})") == "2");
    // A right-click on a row that is not selected selects it alone; on a selected row the selection stays.
    h.backend.selectRows(table, {menuKey(1), menuKey(3)});
    static_cast<void>(h.backend.rightClickCell(table, menuKey(2), mass));
    CHECK(elementValue(*screen, "t", "selection") == "[2]");
    h.backend.selectRows(table, {menuKey(1), menuKey(3)});
    static_cast<void>(h.backend.rightClickCell(table, menuKey(1), mass));
    CHECK(elementValue(*screen, "t", "selection") == "[1,3]");
    // A header's entry shows on the columns its `visible` names.
    std::vector<std::string> const massHeader = menuLabels(h.backend.menuOf(table, mass));
    REQUIRE(massHeader.size() >= 2);
    CHECK(massHeader.at(massHeader.size() - 2) == "-");
    CHECK(massHeader.back() == "Units…");
    std::vector<std::string> const idHeader = menuLabels(h.backend.menuOf(table, columnIndex(h, "id")));
    CHECK_FALSE(std::ranges::contains(idHeader, std::string{"Units…"}));
    CHECK(idHeader.back() != "-");
}

// Mutation: "Filter by this value" adding to include instead of replacing it, which keeps the earlier entry.

TEST_CASE("table menus: filter by this value", "[ui][table][menu]") {
    using namespace morph::ui::testing;
    {
        TableHarness h;
        auto const screen = h.mount(menuDoc(""));
        replyDescribe(h, describeJson(menuDescription()));
        replyPage(h, menuPage(h));
        int const table = h.byTestId("Table", "t");
        std::size_t const mass = columnIndex(h, "mass");
        // The rows stay held across each new filter: `answerMenu` answers it with every row.
        h.backend.chooseCellMenu(table, menuKey(3), mass, {"Filter by this value"});
        answerMenu(h);
        h.backend.chooseCellMenu(table, menuKey(2), mass, {"Filter by this value"});
        answerMenu(h);
        CHECK(filtersOf(*screen) == filterSpec(R"({"columns":{"mass":{"include":[{"oneOf":["0.2"]}]}}})"));
        h.backend.chooseCellMenu(table, menuKey(3), mass, {"Exclude this value"});
        answerMenu(h);
        h.backend.chooseCellMenu(table, menuKey(4), mass, {"Exclude this value"});
        answerMenu(h);
        CHECK(filtersOf(*screen).columns.at("mass").exclude ==
              filterSpec(R"({"columns":{"mass":{"exclude":[{"oneOf":["0.3","0.4"]}]}}})").columns.at("mass").exclude);
        // An empty cell filters by emptiness.
        h.backend.chooseCellMenu(table, menuKey(5), mass, {"Filter by this value"});
        answerMenu(h);
        CHECK(filtersOf(*screen).columns.at("mass").include ==
              filterSpec(R"({"columns":{"mass":{"include":[{"isEmpty":true}]}}})").columns.at("mass").include);
        // A column of a declared group offers the group's filter.
        std::size_t const companyA = columnIndex(h, "companyA");
        CHECK(entryOf(h.backend.rightClickCell(table, menuKey(2), companyA), "Filter by this value in any Company")
                  .enabled);
        h.backend.chooseCellMenu(table, menuKey(2), companyA, {"Filter by this value in any Company"});
        answerMenu(h);
        CHECK(filtersOf(*screen).groups ==
              filterSpec(R"({"groups":[{"group":"company","include":[{"oneOf":["F"]}]}]})").groups);
        CHECK(std::ranges::none_of(
            menuLabels(h.backend.rightClickCell(table, menuKey(2), mass)),
            [](std::string const& label) { return label.starts_with("Filter by this value in"); }));
    }
    {
        // On a cell whose level has not loaded, both entries are disabled.
        TableHarness h;
        table::TableDescription const described = withLevel(menuDescription(), "mass", "assay");
        auto const screen = h.mount(menuDoc(""));
        replyDescribe(h, describeJson(described));
        replyPage(h, menuPage(h, described, {table::UnloadedRow{.index = 2, .levels = {"assay"}, .failed = {}}}));
        int const table = h.byTestId("Table", "t");
        std::size_t const mass = columnIndex(h, "mass");
        std::vector<morph::ui::MenuEntry> const unloaded = h.backend.rightClickCell(table, menuKey(3), mass);
        CHECK_FALSE(entryOf(unloaded, "Filter by this value").enabled);
        CHECK_FALSE(entryOf(unloaded, "Exclude this value").enabled);
        CHECK(entryOf(h.backend.rightClickCell(table, menuKey(2), mass), "Filter by this value").enabled);
    }
}

// Mutations: keys bound only while the menu is open; every key the table leaves asking for the entries (no chord
// check in `RecordingBackend::cellKey`).

TEST_CASE("table menus: an entry's keys work while the table has focus", "[ui][table][menu]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto const screen =
        h.mount(menuDoc(R"("columns":[{"id":"id"},{"id":"mass"}],)"
                        R"("cellMenu":[{"label":"Open","keys":"Ctrl+O","description":{"ref":"row.companyB"},)"
                        R"("onClick":[{"set":"opened","value":{"ref":"row.id"}}]}])"));
    replyDescribe(h, describeJson(menuDescription()));
    // What the entries read is fetched for every row, though no column shows it.
    std::vector<std::string> const named = lastQuery(h).columns.value_or(std::vector<std::string>{});
    CHECK(std::ranges::contains(named, std::string{"id"}));
    CHECK(std::ranges::contains(named, std::string{"companyB"}));
    replyPage(h, menuPage(h));
    int const table = h.byTestId("Table", "t");
    h.backend.focusMove(table, morph::ui::FocusMove::Down);
    h.backend.focusMove(table, morph::ui::FocusMove::Down);
    CHECK_FALSE(h.backend.press(h.byTestId("Button", "other"), "Ctrl+O"));
    CHECK(readJson(*screen, R"({"ref":"opened"})") == "null");
    // A key that is none of the entries' chords asks for no entries, and asking for them moves no focus.
    std::pair<std::size_t, std::size_t> const focused = h.backend.focusCell(table);
    CHECK_FALSE(h.backend.press(table, "Down"));
    CHECK_FALSE(h.backend.press(table, "Ctrl+P"));
    CHECK(h.backend.cellMenuAsks(table) == 0);
    CHECK(h.backend.press(table, "Ctrl+O"));
    CHECK(readJson(*screen, R"({"ref":"opened"})") == "3");
    CHECK(h.backend.cellMenuAsks(table) == 1);
    CHECK(h.backend.focusCell(table) == focused);
}

// Mutations: headerEnabled read once at mount, which leaves the header enabled after the condition turns false; the
// cell menu's filter entries not reading headerEnabled, which leaves them enabled.

TEST_CASE("table menus: a disabled header", "[ui][table][header]") {
    using namespace morph::ui::testing;
    TableHarness h;
    auto const screen =
        h.mount(menuDoc(R"("headerEnabled":{"ref":"idle"},)"
                        R"("cellMenu":[{"label":"Pick","onClick":[{"set":"picked","value":{"ref":"row.id"}}]}])"));
    replyDescribe(h, describeJson(menuDescription()));
    replyPage(h, menuPage(h));
    int const table = h.byTestId("Table", "t");
    std::size_t const mass = columnIndex(h, "mass");
    std::string const busy = "Sorting, filtering and arranging are unavailable right now";
    auto const message = [&] {
        h.settle();
        return h.backend.tableStatus(table).message;
    };
    std::string const sort = elementValue(*screen, "t", "sort");
    std::string const layout = elementValue(*screen, "t", "layout");
    std::string const filters = elementValue(*screen, "t", "filters");
    std::size_t const pages = pageCalls(h);
    h.backend.clickHeader(table, mass);
    CHECK(message() == busy);
    h.advance(7s);
    REQUIRE(message().empty());
    h.backend.clickFilter(table, mass);
    CHECK(message() == busy);
    h.advance(7s);
    REQUIRE(message().empty());
    h.backend.moveColumn(table, mass, 0);
    CHECK(message() == busy);
    CHECK(elementValue(*screen, "t", "sort") == sort);
    CHECK(elementValue(*screen, "t", "layout") == layout);
    CHECK(elementValue(*screen, "t", "filters") == filters);
    CHECK(pageCalls(h) == pages);
    // The header's own entries are disabled; the application's stay.
    std::vector<morph::ui::MenuEntry> const header = h.backend.menuOf(table, mass);
    REQUIRE_FALSE(header.empty());
    CHECK(std::ranges::none_of(header, &morph::ui::MenuEntry::enabled));
    CHECK(entryOf(h.backend.menuOf(table, std::nullopt), "Copy").enabled);
    CHECK_FALSE(entryOf(h.backend.menuOf(table, std::nullopt), "Sort…").enabled);
    // Selecting, copying and the cell menu's entries still work.
    h.backend.clickRow(table, menuKey(2), morph::ui::RowClick::Only);
    CHECK(elementValue(*screen, "t", "selection") == "[2]");
    h.backend.pressCopy(table);
    CHECK_FALSE(h.backend.clipboardText().empty());
    // The cell menu's own filter entries are disabled with the header; its copy entries and the application's stay.
    std::vector<morph::ui::MenuEntry> const cell = h.backend.rightClickCell(table, menuKey(2), mass);
    CHECK_FALSE(entryOf(cell, "Filter by this value").enabled);
    CHECK_FALSE(entryOf(cell, "Exclude this value").enabled);
    CHECK(entryOf(cell, "Copy").enabled);
    CHECK(entryOf(cell, "Pick").enabled);
    h.backend.chooseCellMenu(table, menuKey(2), mass, {"Pick"});
    CHECK(readJson(*screen, R"({"ref":"picked"})") == "2");
    // Once the application is idle, a header click sorts.
    h.backend.click(h.byTestId("Button", "other"));
    h.settle();
    h.backend.clickHeader(table, mass);
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"mass","direction":"asc"}])");
}

// Mutation: a table over `rows` setting no `heldCell`, which leaves its cells without a menu.

TEST_CASE("table menus: a table over rows takes the cell and header menus and a disabled header",
          "[ui][table][menu]") {
    using namespace morph::ui::testing;
    std::size_t const name = 1;  // the columns are `id`, `name` and `weight`
    morph::ui::Key const b{std::string{"b"}};
    {
        TableHarness h;
        auto const screen = h.mount(rowsDoc(
            kThree,
            R"(,"selectionMode":"multiple",)"
            R"("cellMenu":[{"label":{"fn":"format","args":["{0} of {1}",{"ref":"column"},{"ref":"row.id"}]}}],)"
            R"("headerMenu":[{"label":"About"}])"));
        REQUIRE(reportsOf(h).empty());
        int const table = tableOf(h);
        CHECK(menuLabels(h.backend.rightClickCell(table, b, name)) ==
              std::vector<std::string>{"Copy", "Copy with headers", "Filter by this value", "Exclude this value", "-",
                                       "name of b"});
        CHECK(menuLabels(h.backend.menuOf(table, name)).back() == "About");
        h.backend.chooseCellMenu(table, b, name, {"Filter by this value"});
        h.settle();
        CHECK(filterSpec(elementValue(*screen, "list", "filters")) ==
              filterSpec(R"({"columns":{"name":{"include":[{"oneOf":["b"]}]}}})"));
        CHECK(elementValue(*screen, "list", "viewCount") == "1");
    }
    {
        TableHarness h;
        auto const screen = h.mount(rowsDoc(kThree, R"(,"headerEnabled":false)"));
        int const table = tableOf(h);
        h.backend.clickHeader(table, name);
        h.settle();
        CHECK(statusOf(h).message == "Sorting, filtering and arranging are unavailable right now");
        CHECK(elementValue(*screen, "list", "sort") == "[]");
        CHECK_FALSE(entryOf(h.backend.rightClickCell(table, b, name), "Filter by this value").enabled);
    }
}

namespace {

// The same reply, keyed: with "indices".

std::string selectionKeyed(std::vector<SelRow> const& rows, std::int64_t total, std::string_view viewRevision) {
    return selectionReply(rows, total, viewRevision, {}, true);
}

// The text of a query, for comparing two.

std::string queryText(table::TableQuery const& query) { return glz::write_json(query).value_or(""); }

}  // namespace

TEST_CASE("view selection: Ctrl+A selects the view and fetches no row", "[ui][table][selection]") {
    TableHarness h;
    auto const screen = mountSelection(h);
    int const table = h.byTestId("Table", "t");
    auto const before = lastQuery(h);
    std::size_t const sent = h.dispatcher.count("Table.samples");
    h.backend.selectAll(table);  // the backend's Ctrl+A
    h.settle();
    CHECK(h.dispatcher.count("Table.samples") == sent);  // nothing sent
    CHECK(queryText(lastQuery(h)) == queryText(before));
    CHECK(elementValue(h, "t", "selectionSet") ==
          value(R"({"view":{"filters":{"columns":{"status":{"include":[{"eq":"open"}]}}},
                    "retained":[],"revision":"v1"},"except":[]})"));
    CHECK(elementValue(h, "t", "selection") == value("[]"));
    CHECK(elementValue(h, "t", "selectedCount") == morph::ui::Value{std::int64_t{6}});
    CHECK(elementValue(h, "t", "selectedCountKind") == morph::ui::Value{std::string{"exact"}});
    h.backend.ctrlClickRow(table, std::int64_t{3});
    h.settle();
    CHECK(elementValue(h, "t", "selectedCount") == morph::ui::Value{std::int64_t{5}});
    h.backend.clickRow(table, std::int64_t{2});
    h.settle();
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[2]})"));
}

// Mutation: select-all selecting the held keys, which sends no view and counts the held rows only.

TEST_CASE("view selection: a key of except that leaves the view is dropped from it",
          "[ui][table][selection][review]") {
    TableHarness h;
    auto const screen = mountSelection(h);
    int const table = h.byTestId("Table", "t");
    h.backend.selectAll(table);
    h.settle();
    h.backend.ctrlClickRow(table, std::int64_t{3});
    h.settle();
    CHECK(elementValue(h, "t", "selectedCount") == morph::ui::Value{std::int64_t{5}});
    fireEvent(h, "Lims_SampleModel.samples", {3});
    REQUIRE(lastQuery(h).keys.has_value());
    SECTION("the keyed reply leaves 3 out of the view") {
        // Row 3 became "closed": the reply holds 0, 1, 2, 4 and 5, and a page refetch, if any, answers the same.
        std::vector<SelRow> const left{{.key = 0}, {.key = 1}, {.key = 2}, {.key = 4}, {.key = 5}};
        replyPage(h, selectionKeyed(left, 5, "v2"));
        if (openCalls(h, "Table.samples") > 0) {
            replyPage(h, selectionPage(left, 5, "v2"));
        }
        CHECK(elementValue(h, "t", "selectedCount") == morph::ui::Value{std::int64_t{5}});
        CHECK(elementValue(h, "t", "selectedCountKind") == morph::ui::Value{std::string{"exact"}});
        CHECK(*elementValue(h, "t", "selectionSet").field("except") == value("[]"));
    }
    SECTION("the event's keyed fetch is left unanswered: key 3 is held but owed a refetch") {
        CHECK(elementValue(h, "t", "selectedCountKind") == morph::ui::Value{std::string{"approximate"}});
        CHECK(statusOf(h, table).counts == "about 5 of 6 selected");
    }
}

// Mutation: except keys never pruned, which leaves except [3].

TEST_CASE("view selection: its life", "[ui][table][selection]") {
    TableHarness h;
    addAction(h, "Lims_TagRows", std::string{kSelectionSchema}, samplesAction());
    auto const screen = h.mount(
        R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
        R"("state":{"f":{"init":{"lit":{"columns":{"status":{"include":[{"eq":"open"}]}}}}}},)"
        R"("view":{"kind":"column","children":[{"kind":"table","id":"t","testId":"t","model":"sample",)"
        R"("table":"samples","selectionMode":"multiple","filters":{"ref":"f"}},)"
        R"({"kind":"button","testId":"clear","label":"clear","onClick":[{"set":"f","value":{"lit":{"columns":{}}}}]}]}})");
    replyDescribe(h, describeJson(selectionDescription()));
    replyPage(h, selectionPage(openRows(), 6, "v1"));
    int const table = h.byTestId("Table", "t");
    h.backend.selectAll(table);
    h.settle();
    h.backend.clickHeader(table, 1);  // a header sort
    h.settle();
    CHECK(elementValue(h, "t", "selectionSet").field("view") != nullptr);
    h.backend.click(h.byTestId("Button", "clear"));  // a filter change
    h.settle();
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[]})"));
}

// Mutation: the selection ended by a sort, which empties it on the first step.

TEST_CASE("view selection: select-all before the new filter's reply is refused", "[ui][table][selection]") {
    TableHarness h;
    addAction(h, "Lims_TagRows", std::string{kSelectionSchema}, samplesAction());
    auto const screen = h.mount(
        R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
        R"("state":{"f":{"init":{"lit":{"columns":{"status":{"include":[{"eq":"open"}]}}}}}},)"
        R"("view":{"kind":"column","children":[{"kind":"table","id":"t","testId":"t","model":"sample",)"
        R"("table":"samples","selectionMode":"multiple","filters":{"ref":"f"}},)"
        R"({"kind":"button","testId":"clear","label":"clear","onClick":[{"set":"f","value":{"lit":{"columns":{}}}}]}]}})");
    replyDescribe(h, describeJson(selectionDescription()));
    replyPage(h, selectionPage(openRows(), 6, "v1"));
    int const table = h.byTestId("Table", "t");
    h.backend.click(h.byTestId("Button", "clear"));  // a filter change, its reply not in yet
    h.settle();
    h.backend.selectAll(table);
    h.settle();
    CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[]})"));
    CHECK(statusOf(h, table).message == "Select all is available once the rows have loaded");
    replyPage(
        h,
        selectionPage(
            {{.key = 0}, {.key = 1}, {.key = 2}, {.key = 3}, {.key = 4}, {.key = 5}, {.key = 6, .status = "closed"}},
            7, "v2"));
    h.backend.selectAll(table);
    h.settle();
    CHECK(*elementValue(h, "t", "selectionSet").field("view")->field("revision") == Value{std::string{"v2"}});
}

// Mutation: setFilter keeping the old filter's revision, which selects the new filter's view under revision v1.

TEST_CASE("view selection: only the table whose view was refused offers it again", "[ui][table][selection]") {
    TableHarness h;
    addAction(h, "Lims_TagRows", std::string{kSelectionSchema}, samplesAction());
    std::string const other =
        R"({"kind":"table","id":"u","testId":"u","model":"sample","table":"samples","selectionMode":"multiple",)"
        R"("filters":{"columns":{"status":{"include":[{"eq":"closed"}]}}}})";
    auto const screen = h.mount(selectionDoc(std::string{kSelectionTable} + "," + other, kSelectionMutations, h));
    while (openCalls(h, "Table.samples.describe") > 0) {
        replyDescribe(h, describeJson(selectionDescription()));
    }
    while (openCalls(h, "Table.samples") > 0) {
        std::size_t const call = oldestOpen(h, "Table.samples");
        bool const closed = queryOf(h, call).filters.columns.at("status").include.front().eq == "closed";
        std::string page;
        if (closed) {
            page = selectionPage({{.key = 6, .status = "closed"},
                                  {.key = 7, .status = "closed"},
                                  {.key = 8, .status = "closed"},
                                  {.key = 9, .status = "closed"}},
                                 4, "c1");
        } else {
            page = selectionPage(openRows(), 6, "v1");
        }
        h.reply(call, std::move(page));
    }
    int const t = h.byTestId("Table", "t");
    int const u = h.byTestId("Table", "u");
    h.backend.selectAll(t);
    h.settle();
    h.backend.selectAll(u);
    h.settle();
    runMutation(h, "tag");  // over t's view
    refuseMutation(h, "tag", "morph.table.viewChanged");
    CHECK(statusOf(h, t).entries.size() == 1);
    CHECK(statusOf(h, u).message.empty());
    CHECK(statusOf(h, u).entries.empty());
}

// Mutation: the rerun offered by every table of the record, which puts "Run on these" on u over its own view.

TEST_CASE("view selection: not while membership is open", "[ui][table][selection]") {
    TableHarness h;
    SECTION("rows are undecided") {
        auto const screen = mountSelection(h);
        int const table = h.byTestId("Table", "t");
        fireEvent(h, "Lims_SampleModel.samples", {});
        replyPage(h, selectionReply(openRows(), 6, "v1", {}, false, R"("undecided":40,"totalKind":"atLeast")"));
        h.backend.selectAll(table);
        h.settle();
        CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[]})"));
        CHECK(statusOf(h, table).message == "Select all is available once 40 rows are decided");
    }
    SECTION("the filtered column's level is awaited") {
        addAction(h, "Lims_TagRows", std::string{kSelectionSchema}, samplesAction());
        auto const described = withLevel(selectionDescription(), "status", "recipe");
        auto const screen = h.mount(selectionDoc(kSelectionTable, kSelectionMutations, h));
        replyDescribe(h, describeJson(described));
        replyPage(h, selectionReply(openRows(), 6, "v1", {}, false, R"("awaiting":["recipe"])", described));
        int const table = h.byTestId("Table", "t");
        h.backend.selectAll(table);
        h.settle();
        CHECK(elementValue(h, "t", "selectionSet") == value(R"({"keys":[]})"));
        CHECK(statusOf(h, table).message == "Select all is available once recipe data has loaded");
    }
    SECTION("only a column the filter does not read has the level") {
        addAction(h, "Lims_TagRows", std::string{kSelectionSchema}, samplesAction());
        auto const described = withLevel(selectionDescription(), "lockedBy", "recipe");
        auto const screen = h.mount(selectionDoc(kSelectionTable, kSelectionMutations, h));
        replyDescribe(h, describeJson(described));
        replyPage(h, selectionReply(openRows(), 6, "v1", {}, false, R"("awaiting":["recipe"])", described));
        int const table = h.byTestId("Table", "t");
        h.backend.selectAll(table);
        h.settle();
        CHECK(elementValue(h, "t", "selectionSet").field("view") != nullptr);
    }
}

// Mutation: select-all ignoring `undecided`, which selects a view whose rows are not known.

TEST_CASE("view selection: the rows changed since the user selected them", "[ui][table][selection]") {
    TableHarness h;
    auto const screen = mountSelection(h);
    int const table = h.byTestId("Table", "t");
    h.backend.selectAll(table);
    h.settle();
    h.backend.ctrlClickRow(table, std::int64_t{3});
    h.settle();
    runMutation(h, "tag");
    REQUIRE(h.dispatcher.count("Lims_TagRows") == 1);
    // Seven rows now: a change of the record refetches the page.
    fireEvent(h, "Lims_SampleModel.samples", {});
    replyPage(h, selectionPage({{.key = 0}, {.key = 1}, {.key = 2}, {.key = 3}, {.key = 4}, {.key = 5}, {.key = 6}}, 7,
                               "v2"));
    refuseMutation(h, "tag", "morph.table.viewChanged");
    TableStatus const status = statusOf(h, table);
    CHECK(status.message == "The rows changed since you selected them: 6 rows now. Run on these?");
    REQUIRE(status.entries.size() == 1);
    CHECK(status.entries.front().label == "Run on these");
    CHECK(h.dispatcher.count("Lims_TagRows") == 1);  // not choosing it sends nothing
    h.backend.chooseStatusEntry(table, status.entries.front().id);
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_TagRows") == 2);
    Value const body = value(h.dispatcher.calls.at(h.dispatcher.last("Lims_TagRows")).body);
    CHECK(*body.field("rows")->field("view")->field("revision") == Value{std::string{"v2"}});
    CHECK(*body.field("rows")->field("except") == value("[3]"));
}

// Mutation: the rerun sending the old revision, which is refused again.

TEST_CASE("view selection: a Shift range toggles a range of except", "[ui][table][selection]") {
    TableHarness h;
    auto const screen = mountSelection(h);
    int const table = h.byTestId("Table", "t");
    auto const except = [&h] { return *elementValue(h, "t", "selectionSet").field("except"); };
    h.backend.selectAll(table);
    h.settle();
    h.backend.ctrlClickRow(table, std::int64_t{1});  // anchor 1
    h.settle();
    CHECK(except() == value("[1]"));
    h.backend.selectTableRange(table, 1, 3);  // keys 1, 2, 3 through keysOf
    h.settle();
    CHECK(except() == value("[1,2,3]"));
    CHECK(elementValue(h, "t", "selectedCount") == morph::ui::Value{std::int64_t{3}});
    h.backend.ctrlClickRow(table, std::int64_t{5});
    h.settle();
    h.backend.ctrlClickRow(table, std::int64_t{5});  // anchor 5, selected again
    h.settle();
    h.backend.selectTableRange(table, 5, 2);  // keys 2..5
    h.settle();
    CHECK(except() == value("[1]"));
    CHECK(elementValue(h, "t", "selectedCount") == morph::ui::Value{std::int64_t{5}});
    // Over maxRangeRows (10,000): refused, except unchanged.
    h.backend.selectTableRange(table, 0, 10'000);
    h.settle();
    CHECK(except() == value("[1]"));
    CHECK(statusOf(h, table).message == "A range selects at most 10,000 rows");
}

// Mutation: a Shift range in a view selection replacing the selection with the range's keys.

TEST_CASE("view selection: copying it fetches the view's rows in order", "[ui][table][selection]") {
    TableHarness h;
    auto const screen = mountSelection(h);
    int const table = h.byTestId("Table", "t");
    h.backend.selectAll(table);
    h.settle();
    h.backend.ctrlClickRow(table, std::int64_t{1});
    h.settle();
    h.backend.ctrlClickRow(table, std::int64_t{4});
    h.settle();
    std::size_t const sent = h.dispatcher.count("Table.samples");
    pressCopy(h, table);
    REQUIRE(h.dispatcher.count("Table.samples") == sent + 1);
    auto const query = lastQuery(h);
    CHECK_FALSE(query.keys.has_value());
    REQUIRE(query.columns.has_value());
    CHECK(*query.columns == std::vector<std::string>{"id", "status"});  // the visible columns
    // The fetched rows say "copied" where the held rows say "open": the copy writes what the page queries brought.
    replyPage(h, selectionPage({{.key = 0, .status = "copied"},
                                {.key = 1, .status = "copied"},
                                {.key = 2, .status = "copied"},
                                {.key = 3, .status = "copied"},
                                {.key = 4, .status = "copied"},
                                {.key = 5, .status = "copied"}},
                               6, "v1"));
    CHECK(clipboardText(h) == "0\tcopied\n2\tcopied\n3\tcopied\n5\tcopied");
    SECTION("over maxCopyRows it is refused") {
        std::string const kept = clipboardText(h);
        fireEvent(h, "Lims_SampleModel.samples", {});
        replyPage(h, selectionPage(openRows(), 10'001, "v3"));
        h.backend.selectAll(table);
        h.settle();
        pressCopy(h, table);
        CHECK(statusOf(h, table).message == "A copy holds at most 10,000 rows");
        CHECK(clipboardText(h) == kept);
    }
}
