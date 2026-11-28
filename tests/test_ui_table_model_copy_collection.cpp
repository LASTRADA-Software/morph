// SPDX-License-Identifier: Apache-2.0

// Copying cells, collections as tables, and display units.
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
using morph::ui::testing::describeJson;
using morph::ui::testing::keyedJsonAt;
using morph::ui::testing::pageJsonAt;
using morph::ui::testing::RowsFixture;
using morph::ui::testing::samplesDescription;
using morph::ui::testing::TestRowsSource;
using namespace std::chrono_literals;
using morph::ui::HeaderClick;
using morph::ui::Key;
using morph::ui::SelectionChange;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::clickFilter;
using morph::ui::testing::clipboardText;
using morph::ui::testing::elementValue;
using morph::ui::testing::kCollectionBesideTable;
using morph::ui::testing::kConfigModelTable;
using morph::ui::testing::kMasterDetailSamples;
using morph::ui::testing::kMixedSamplesView;
using morph::ui::testing::kNine;
using morph::ui::testing::kSamplesCollection;
using morph::ui::testing::labelsOf;
using morph::ui::testing::lastQuery;
using morph::ui::testing::loads;
using morph::ui::testing::menuOf;
using morph::ui::testing::pressCopy;
using morph::ui::testing::queryOf;
using morph::ui::testing::readJson;
using morph::ui::testing::refusalOf;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::replyPage;
using morph::ui::testing::rowKeys;
using morph::ui::testing::statusOf;
using morph::ui::testing::TableHarness;
using morph::ui::testing::withLevel;
using morph::ui::testing::withTableKey;

// A user-formed group has no values list, and its filter names its columns. Mutation: removing the group leaving its
// filter held (the last CHECK: the groups still hold it).

TEST_CASE("filter menu: a user-formed group offers conditions and the match line, no values list",
          "[ui][table][config]") {
    TableHarness h;
    auto const screen = h.mount(kKinds);
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
    CHECK(
        readJson(*screen, R"({"ref":"t.filters.groups"})") ==
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
    auto const screen = mountDescribed(h, described);
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
        toggleBox(h, "table_filter_value_3");                                       // March
        CHECK(propOf(h, "Checkbox", "table_filter_value_4", "checked") == "true");  // its first day, covered
        CHECK(propOf(h, "Checkbox", "table_filter_value_4", "enabled") == "false");
        toggleBox(h, "table_filter_value_0");  // 2025
        pressButton(h, "table_filter_apply");
        CHECK(readJson(*screen, R"({"ref":"t.filters.columns.c3"})") ==
              R"({"combine":"any","exclude":[],"include":[{"between":["2025-01-01","2025-12-31"]},)"
              R"({"between":["2026-03-01","2026-03-31"]}]})");
        clickFilter(h, table, 2);
        answer(years);
        pressButton(h, "table_filter_open_1");
        answer(months);
        CHECK(propOf(h, "Checkbox", "table_filter_value_0", "checked") == "true");   // 2025
        CHECK(propOf(h, "Checkbox", "table_filter_value_1", "checked") == "false");  // 2026
        CHECK(propOf(h, "Checkbox", "table_filter_value_3", "checked") == "true");   // March
    }
    // Mutation: a bucket a merged range covers shown disabled (the range is no listed bucket's own tick).
    SECTION("buckets Apply merged come back ticked and can be unticked") {
        openTree();
        toggleBox(h, "table_filter_value_3");   // March
        toggleBox(h, "table_filter_value_35");  // April, after March's days
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
        toggleBox(h, "table_filter_value_4");  // untick April
        pressButton(h, "table_filter_apply");
        CHECK(readJson(*screen, R"({"ref":"t.filters.columns.c3.include"})") ==
              R"([{"between":["2026-03-01","2026-03-31"]}])");
    }
    SECTION("touching days are one entry") {
        openTree();
        pressButton(h, "table_filter_open_35");  // April, after March's days
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
        pressButton(h, "table_filter_open_66");  // May, after April's days
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
    auto const screen = mountDescribed(h, kindsDescription());
    int const table = h.backend.all("Table").front();
    std::size_t const pages = pageCalls(h);
    chooseMenu(h, table, 6, {"Display unit", "mass: g"});
    CHECK(readJson(*screen, R"({"ref":"t.layout.units"})") == R"({"weight":{"mass":"g"}})");
    CHECK(pageCalls(h) == pages);
    chooseMenu(h, table, 6, {"Display unit", "mass: g"});  // the checked one clears it
    CHECK(readJson(*screen, R"({"ref":"t.layout.units"})") == "null");
}

// Mutation: the entry labelled with the reply's `text`.

TEST_CASE("filter menu: the values list labels by the column's display", "[ui][table][config]") {
    TableHarness h;
    auto const screen = mountDescribed(h, companionDescription(), doc(kTable));
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
    auto const screen = h.mount(withTableKey(kStatusRows, R"("configurable":false)"));
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
    auto const screen =
        mountDescribed(h, samplesDescription(5),
                       R"({"vocab":"ui/1","id":"samples","models":{"sample":{"type":"Lims_SampleModel"}},)"
                       R"("view":{"kind":"localeScope","locale":"de","children":[)"
                       R"({"kind":"table","id":"t","model":"sample","table":"samples"}]}})");
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    h.reply(lastDistinct(h),
            distinctReply(h.described.revision, R"({"values":[{"text":"x","operand":"x","count":1234}],"distinct":1,)"
                                                R"("invalid":1234})"));
    CHECK(propOf(h, "Checkbox", "table_filter_value_0", "label") == "x (1.234)");
    CHECK(h.text("table_filter_note") == "1.234 cells could not be read");
}

// Mutation: a reply asked for before the record changed kept as fresh (the reopened menu sends nothing).

TEST_CASE("filter menu: a values list asked for before a change is not kept", "[ui][table][config]") {
    TableHarness h;
    auto const screen = mountDescribed(h, samplesDescription(5));
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
    CHECK(h.dispatcher.count("Table.samples") == kept);  // a reply after the change is kept
}

// Mutation: a user-formed group's menu asking for its values (a distinct request with the group's layout id).

TEST_CASE(
    "filter menu: a user-formed group of a model's table sends no values request and its filter names its columns",
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
    auto const screen = mountDescribed(h, pair);
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
    CHECK(h.dispatcher.count("Table.samples") == calls);  // no values request
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
    h.reply(lastDistinct(h),
            distinctReply(pair.revision, R"({"values":[{"text":"x","operand":"x","count":1}],"distinct":1})"));
    selectAll(h, false);
    typeInto(h, "table_filter_search", "x");
    h.advance(250ms);
    h.reply(lastDistinct(h),
            distinctReply(pair.revision, R"({"values":[{"text":"x","operand":"x","count":1}],"distinct":1})"));
    selectAll(h, true);
    pressButton(h, "table_filter_apply");
    REQUIRE(lastQuery(h).distinct.has_value());
    CHECK(lastQuery(h).distinct->limit == 1000);
    CHECK(lastQuery(h).distinct->search == "x");
}

// Mutation: the one-kind refusal dropped (a group of a text and an integer column loads).

TEST_CASE("ui document: a rows table's groups are 2 to 16 of its column parts, of one kind, each in one group",
          "[ui][document]") {
    auto const with = [](std::string_view groups) {
        return withTableKey(kKinds, R"("groups":)" + std::string{groups});
    };
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
    CHECK(refusalOf(with(R"([{"id":"g","columns":{"lit":["a","n"]}}])")) ==
          "a group's columns hold one kind of value");
}

// Mutation: WindowRows compiling a filter without the description's units (the measure operand is refused).

TEST_CASE("window rows: a filter on a measure reads its operand with the description's units", "[ui][table]") {
    RowsFixture h;
    table::TableDescription d = samplesDescription(2);
    d.columns[1].info = table::ColumnInfo{.id = "weight", .kind = table::ColumnKind::Measure};
    d.units = {
        table::UnitInfo{
            .id = "kg", .symbol = "kg", .dimension = "mass", .toBase = *table::parseDecimal("1"), .decimals = 3},
        table::UnitInfo{
            .id = "g", .symbol = "g", .dimension = "mass", .toBase = *table::parseDecimal("0.001"), .decimals = 0}};
    auto const rows = h.windowRows("samples", table::describe(d));
    table::FilterEntry above;
    above.gt = "0.3 kg";
    table::FilterSpec filter;
    filter.columns.insert_or_assign(
        "weight", table::ColumnFilter{.combine = table::Combine::Any, .include = {above}, .exclude = {}});
    auto const set = rows->setFilter(filter);
    INFO((set ? std::string{} : set.error().message));
    CHECK(set.has_value());
}

// Mutation: the filter menu holding the mount's filter path strongly (the menu, kept by the element, keeps the
// mount's scope alive, and the runtime aborts at teardown: "Runtime destroyed while nodes are alive").

TEST_CASE("filter menu: a table whose menu set a filter tears down cleanly", "[ui][table][config]") {
    {
        TableHarness h;
        auto const screen = h.mount(kStatusRows);
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
    auto const screen = mountDescribed(h, samplesDescription(5));
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
    typeInto(h, "table_filter_search", "s0");  // the oldest but one: kept
    h.advance(250ms);
    CHECK(h.dispatcher.count("Table.samples") == calls);
    typeInto(h, "table_filter_search", "");  // the oldest: gone
    h.advance(250ms);
    CHECK(h.dispatcher.count("Table.samples") == calls + 1);
}

// ── Copy ────────────────────────────────────────────────────────────────────

// Mutation: `fetchRows` giving a row its place among the keys asked for instead of its view row.

TEST_CASE("engine rows: fetchRows reads the rows of keys at once, a key the view lacks left out", "[ui][table]") {
    RowsFixture h;
    auto const source = std::make_shared<TestRowsSource>(10);
    EngineRows rows{source, morph::ui::testing::TestRowsSource::recordOf(), table::EngineOptions{.owner = &h.owner}};
    REQUIRE(rows.setSort({{.column = "id", .dir = table::SortDirection::Descending}}).has_value());
    h.settle();
    std::optional<std::expected<std::vector<morph::ui::FetchedRow>, table::TableError>> got;
    rows.fetchRows({std::int64_t{2}, std::int64_t{99}, std::int64_t{7}}, {"name", "nosuch", "id"})
        .then([&got](std::expected<std::vector<morph::ui::FetchedRow>, table::TableError> const& result) {
            got = result;
        });
    h.settle();
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    REQUIRE((*got)->size() == 2);
    morph::ui::FetchedRow const& two = (*got)->at(0);
    CHECK(two.key == table::RowId{std::int64_t{2}});
    CHECK(two.viewRow == 7);
    CHECK(two.cells ==
          std::vector<table::Cell>{table::Cell{std::string{"row-2"}}, table::Cell{}, table::Cell{std::int64_t{2}}});
    CHECK((*got)->at(1).viewRow == 2);
}

namespace {

// Answers every open `Table.samples` call that carries no keys with the page its query asks for, of a view of
// @p total rows holding the columns the query names (row k keyed k, named "row-<k>"), until none is open.

void replyConfigPages(TableHarness& h, std::int64_t total) {
    for (int round = 0; round < 128; ++round) {
        std::optional<std::size_t> open;
        for (std::size_t index = 0; index < h.dispatcher.calls.size() && !open; ++index) {
            auto const& call = h.dispatcher.calls[index];
            if (call.action == "Table.samples" && !call.answered && !call.options.stop.stop_requested() &&
                !queryOf(h, index).keys.has_value()) {
                open = index;
            }
        }
        if (!open) {
            return;
        }
        table::TableQuery const query = queryOf(h, *open);
        std::int64_t const count = std::clamp<std::int64_t>(total - query.page.offset, 0, query.page.limit);
        h.reply(*open, pageJsonAt(h.described.revision, query.page.offset, count, total,
                                  query.columns.value_or(std::vector<std::string>{})));
    }
    FAIL("the pages kept coming");
}

// Answers every open keyed `Table.samples` call with its keys, key k at view row k, holding the columns its query
// names (each name "renamed-<k>"), of a view of @p total rows.

void replyCopyFetches(TableHarness& h, std::int64_t total) {
    for (std::size_t index = 0; index < h.dispatcher.calls.size(); ++index) {
        auto const& call = h.dispatcher.calls[index];
        if (call.action != "Table.samples" || call.answered || call.options.stop.stop_requested()) {
            continue;
        }
        table::TableQuery const query = queryOf(h, index);
        if (!query.keys) {
            continue;
        }
        std::vector<std::pair<std::int64_t, std::int64_t>> rows;
        for (table::RowId const& key : *query.keys) {
            rows.emplace_back(std::get<std::int64_t>(key), std::get<std::int64_t>(key));
        }
        h.reply(index,
                keyedJsonAt(h.described.revision, rows, total, query.columns.value_or(std::vector<std::string>{})));
    }
}

}  // namespace

// Mutation: copy writing rows in selection order instead of view order.

TEST_CASE("copy: selected rows as tab-separated display text in layout order, rows not held fetched",
          "[ui][table][config]") {
    constexpr std::string_view kArranged =
        R"("selectionMode":"multiple","sort":{"lit":[{"column":"name","direction":"asc"}]},)"
        R"("layout":{"lit":{"order":["name","id","c3","c4","c5"],"hidden":["c3"],"pinned":0,"widths":{}}})";
    std::string const text = withTableKey(kConfigModelTable, kArranged);
    SECTION("the selected rows") {
        TableHarness h;
        auto const screen = h.mount(text);
        replyDescribe(h, samplesDescription(5));
        replyConfigPages(h, 1000);  // rows 0 to 399 held
        int const table = h.backend.all("Table").front();
        REQUIRE(elementValue(*screen, "t", "visibleColumns") == R"(["name","id","c4","c5"])");
        h.backend.selectTableRange(table, 700, 700);
        h.settle();
        replyConfigPages(h, 1000);
        h.backend.selectTableRange(table, 500, 500, SelectionChange::Extend);
        h.settle();
        replyConfigPages(h, 1000);
        h.backend.selectTableRange(table, 3, 3, SelectionChange::Extend);
        h.settle();
        REQUIRE(elementValue(*screen, "t", "selection") == "[700,500,3]");
        std::size_t const sent = h.dispatcher.calls.size();
        pressCopy(h, table);
        REQUIRE(h.dispatcher.calls.size() == sent + 1);  // one keyed request; row 3 is held
        table::TableQuery const fetched = queryOf(h, sent);
        CHECK(fetched.keys == std::optional{std::vector<table::RowId>{std::int64_t{700}, std::int64_t{500}}});
        CHECK(fetched.columns == std::optional{std::vector<std::string>{"name", "id", "c4", "c5"}});
        replyCopyFetches(h, 1000);
        CHECK(clipboardText(h) == "row-3\t3\t3\t3\nrenamed-500\t500\t500\t500\nrenamed-700\t700\t700\t700");
        CHECK(statusOf(h, table).message == "Copied 3 rows");
        chooseMenu(h, table, std::nullopt, {"Copy with headers"});
        replyCopyFetches(h, 1000);
        CHECK(clipboardText(h) ==
              "Name\tId\tC4\tC5\nrow-3\t3\t3\t3\nrenamed-500\t500\t500\t500\nrenamed-700\t700\t700\t700");
    }
    SECTION("no row selected: the focused cell") {
        TableHarness h;
        auto const screen = h.mount(text);
        replyDescribe(h, samplesDescription(5));
        replyConfigPages(h, 1000);
        int const table = h.backend.all("Table").front();
        h.backend.focusMove(table, morph::ui::FocusMove::First);
        h.backend.focusMove(table, morph::ui::FocusMove::Down);
        h.settle();
        pressCopy(h, table);
        CHECK(clipboardText(h) == "row-1");
        CHECK(statusOf(h, table).message == "Copied 1 row");
    }
    SECTION("a rows table's column with its own cell copies its value's text") {
        TableHarness h;
        auto const screen =
            h.mount(R"({"vocab":"ui/1","id":"samples","state":{"items":{"init":{"lit":)"
                    R"([{"id":"a","name":"Ann"},{"id":"b","name":"Bob"},{"id":"c","name":"Cy"}]}}},)"
                    R"("view":{"kind":"table","id":"list","testId":"list","rows":{"ref":"items"},"key":{"row":"id"},)"
                    R"("selectionMode":"multiple","columns":[{"id":"id","label":"Id"},)"
                    R"({"id":"name","label":"Name","cell":[{"kind":"badge","text":{"row":"name"}}]}]}})");
        int const table = h.byTestId("Table", "list");
        h.backend.selectRows(table, {Key{std::string{"c"}}, Key{std::string{"a"}}});
        h.settle();
        chooseMenu(h, table, std::nullopt, {"Copy with headers"});
        CHECK(clipboardText(h) == "Id\tName\na\tAnn\nc\tCy");
        CHECK(statusOf(h, table).message == "Copied 2 rows");
    }
}

// Mutation: unloaded cells copied as their placeholder text.

TEST_CASE("copy: unloaded cells copy as empty and are counted", "[ui][table][config]") {
    TableHarness h;
    auto const screen = h.mount(withTableKey(kConfigModelTable, R"("selectionMode":"multiple")"));
    replyDescribe(h, withLevel(samplesDescription(5), "c4", "recipe"));
    table::TableQuery const first = lastQuery(h);
    REQUIRE(first.columns.has_value());
    // A hand-written reply: rows 0 and 1 lack the level `recipe`, their `c4` null on the wire.
    table::GenericPage page = morph::ui::testing::pageAt(h.described.revision, 0, 200, 200, *first.columns);
    auto const c4 = static_cast<std::size_t>(std::ranges::find(page.columns, "c4") - page.columns.begin());
    REQUIRE(c4 < page.columns.size());
    page.rows.at(0).at(c4) = table::Cell{};
    page.rows.at(1).at(c4) = table::Cell{};
    page.unloadedRows = {{.index = 0, .levels = {"recipe"}, .failed = {}},
                         {.index = 1, .levels = {"recipe"}, .failed = {}}};
    replyPage(h, glz::write_json(page).value_or(""));
    int const table = h.backend.all("Table").front();
    h.backend.selectTableRange(table, 0, 1);
    h.settle();
    REQUIRE(elementValue(*screen, "t", "selectedCount") == "2");
    std::size_t const sent = h.dispatcher.calls.size();
    pressCopy(h, table);
    CHECK(h.dispatcher.calls.size() == sent);  // no request waits for the level
    CHECK(clipboardText(h) == "0\trow-0\t0\t\t0\n1\trow-1\t1\t\t1");
    CHECK(statusOf(h, table).message == "Copied 2 rows, 2 cells were not loaded");
}

// Review Focus 3. Mutations: the bounds compared with `>=`; `fetchRows` sending every key in one request.

TEST_CASE("range and copy at their bounds: 10,000 pass, 10,001 are refused", "[ui][table][config]") {
    TableHarness h;
    auto const screen = h.mount(withTableKey(kConfigModelTable, R"("selectionMode":"multiple")"));
    replyDescribe(h, samplesDescription(5));
    replyConfigPages(h, 100'000);  // rows 0 to 399 held
    int const table = h.backend.all("Table").front();
    std::size_t const sent = h.dispatcher.calls.size();
    h.backend.selectTableRange(table, 0, 9'999);
    h.settle();
    REQUIRE(h.dispatcher.calls.size() > sent);
    for (std::size_t call = sent; call < h.dispatcher.calls.size(); ++call) {
        CHECK(h.dispatcher.calls.at(call).body.contains(R"("columns":[])"));  // the key pages name no columns
    }
    replyConfigPages(h, 100'000);
    REQUIRE(elementValue(*screen, "t", "selectedCount") == "10000");
    std::size_t const copyFrom = h.dispatcher.calls.size();
    pressCopy(h, table);
    // The 9,600 rows not held go in requests of at most `maxKeys` keys, which together name each of them once.
    std::vector<std::size_t> sizes;
    std::set<std::int64_t> named;
    for (std::size_t call = copyFrom; call < h.dispatcher.calls.size(); ++call) {
        table::TableQuery const query = queryOf(h, call);
        REQUIRE(query.keys.has_value());
        sizes.push_back(query.keys->size());
        for (table::RowId const& key : *query.keys) {
            named.insert(std::get<std::int64_t>(key));
        }
    }
    CHECK(sizes.size() == 10);
    CHECK(std::ranges::all_of(sizes, [](std::size_t size) { return size <= 1000; }));
    CHECK(std::accumulate(sizes.begin(), sizes.end(), std::size_t{0}) == 9'600);
    CHECK(named.size() == 9'600);
    CHECK(*named.begin() == 400);
    CHECK(*named.rbegin() == 9'999);
    replyCopyFetches(h, 100'000);
    std::string const copied = clipboardText(h);
    CHECK(std::ranges::count(copied, '\n') == 9'999);
    CHECK(copied.starts_with("0\trow-0\t0\t0\t0\n1\trow-1\t1\t1\t1\n"));
    CHECK(copied.ends_with("\n9999\trenamed-9999\t9999\t9999\t9999"));
    CHECK(statusOf(h, table).message == "Copied 10,000 rows");
    std::size_t const before = h.dispatcher.calls.size();
    h.backend.selectTableRange(table, 0, 10'000);  // 10,001 rows
    h.settle();
    CHECK(h.dispatcher.calls.size() == before);
    CHECK(statusOf(h, table).message == "A range selects at most 10,000 rows");
    CHECK(elementValue(*screen, "t", "selectedCount") == "10000");
    h.backend.selectTableRange(table, 10'000, 10'000, SelectionChange::Extend);
    h.settle();
    replyConfigPages(h, 100'000);
    REQUIRE(elementValue(*screen, "t", "selectedCount") == "10001");
    std::size_t const asked = h.dispatcher.calls.size();
    pressCopy(h, table);
    CHECK(h.dispatcher.calls.size() == asked);
    CHECK(statusOf(h, table).message == "A copy holds at most 10,000 rows");
    CHECK(clipboardText(h) == copied);
}

// Mutation: copy writing `table::displayText`, which writes `0.125`.

TEST_CASE("copy: writes the shown text and the header with its unit", "[ui][table][config]") {
    TableHarness h;
    auto const screen =
        h.mount(doc(R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",)"
                    R"("selectionMode":"multiple","columns":[{"id":"id"},{"id":"mass"},{"id":"name"}]})"));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, companionPage(h));
    int const table = h.byTestId("Table", "t");
    h.backend.selectRows(table, {Key{std::int64_t{1}}});
    h.settle();
    chooseMenu(h, table, std::nullopt, {"Copy with headers"});
    CHECK(clipboardText(h) == "Id\tMass [kg]\tName\n1\t0.13\tA");
}

namespace {

// One row of a hand-written keyed reply: its key, its view row, and whether it lacks the level `recipe`.

struct KeyedRow {
    std::int64_t key = 0;
    std::int64_t viewRow = 0;
    bool unloaded = false;
};

// A keyed reply of `samples` to the query of call @p index: each row named "renamed-<key>", every other cell its key,
// and a row that lacks `recipe` with its `c4` null and listed in `unloadedRows`.

std::string keyedReply(TableHarness const& h, std::size_t index, std::vector<KeyedRow> const& rows,
                       std::int64_t total) {
    table::GenericPage page;
    page.revision = h.described.revision;
    page.columns = queryOf(h, index).columns.value_or(std::vector<std::string>{});
    page.total = total;
    for (KeyedRow const& row : rows) {
        page.keys.emplace_back(row.key);
        page.indices.push_back(row.viewRow);
        table::GenericRow cells;
        for (std::string const& column : page.columns) {
            if (column == "name") {
                cells.emplace_back("renamed-" + std::to_string(row.key));
            } else if (column == "c4" && row.unloaded) {
                cells.emplace_back();
            } else {
                cells.emplace_back(row.key);
            }
        }
        if (row.unloaded) {
            page.unloadedRows.push_back(table::UnloadedRow{
                .index = static_cast<std::int64_t>(page.rows.size()), .levels = {"recipe"}, .failed = {}});
        }
        page.rows.push_back(std::move(cells));
    }
    return glz::write_json(page).value_or("");
}

// The open keyed `Table.samples` calls, oldest first.

std::vector<std::size_t> openKeyed(TableHarness const& h) {
    std::vector<std::size_t> out;
    for (std::size_t index = 0; index < h.dispatcher.calls.size(); ++index) {
        auto const& call = h.dispatcher.calls[index];
        if (call.action == "Table.samples" && !call.answered && !call.options.stop.stop_requested() &&
            queryOf(h, index).keys.has_value()) {
            out.push_back(index);
        }
    }
    return out;
}

// A table over `samples` whose `c4` is of the level `recipe`, its first two pages held of 1,000 rows, rows 500 and
// then 700 selected.

std::unique_ptr<morph::ui::Screen> mountSelected(TableHarness& h) {
    auto screen = h.mount(withTableKey(kConfigModelTable, R"("selectionMode":"multiple")"));
    replyDescribe(h, withLevel(samplesDescription(5), "c4", "recipe"));
    replyConfigPages(h, 1000);
    int const table = h.backend.all("Table").front();
    h.backend.selectTableRange(table, 500, 500);
    h.settle();
    replyConfigPages(h, 1000);
    h.backend.selectTableRange(table, 700, 700, SelectionChange::Extend);
    h.settle();
    replyConfigPages(h, 1000);
    REQUIRE(elementValue(*screen, "t", "selection") == "[500,700]");
    return screen;
}

}  // namespace

// Mutations: no sort by view row (the rows in the reply's order, 500 first); `fetchedOf` not marking a cell of a level
// its row lacks (the message counts no unloaded cell); no generation check (the earlier copy's late rows overwrite
// the later copy's); a copy that found no rows still written ("Copied 0 rows"); "Copied" shown though `copyText`
// wrote nothing.

TEST_CASE("copy: fetched rows in view order, an unloaded level, a failed fetch, a later copy and no clipboard",
          "[ui][table][config]") {
    TableHarness h;
    auto const screen = mountSelected(h);
    int const table = h.backend.all("Table").front();
    pressCopy(h, table);
    std::vector<std::size_t> keyed = openKeyed(h);
    REQUIRE(keyed.size() == 1);
    SECTION("the view rows order them, and an unloaded cell is copied empty and counted") {
        // The view moved under the selection: key 700 is now above key 500, and lacks `recipe`.
        h.reply(keyed.front(), keyedReply(h, keyed.front(),
                                          {{.key = 500, .viewRow = 900, .unloaded = false},
                                           {.key = 700, .viewRow = 800, .unloaded = true}},
                                          1000));
        CHECK(clipboardText(h) == "700\trenamed-700\t700\t\t700\n500\trenamed-500\t500\t500\t500");
        CHECK(statusOf(h, table).message == "Copied 2 rows, 1 cell was not loaded");
    }
    SECTION("a failed fetch copies nothing and says why") {
        h.fail(keyed.front());
        CHECK(clipboardText(h).empty());
        CHECK(statusOf(h, table).message == "The rows could not be copied: it failed");
    }
    SECTION("rows that left the view copy nothing and say nothing") {
        h.reply(keyed.front(), keyedReply(h, keyed.front(), {}, 1000));
        CHECK(clipboardText(h).empty());
        CHECK(statusOf(h, table).message.empty());
    }
    SECTION("a later copy wins over an earlier one whose rows arrive after it") {
        h.backend.selectTableRange(table, 700, 700);
        h.settle();
        replyConfigPages(h, 1000);
        pressCopy(h, table);
        keyed = openKeyed(h);
        REQUIRE(keyed.size() == 2);
        h.reply(keyed.back(), keyedReply(h, keyed.back(), {{.key = 700, .viewRow = 700, .unloaded = false}}, 1000));
        CHECK(clipboardText(h) == "700\trenamed-700\t700\t700\t700");
        h.reply(keyed.front(), keyedReply(h, keyed.front(),
                                          {{.key = 500, .viewRow = 500, .unloaded = false},
                                           {.key = 700, .viewRow = 700, .unloaded = false}},
                                          1000));
        CHECK(clipboardText(h) == "700\trenamed-700\t700\t700\t700");
        CHECK(statusOf(h, table).message == "Copied 1 row");
    }
    SECTION("a backend without a clipboard: no claim that anything was copied") {
        h.backend.setClipboard(false);
        h.reply(keyed.front(), keyedReply(h, keyed.front(), {{.key = 500, .viewRow = 500, .unloaded = false}}, 1000));
        CHECK(statusOf(h, table).message == "The rows could not be copied: there is no clipboard");
    }
}

// Mutation: `~WindowRows` not stopping the fetches `fetchRows` sent (the keyed call is still open after the unmount).

TEST_CASE("copy: a table unmounted with its copy's fetch in flight tears down cleanly", "[ui][table][config]") {
    {
        TableHarness h;
        auto screen = mountSelected(h);
        pressCopy(h, h.backend.all("Table").front());
        std::vector<std::size_t> const keyed = openKeyed(h);
        REQUIRE(keyed.size() == 1);
        screen.reset();
        h.settle();
        CHECK(h.dispatcher.calls.at(keyed.front()).options.stop.stop_requested());
        CHECK(clipboardText(h).empty());
    }
    SUCCEED("the screen, the session and the runtime went without an abort");
}

// Mutation: the slots not cleared (each `gather` holds its mount's scope, which holds the element that holds the
// configuring: the runtime aborts at teardown, "Runtime destroyed while nodes are alive").

TEST_CASE("configuring: the slots a mount sets go with the mount, so a handler may hold its scope",
          "[ui][table][config]") {
    {
        TableHarness h;
        auto screen = h.mount(withTableKey(kConfigModelTable, R"("selectionMode":"multiple")"));
        replyDescribe(h, samplesDescription(5));
        replyConfigPages(h, 1000);
        auto rows = h.mount(kNine);
        rows.reset();
        screen.reset();
        h.settle();
    }
    SUCCEED("the screens, the session and the runtime went without an abort");
}

// Mutation: the collection keeping the reply's order (chrome.rows not set by the interpreter).

TEST_CASE("collection: a collection and a rows table over the same rows show the same order and selection",
          "[ui][table]") {
    TableHarness h;
    auto const screen = h.mount(kCollectionBesideTable);
    h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
    int const collection = h.byTestId("Table", "collection_rows");
    int const table = h.find("Table", "testId", "t");
    // A header index counts every column of `Table::columns`, hidden ones included (the surface lays out every
    // column and hides through `Table::arrangement`): ID is index 0 and hidden, Name is index 1 on both tables.
    REQUIRE(h.backend.prop(collection, "columns").starts_with("[ID"));
    h.backend.clickHeader(collection, 1, HeaderClick::Plain);  // Name
    h.backend.clickHeader(table, 1, HeaderClick::Plain);       // Name
    h.settle();
    CHECK(rowKeys(h, collection) == rowKeys(h, table));
    CHECK(rowKeys(h, collection) != std::vector<std::string>{"1", "2", "3", "4"});
    CHECK(h.text("cell_name_2") == "Alpha");
}

// Mutation: the master-detail selection lost when the engine reorders.

TEST_CASE("collection: a master-detail selection and its open row hold through a sort", "[ui][table]") {
    TableHarness h;
    h.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMasterDetailSamples});
    auto const screen = h.mount(kSamplesCollection);
    h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
    int const collection = h.byTestId("Table", "collection_rows");
    h.backend.selectRows(collection, {Key{std::int64_t{3}}});
    h.settle();
    REQUIRE(h.backend.prop(h.byTestId("Column", "detailForm"), "visible") == "true");
    std::string const selected = h.backend.prop(collection, "selection");
    REQUIRE(selected == "[3]");
    h.backend.clickHeader(collection, 1, HeaderClick::Plain);  // Name
    h.settle();
    REQUIRE(rowKeys(h, collection) != std::vector<std::string>{"1", "2", "3", "4"});
    CHECK(h.backend.prop(collection, "selection") == selected);
    CHECK(h.backend.prop(h.byTestId("Column", "detailForm"), "visible") == "true");
    CHECK(h.backend.prop(h.byTestId("TextInput", "field_sampleId"), "text") == "3");
}

// Mutation: the collection's state saved under the screen's rows key.

TEST_CASE("collection: header and table menus are present and its arrangement persists under its action",
          "[ui][table]") {
    auto const store = std::make_shared<table::MemoryTableStateStore>(morph::ui::testing::storeExecutor());
    TableHarness h{store};
    // Every column shown, so hiding Name leaves one.
    h.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMixedSamplesView});
    auto const screen = h.mount(kSamplesCollection);
    h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
    int const collection = h.byTestId("Table", "collection_rows");
    CHECK_FALSE(menuOf(h, collection, std::nullopt).empty());
    std::vector<std::string> const labels = labelsOf(menuOf(h, collection, 1));  // Name
    REQUIRE(std::ranges::find(labels, "Hide column") != labels.end());
    chooseMenu(h, collection, 1, {"Hide column"});
    h.advance(500ms);
    std::optional<std::optional<table::StoredTableState>> stored;
    store
        ->load(table::TableStateKey{.application = "lims",
                                    .model = "Lims_SampleModel",
                                    .table = "Lims_ListSamples",
                                    .qualifier = "samples",
                                    .part = "state"})
        .thenDetached([&stored](std::optional<table::StoredTableState> const& state) { stored = state; });
    h.settle();
    REQUIRE(stored.has_value());
    REQUIRE(stored->has_value());
    CHECK((*stored)->revision == "1");  // one save
    CHECK((*stored)->json.contains(R"("hidden":["name"])"));
}
