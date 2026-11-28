// SPDX-License-Identifier: Apache-2.0

// A table over a typed list action reads its columns, key and units from the action's view document: what the
// document leaves unsaid or says wrongly is skipped or reported, never fatal.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_support.hpp"

namespace {

using morph::ui::ActionInfo;
using morph::ui::testing::addAction;
using morph::ui::testing::addView;
using morph::ui::testing::elementValue;
using morph::ui::testing::kindOfColumn;
using morph::ui::testing::reportsOf;
using morph::ui::testing::TableHarness;
using morph::ui::testing::windowOf;

constexpr std::string_view kQuery = R"({"type":"object","properties":{"query":{"type":"object","x-table":true}}})";

std::string tableDoc(std::string_view extra) {
    return R"({"vocab":"ui/1","id":"samples","models":{"lab":{"type":"Lims_SampleModel"}},"view":{"kind":"table",)"
           R"("id":"list","testId":"list","model":"lab","action":"Lims_ViewSamples",)" +
           std::string{extra} + "}}";
}

struct Mounted {
    TableHarness h;
    std::unique_ptr<morph::ui::Screen> screen;

    Mounted(std::string_view view, std::string_view extra) {
        addAction(h, "Lims_ViewSamples", std::string{kQuery}, ActionInfo{.modelType = "Lims_SampleModel"});
        addView(h, "Lims_ViewSamples", std::string{view});
        screen = h.mount(tableDoc(extra));
        h.reply(h.dispatcher.last("Lims_ViewSamples"), R"({"rows":[],"total":0})");
    }

    std::string visible() const { return elementValue(*screen, "list", "visibleColumns"); }

    std::string units() const {
        std::string out;
        for (auto const& unit : windowOf(*screen).description()->units) {
            out += unit.id + "|" + unit.symbol + "|" + unit.dimension + "|" + std::to_string(unit.decimals) + ";";
        }
        return out;
    }
};

struct ColumnsCase {
    std::string_view name;
    std::string_view view;
    std::string_view extra;
    std::string_view visible;
    std::string_view key;
};

}  // namespace

TEST_CASE("view document columns: entries that are no columns are skipped and a bad kind is inferred",
          "[covg][ui][table]") {
    auto const& c = GENERATE(values<ColumnsCase>({
        {"entries that are not columns",
         R"({"v-columns":[7,{"label":"no field"},{"field":"id","label":"Ident","kind":"bogus"},)"
         R"({"field":"name","v-hidden":"yes"},{"field":"note","v-hidden":true},{"field":4}]})",
         R"("key":{"row":"id"})", R"(["id","name"])", "id"},
        {"columns that are no array", R"({"v-columns":5})", R"("key":{"row":"id"})", "[]", "id"},
        {"a view that is no object", "[1]", R"("key":{"row":"id"})", "[]", "id"},
        {"a view that is no document", "nonsense", R"("key":{"row":"id"})", "[]", "id"},
        {"a row key the view names", R"({"v-rowKey":"sid","v-columns":[{"field":"sid"}]})",
         R"("columns":[{"id":"sid"}])", R"(["sid"])", "sid"},
        {"a row key that is no text", R"({"v-rowKey":7,"v-columns":[{"field":"sid"}]})", R"("columns":[{"id":"sid"}])",
         R"(["sid"])", "id"},
        {"a key that reads no single field", R"({"v-rowKey":"sid","v-columns":[{"field":"sid"}]})",
         R"("key":{"row":"a.b"},"columns":[{"id":"sid"}])", R"(["sid"])", "sid"},
    }));
    CAPTURE(c.name);
    Mounted m{c.view, c.extra};
    CHECK(m.visible() == c.visible);
    CHECK(windowOf(*m.screen).description()->key == c.key);
}

TEST_CASE("view document columns: a part keeps its own kind and takes the view's label and kind otherwise",
          "[covg][ui][table]") {
    Mounted m{R"({"v-columns":[{"field":"id","label":"ID","kind":"integer"},{"field":"name","label":"Name"}]})",
              R"("key":{"row":"id"},"columns":[{"id":"id"},{"id":"name","kind":"date"},{"id":"free"}])"};
    CHECK(kindOfColumn(*m.screen, "id") == morph::table::ColumnKind::Integer);  // the view's kind
    CHECK(kindOfColumn(*m.screen, "name") == morph::table::ColumnKind::Date);   // the part's own kind
    auto const& columns = windowOf(*m.screen).description()->columns;
    REQUIRE(columns.size() == 3);
    CHECK(columns[0].label == "ID");    // the view's label
    CHECK(columns[1].label == "Name");  //
    CHECK(columns[2].label == "free");  // no entry: its id
}

namespace {

struct UnitsCase {
    std::string_view name;
    std::string_view column;
    std::string_view units;
    std::string_view reports;
};

}  // namespace

TEST_CASE("view document units: what a unit declares wrongly is left out or defaulted, and said",
          "[covg][ui][table]") {
    auto const& c = GENERATE(values<UnitsCase>({
        {"ExtUnits that is no object", R"("ExtUnits":5)", "", ""},
        {"a unit with no id", R"("ExtUnits":{"unitAscii":""})", "", ""},
        {"a unit with no symbol and no places", R"("ExtUnits":{"unitAscii":"kg"})", "kg|kg|kg|0;", ""},
        {"a symbol that is empty", R"("ExtUnits":{"unitAscii":"kg","unitUnicode":""},"x-decimalPlaces":2)",
         "kg|kg|kg|2;", ""},
        {"places below zero and past the bound",
         R"("ExtUnits":{"unitAscii":"kg","unitUnicode":"K"},"x-decimalPlaces":-4)", "kg|K|kg|0;", ""},
        {"alternatives that are no array", R"("ExtUnits":{"unitAscii":"kg"},"x-unitAlternatives":3)", "kg|kg|kg|0;",
         ""},
        {"alternatives of every wrong shape",
         R"("ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},"x-unitAlternatives":[3,{"num":1},{"id":""},{"id":"kg"},)"
         R"({"id":"g","num":0},{"id":"mg","den":-5},)"
         R"({"id":"t","num":18446744073709551615,"den":1000,"display":"","decimals":2},)"
         R"({"id":"q","num":"x","display":"Q"}])",
         "kg|kg|kg|0;t|t|kg|2;q|Q|kg|0;",
         "the unit `g` has a factor of zero or less; it is not listed|"
         "the unit `mg` has a factor of zero or less; it is not listed"},
    }));
    CAPTURE(c.name);
    std::string const view = R"({"v-columns":[{"field":"id","kind":"integer"},{"field":"mass","kind":"quantity",)" +
                             std::string{c.column} + "}]}";
    Mounted m{view, R"("key":{"row":"id"})"};
    CHECK(m.units() == c.units);
    std::string joined;
    for (auto const& report : reportsOf(m.h)) {
        joined += (joined.empty() ? "" : "|") + report;
    }
    CHECK(joined == c.reports);
    CHECK(kindOfColumn(*m.screen, "mass") ==
          (c.units.empty() ? morph::table::ColumnKind::Quantity : morph::table::ColumnKind::Measure));
}

TEST_CASE("view document units: a unit two columns declare differently is said once and the first is kept",
          "[covg][ui][table]") {
    Mounted const m{
        R"({"v-columns":[)"
        R"({"field":"a","kind":"quantity","ExtUnits":{"unitAscii":"kg"},"x-unitAlternatives":[{"id":"g","num":1,"den":1000}]},)"
        R"({"field":"b","kind":"quantity","ExtUnits":{"unitAscii":"kg"},"x-unitAlternatives":[{"id":"g","num":1,"den":100}]}]})",
        R"("key":{"row":"id"})"};
    CHECK(reportsOf(m.h) ==
          std::vector<std::string>{"the unit `g` is declared differently by two columns; the first is kept"});
    CHECK(m.units() == "kg|kg|kg|0;g|g|kg|0;");
}
