// SPDX-License-Identifier: Apache-2.0

// A model table's cell menu and header gestures at their edges: a cell of a column the table lacks, a column that does
// not filter, repeated exclusions, and a header the document disables.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/ui/mount.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
using morph::ui::Key;
using morph::ui::MenuEntry;
using morph::ui::testing::columnIndex;
using morph::ui::testing::describeJson;
using morph::ui::testing::elementValue;
using morph::ui::testing::entryOf;
using morph::ui::testing::labelsOf;
using morph::ui::testing::lastQuery;
using morph::ui::testing::oldestOpen;
using morph::ui::testing::openCalls;
using morph::ui::testing::queryOf;
using morph::ui::testing::readJson;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::replyPage;
using morph::ui::testing::statusOf;
using morph::ui::testing::TableHarness;

// `id` (the key), `mass` (a decimal), and `companyA` and `companyB`, the declared group `company` ("Company").
table::TableDescription cellDescription() {
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

using CellRow = std::map<std::string, table::Cell, std::less<>>;

// Rows 1 to 4 weigh 0.1 to 0.4, row 5 has no mass; row 1 is of companies E and G, row 2 of F.
std::vector<CellRow> cellRows() {
    auto const mass = [](std::string_view text) { return table::Cell{table::parseDecimal(text).value()}; };
    auto const text = [](std::string value) { return table::Cell{std::move(value)}; };
    return {
        CellRow{{"id", table::Cell{std::int64_t{1}}},
                {"mass", mass("0.1")},
                {"companyA", text("E")},
                {"companyB", text("G")}},
        CellRow{{"id", table::Cell{std::int64_t{2}}}, {"mass", mass("0.2")}, {"companyA", text("F")}},
        CellRow{{"id", table::Cell{std::int64_t{3}}}, {"mass", mass("0.3")}},
        CellRow{{"id", table::Cell{std::int64_t{4}}}, {"mass", mass("0.4")}},
        CellRow{{"id", table::Cell{std::int64_t{5}}}},
    };
}

std::string cellPageOf(std::vector<std::string> columns) {
    std::vector<CellRow> const rows = cellRows();
    table::GenericPage page;
    page.revision = cellDescription().revision;
    page.columns = std::move(columns);
    page.total = static_cast<std::int64_t>(rows.size());
    page.totalKind = table::TotalKind::Exact;
    page.offset = 0;
    for (CellRow const& row : rows) {
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

// Answers every open `Table.samples` call with the rows, each by the columns its own query names.
void answerAll(TableHarness& h) {
    for (int round = 0; round < 16 && openCalls(h, "Table.samples") > 0; ++round) {
        std::size_t const index = oldestOpen(h, "Table.samples");
        h.reply(index, cellPageOf(queryOf(h, index).columns.value_or(std::vector<std::string>{})));
    }
    REQUIRE(openCalls(h, "Table.samples") == 0);
}

// A document whose table `t` over the samples carries @p tableKeys, beside a button `other` that sets `idle`; the
// state `picked` and `idle` (false) are the cases' to set and read.
std::string cellDoc(std::string_view tableKeys) {
    std::string tableText = R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",)"
                            R"("selectionMode":"multiple")";
    if (!tableKeys.empty()) {
        tableText += "," + std::string{tableKeys};
    }
    tableText += '}';
    return R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
           R"("state":{"picked":{"init":{"lit":null}},"idle":{"init":{"lit":false}}},)"
           R"("view":{"kind":"column","children":[)" +
           tableText +
           R"(,{"kind":"button","testId":"other","label":"Other","onClick":[{"set":"idle","value":true}]}]}})";
}

// The table mounted over the five rows.
struct Mounted {
    explicit Mounted(std::string_view tableKeys) : screen{h.mount(cellDoc(tableKeys))} {
        replyDescribe(h, describeJson(cellDescription()));
        replyPage(h, cellPageOf(lastQuery(h).columns.value_or(std::vector<std::string>{})));
        table = h.byTestId("Table", "t");
    }

    TableHarness h;
    std::unique_ptr<morph::ui::Screen> screen;
    int table = 0;
};

table::FilterSpec filtersOf(morph::ui::Screen const& screen) {
    table::FilterSpec out;
    std::string const json = elementValue(screen, "t", "filters");
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(out, json));
    return out;
}

Key keyOf(std::int64_t key) { return Key{key}; }

}  // namespace

TEST_CASE("covi cell menu: a cell of a column the table does not have offers copying and no filtering",
          "[covi][ui][table][menu]") {
    Mounted m{R"("cellMenu":[{"label":"Column","onClick":[{"set":"picked","value":{"ref":"column"}}]},)"
              R"({"label":"Value","onClick":[{"set":"picked","value":{"ref":"value"}}]}])"};
    std::vector<MenuEntry> const entries = m.h.backend.rightClickCell(m.table, keyOf(2), 99);
    CHECK(labelsOf(entries) == std::vector<std::string>{"Copy", "Copy with headers", "Filter by this value",
                                                        "Exclude this value", "Column", "Value"});
    CHECK(entryOf(entries, "Copy").enabled);
    CHECK_FALSE(entryOf(entries, "Filter by this value").enabled);
    CHECK_FALSE(entryOf(entries, "Exclude this value").enabled);
    m.h.backend.chooseCellMenu(m.table, keyOf(2), 99, {"Column"});
    m.h.settle();
    CHECK(readJson(*m.screen, R"({"ref":"picked"})") == R"("")");
    m.h.backend.chooseCellMenu(m.table, keyOf(2), 99, {"Value"});
    m.h.settle();
    CHECK(readJson(*m.screen, R"({"ref":"picked"})") == "null");
}

TEST_CASE("covi cell menu: a column that says filterable false offers its cells no filter entry enabled",
          "[covi][ui][table][menu]") {
    Mounted m{R"("columns":[{"id":"id"},{"id":"mass","filterable":false}])"};
    std::size_t const mass = columnIndex(m.h, "mass");
    std::size_t const id = columnIndex(m.h, "id");
    std::vector<MenuEntry> const onMass = m.h.backend.rightClickCell(m.table, keyOf(2), mass);
    CHECK_FALSE(entryOf(onMass, "Filter by this value").enabled);
    CHECK_FALSE(entryOf(onMass, "Exclude this value").enabled);
    std::vector<MenuEntry> const onId = m.h.backend.rightClickCell(m.table, keyOf(2), id);
    CHECK(entryOf(onId, "Filter by this value").enabled);
    CHECK(entryOf(onId, "Exclude this value").enabled);
}

TEST_CASE("covi cell menu: excluding values merges them into one list and an emptiness joins it once",
          "[covi][ui][table][menu]") {
    Mounted m{""};
    std::size_t const mass = columnIndex(m.h, "mass");
    auto const exclude = [&](std::int64_t key) {
        m.h.backend.chooseCellMenu(m.table, keyOf(key), mass, {"Exclude this value"});
        answerAll(m.h);
    };
    exclude(3);
    exclude(3);  // already listed: not listed twice
    auto const excluded = [&] { return filtersOf(*m.screen).columns.at("mass").exclude; };
    REQUIRE(excluded().size() == 1);
    CHECK(excluded().front().oneOf == std::optional{std::vector<std::string>{"0.3"}});
    exclude(5);  // an empty cell: an emptiness beside the values
    REQUIRE(excluded().size() == 2);
    CHECK(excluded().back().isEmpty);
    exclude(5);  // already listed: not listed twice
    CHECK(excluded().size() == 2);
    exclude(4);  // joins the values' list, not a new one
    REQUIRE(excluded().size() == 2);
    CHECK(excluded().front().oneOf == std::optional{std::vector<std::string>{"0.3", "0.4"}});
}

TEST_CASE(
    "covi cell menu: a table whose header is disabled refuses each gesture in the status line until it is enabled",
    "[covi][ui][table][menu]") {
    Mounted m{R"("headerEnabled":{"ref":"idle"})"};
    std::string const busy = "Sorting, filtering and arranging are unavailable right now";
    std::size_t const mass = columnIndex(m.h, "mass");
    std::size_t const companyA = columnIndex(m.h, "companyA");
    auto const refused = [&](auto&& gesture) {
        // The status line is cleared first, so a message read afterwards is the gesture's own.
        m.h.backend.pressCopy(m.table);
        m.h.settle();
        gesture();
        m.h.settle();
        return statusOf(m.h, m.table).message == busy;
    };
    SECTION("disabled") {
        CHECK(refused([&] { m.h.backend.clickHeader(m.table, mass); }));
        CHECK(refused([&] { m.h.backend.moveColumn(m.table, mass, 0); }));
        CHECK(refused([&] { m.h.backend.resizeColumn(m.table, mass, 12); }));
        CHECK(refused([&] { m.h.backend.clickFilter(m.table, mass); }));
        // The column's own menu is drawn greyed, so choosing from it reports nothing.
        CHECK_FALSE(entryOf(m.h.backend.menuOf(m.table, mass), "Sort ascending").enabled);
        CHECK(m.h.backend.prop(m.table, "sort") == "none");
        // The table's menu offers copying alone while the header is disabled.
        std::vector<MenuEntry> const menu = m.h.backend.menuOf(m.table, std::nullopt);
        REQUIRE_FALSE(menu.empty());
        for (MenuEntry const& entry : menu) {
            if (entry.separator || entry.heading) {
                continue;
            }
            INFO(entry.label);
            CHECK(entry.enabled == (entry.label == "Copy" || entry.label == "Copy with headers"));
        }
        // A cell's filter entries are disabled with the header.
        std::vector<MenuEntry> const cell = m.h.backend.rightClickCell(m.table, keyOf(2), mass);
        CHECK_FALSE(entryOf(cell, "Filter by this value").enabled);
        std::vector<MenuEntry> const inGroup = m.h.backend.rightClickCell(m.table, keyOf(2), companyA);
        CHECK_FALSE(entryOf(inGroup, "Filter by this value in any Company").enabled);
    }
    SECTION("enabled") {
        m.h.backend.click(m.h.byTestId("Button", "other"));
        m.h.settle();
        CHECK_FALSE(refused([&] { m.h.backend.clickHeader(m.table, mass); }));
        CHECK(m.h.backend.prop(m.table, "sort") == "1:Ascending");
        CHECK_FALSE(refused([&] { m.h.backend.resizeColumn(m.table, mass, 12); }));
        CHECK_FALSE(refused([&] { m.h.backend.clickFilter(m.table, mass); }));
        CHECK_FALSE(refused([&] { m.h.backend.moveColumn(m.table, mass, 0); }));
    }
}

TEST_CASE("covi cell menu: an entry that reads its row whole has every column fetched and says so once",
          "[covi][ui][table][menu]") {
    Mounted m{R"("cellMenu":[{"label":"Whole","onClick":[{"set":"picked","value":{"ref":"row"}}]}])"};
    CHECK(morph::ui::testing::diagnosticsMentioning(m.h, "reads its row whole") == 1);
    std::vector<std::string> const fetched = lastQuery(m.h).columns.value_or(std::vector<std::string>{});
    for (std::string const column : {"id", "mass", "companyA", "companyB"}) {
        INFO(column);
        CHECK(std::ranges::contains(fetched, column));
    }
    m.h.backend.chooseCellMenu(m.table, keyOf(2), 1, {"Whole"});
    m.h.settle();
    std::string const picked = readJson(*m.screen, R"({"ref":"picked"})");
    CHECK(picked.contains(R"("id":2)"));
    CHECK(picked.contains(R"("companyA":"F")"));
}

TEST_CASE("covi cell menu: entries in a submenu, objects and a read through get fetch only the fields they name",
          "[covi][ui][table][menu]") {
    Mounted m{R"("columns":[{"id":"id"}],)"
              R"("cellMenu":[{"label":"More","items":[{"label":"Inner","onClick":[{"set":"picked","value":)"
              R"({"obj":{"id":{"ref":"row.id"},"company":{"ref":"row.companyA"},)"
              R"("again":{"get":{"ref":"row"},"path":"mass"}}}}]}]}])"};
    CHECK(morph::ui::testing::diagnosticsMentioning(m.h, "reads its row whole") == 0);
    std::vector<std::string> const fetched = lastQuery(m.h).columns.value_or(std::vector<std::string>{});
    CHECK(std::ranges::contains(fetched, std::string{"companyA"}));
    CHECK_FALSE(std::ranges::contains(fetched, std::string{"companyB"}));
    m.h.backend.chooseCellMenu(m.table, keyOf(2), 0, {"More", "Inner"});
    m.h.settle();
    std::string const picked = readJson(*m.screen, R"({"ref":"picked"})");
    CHECK(picked.contains(R"("id":2)"));
    CHECK(picked.contains(R"("company":"F")"));
}

TEST_CASE("covi cell menu: two entries that give the same keys answer the key once, a submenu's keys too",
          "[covi][ui][table][menu]") {
    Mounted m{R"("cellMenu":[{"label":"A","keys":"Ctrl+O","onClick":[{"set":"picked","value":{"lit":"a"}}]},)"
              R"({"label":"B","keys":"Ctrl+O","onClick":[{"set":"picked","value":{"lit":"b"}}],)"
              R"("items":[{"label":"C","keys":"Ctrl+P","onClick":[{"set":"picked","value":{"lit":"c"}}]}]}])"};
    m.h.backend.focusMove(m.table, morph::ui::FocusMove::Down);
    m.h.backend.focusMove(m.table, morph::ui::FocusMove::Down);
    CHECK(m.h.backend.press(m.table, "Ctrl+O"));
    CHECK(readJson(*m.screen, R"({"ref":"picked"})") == R"("a")");
    CHECK(m.h.backend.press(m.table, "Ctrl+P"));
    CHECK(readJson(*m.screen, R"({"ref":"picked"})") == R"("c")");
}

TEST_CASE("covi cell menu: a locked column is not hidden by its header menu and the status line says so",
          "[covi][ui][table][menu]") {
    Mounted m{R"("columns":[{"id":"id","locked":true},{"id":"mass"}])"};
    REQUIRE(elementValue(*m.screen, "t", "visibleColumns") == R"(["id","mass"])");
    m.h.backend.chooseMenu(m.table, 0, {"Hide column"});
    m.h.settle();
    CHECK(elementValue(*m.screen, "t", "visibleColumns") == R"(["id","mass"])");
    m.h.backend.chooseMenu(m.table, 1, {"Hide column"});
    m.h.settle();
    CHECK(elementValue(*m.screen, "t", "visibleColumns") == R"(["id"])");
}

TEST_CASE("covi cell menu: a bound filter is the document's, the gesture only tells it", "[covi][ui][table][menu]") {
    TableHarness h;
    std::string doc = cellDoc(R"("filters":{"ref":"f"},"onFiltersChange":[{"set":"seen","value":{"ref":"event"}}])");
    doc.replace(doc.find(R"("state":{)") + 9, 0, R"("f":{"init":{"lit":{"columns":{}}}},"seen":{"init":{"lit":null}},)");
    auto const screen = h.mount(doc);
    replyDescribe(h, describeJson(cellDescription()));
    replyPage(h, cellPageOf(lastQuery(h).columns.value_or(std::vector<std::string>{})));
    int const table = h.byTestId("Table", "t");
    h.backend.chooseCellMenu(table, keyOf(2), 1, {"Filter by this value"});
    h.settle();
    CHECK(readJson(*screen, R"({"ref":"seen"})").contains(R"("mass")"));
    CHECK(readJson(*screen, R"({"ref":"f"})") == R"({"columns":{}})");
}

TEST_CASE("covi cell menu: a level that failed is named by its first letter capitalised", "[covi][ui][table]") {
    struct Case {
        char const* level;
        char const* state;
    };
    for (Case const& item : std::vector<Case>{{"recipe", "Recipe data could not be loaded."},
                                              {"Assay", "Assay data could not be loaded."},
                                              {"~raw", "~raw data could not be loaded."}}) {
        INFO(item.level);
        TableHarness h;
        auto const screen = h.mount(R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
                                    R"("view":{"kind":"table","id":"t","testId":"t","model":"sample","table":"samples"}})");
        replyDescribe(h, morph::ui::testing::withLevel(morph::ui::testing::samplesDescription(3), "c3", item.level));
        replyPage(h, morph::ui::testing::pageJsonUnloaded(
                         h.described.revision, 0, 20, 20, h.columns,
                         {{.index = 1, .levels = {item.level}, .failed = {item.level}}}, {item.level}));
        CHECK(statusOf(h, h.byTestId("Table", "t")).state == item.state);
    }
}

TEST_CASE("covi cell menu: a field a cell reads only through the whole row is fetched once it is read",
          "[covi][ui][table]") {
    TableHarness h;
    auto const screen = h.mount(cellDoc(
        R"("columns":[{"id":"id"},{"id":"mass","tooltip":{"get":{"row":""},"path":"companyA"}}])"));
    replyDescribe(h, describeJson(cellDescription()));
    std::vector<std::string> const first = lastQuery(h).columns.value_or(std::vector<std::string>{});
    CHECK_FALSE(std::ranges::contains(first, std::string{"companyA"}));
    replyPage(h, cellPageOf(first));
    h.settle();
    std::size_t const asked = h.dispatcher.count("Table.samples");
    REQUIRE(openCalls(h, "Table.samples") >= 1);
    std::vector<std::string> const then = lastQuery(h).columns.value_or(std::vector<std::string>{});
    CHECK(std::ranges::contains(then, std::string{"companyA"}));
    answerAll(h);
    CHECK(h.dispatcher.count("Table.samples") == asked);  // the field arrived: nothing is asked again
}

TEST_CASE("covi cell menu: a field the rows still lack after it was asked for is not asked for twice",
          "[covi][ui][table]") {
    TableHarness h;
    auto const screen = h.mount(cellDoc(
        R"("columns":[{"id":"id"},{"id":"mass","tooltip":{"get":{"row":""},"path":"companyA"}}])"));
    replyDescribe(h, describeJson(cellDescription()));
    std::vector<std::string> const first = lastQuery(h).columns.value_or(std::vector<std::string>{});
    replyPage(h, cellPageOf(first));
    h.settle();
    REQUIRE(openCalls(h, "Table.samples") >= 1);
    std::size_t const asked = h.dispatcher.count("Table.samples");
    // The reply leaves the field out again: reading it still finds nothing.
    replyPage(h, cellPageOf(first));
    h.settle();
    std::vector<std::string> const named = lastQuery(h).columns.value_or(std::vector<std::string>{});
    CHECK(std::ranges::count(named, std::string{"companyA"}) == 1);
    CHECK(h.dispatcher.count("Table.samples") <= asked + 1);
}
