// SPDX-License-Identifier: Apache-2.0

// A table over `rows` whose columns come from an expression: each column's caption and place, and what a place that
// names none reports.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_support.hpp"

namespace {

using morph::ui::Align;
using morph::ui::TableColumn;
using morph::ui::testing::reportsOf;
using morph::ui::testing::TableHarness;

std::string derivedDoc(std::string_view columns) {
    return R"({"vocab":"ui/1","id":"d","view":{"kind":"table","id":"t","testId":"t",)"
           R"("rows":{"lit":[{"id":"a","name":"A"},{"id":"b","name":"B"}]},"key":{"row":"id"},"columns":{"lit":)" +
           std::string{columns} + "}}}";
}

}  // namespace

TEST_CASE("covi derived columns: each column's caption and place as the expression gave them", "[covi][ui][table]") {
    TableHarness h;
    auto const screen = h.mount(derivedDoc(R"([{"id":"id","label":"Id","align":"end","valign":"start"},)"
                                           R"({"id":"name","align":null},{"label":"Unnamed"}])"));
    std::vector<TableColumn> const columns = h.backend.tableColumns(h.byTestId("Table", "t"));
    REQUIRE(columns.size() == 3);
    CHECK(columns.at(0).label == "Id");
    CHECK(columns.at(0).align == Align::End);
    CHECK(columns.at(0).valign == Align::Start);
    CHECK(columns.at(1).label == "name");  // no label: the id
    CHECK(columns.at(1).align == Align::Start);
    CHECK(columns.at(1).valign == Align::Center);
    CHECK(columns.at(2).label == "Unnamed");
    CHECK(reportsOf(h).empty());
}

TEST_CASE("covi derived columns: a place that names none is reported once for the table", "[covi][ui][table]") {
    struct Case {
        char const* what;
        std::string columns;
    };
    for (Case const& item : std::vector<Case>{
             {"a name that is no place", R"([{"id":"id","align":"sideways"},{"id":"name","valign":"up"}])"},
             {"a place that is no text", R"([{"id":"id","align":5},{"id":"name","align":7}])"},
         }) {
        INFO(item.what);
        TableHarness h;
        auto const screen = h.mount(derivedDoc(item.columns));
        std::vector<TableColumn> const columns = h.backend.tableColumns(h.byTestId("Table", "t"));
        REQUIRE(columns.size() == 2);
        CHECK(columns.at(0).align == Align::Start);  // the fallback
        std::vector<std::string> const reports = reportsOf(h);
        REQUIRE(reports.size() == 1);
        CHECK(reports.front().contains("a cell's place is start, center or end"));
    }
}

TEST_CASE("covi derived columns: an expression that is no list gives the table no column", "[covi][ui][table]") {
    TableHarness h;
    auto const screen = h.mount(derivedDoc(R"({"id":"id"})"));
    CHECK(h.backend.tableColumns(h.byTestId("Table", "t")).empty());
}
