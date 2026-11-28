// SPDX-License-Identifier: Apache-2.0

// A form grid's columns and the overlays they take per row: the unit, from a name or an expression of the row, and the
// options.

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>
#include <string_view>

#include "ui_table_support.hpp"

namespace {

using namespace morph;

constexpr std::string_view kRowsSchema = R"({"type":"object","title":"Rows","properties":{
  "rows":{"type":"array","title":"Rows","x-order":0,"items":{"type":"object","properties":{
    "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":3,"ExtUnits":{"unitAscii":"kg"},
            "x-unitAlternatives":[{"id":"g","decimals":0,"num":1,"den":1000}],"x-order":0},
    "choice":{"enum":["low","high",null],"title":"Choice","x-order":1},
    "note":{"type":["string","null"],"title":"Note","x-order":2}}}}}})";

constexpr std::string_view kUnitByRow = R"({"vocab":"ui/1","id":"rows",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordRows","submit":"explicit"},
    {"kind":"table","id":"grid","testId":"t","form":"rec","field":"rows",
     "columns":[{"id":"mass","field":"mass",
                 "unit":{"if":[{"fn":"eq","args":[{"fn":"coalesce","args":[{"ref":"row.note"},""]},"g"]},"g","kg"]}},
                {"id":"choice","field":"choice",
                 "options":{"if":[{"fn":"eq","args":[{"fn":"coalesce","args":[{"ref":"row.note"},""]},"one"]},
                                  {"lit":["low"]},{"lit":null}]}}]}]}})";

constexpr std::string_view kNamedUnit = R"({"vocab":"ui/1","id":"rows",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordRows","submit":"explicit"},
    {"kind":"table","id":"grid","testId":"t","form":"rec","field":"rows",
     "columns":[{"id":"mass","field":"mass","unit":"g"}]}]}})";

void addRows(ui::testing::TableHarness& h) {
    ui::testing::addAction(h, "Lims_RecordRows", std::string{kRowsSchema},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
}

}  // namespace

TEST_CASE("cov forms: a column's unit and options are read in each row", "[ui][grid][interpreter][cov]") {
    ui::testing::TableHarness h;
    addRows(h);
    auto const screen = h.mount(kUnitByRow);
    forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
    form.prefill(R"({"rows":[{"note":"g"},{"note":"one"}]})");
    h.settle();
    forms::FieldState const& first = form.field("rows[0].mass");
    forms::FieldState const& second = form.field("rows[1].mass");
    REQUIRE(first.spec().units.size() == 2);
    CHECK(first.spec().units.at(first.unit().peek()).id == "g");
    CHECK(second.spec().units.at(second.unit().peek()).id == "kg");
    CHECK(form.field("rows[0].choice").options().size() == 2);  // no list for this row: the schema's
    REQUIRE(form.field("rows[1].choice").options().size() == 1);
    CHECK(form.field("rows[1].choice").options().front().valueJson == R"("low")");
}

TEST_CASE("cov forms: a column's unit named in the document applies to every row", "[ui][grid][interpreter][cov]") {
    ui::testing::TableHarness h;
    addRows(h);
    auto const screen = h.mount(kNamedUnit);
    forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
    form.prefill(R"({"rows":[{},{}]})");
    h.settle();
    for (char const* const path : {"rows[0].mass", "rows[1].mass"}) {
        forms::FieldState const& mass = form.field(path);
        REQUIRE(mass.spec().units.size() == 2);
        CHECK(mass.spec().units.at(mass.unit().peek()).id == "g");
    }
}
