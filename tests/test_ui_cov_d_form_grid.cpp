// SPDX-License-Identifier: Apache-2.0

// A form grid's columns as a document spells them: the overlays that give nothing, the columns a document leaves out,
// and the member a column names by expression.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <morph/forms/engine/form_session.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_support.hpp"

namespace {

using namespace morph;

constexpr std::string_view kRowsSchema = R"({"type":"object","title":"Rows","properties":{
  "rows":{"type":"array","title":"Rows","x-order":0,"items":{"type":"object","properties":{
    "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":3,"ExtUnits":{"unitAscii":"kg"},
            "x-unitAlternatives":[{"id":"g","decimals":0,"num":1,"den":1000}],"x-order":0},
    "choice":{"enum":["low","high",null],"title":"Choice","x-order":1},
    "note":{"type":["string","null"],"title":"Note","x-order":2}}}}}})";

// A document with one state, `v`, set to @p state, which a column's expression reads: an overlay the loader cannot
// check, because its value is only known when it is read.
std::string gridDocument(std::string_view tableKeys, std::string_view state) {
    return std::string{R"({"vocab":"ui/1","id":"rows","state":{"v":{"init":)"} + std::string{state} +
           std::string{R"(}},
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordRows","submit":"explicit"},
    {"kind":"table","id":"grid","testId":"t","form":"rec","field":"rows",)"} +
           std::string{tableKeys} + "}]}}";
}

void addRows(ui::testing::TableHarness& h) {
    ui::testing::addAction(h, "Lims_RecordRows", std::string{kRowsSchema},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
}

struct Mounted {
    ui::testing::TableHarness h;
    std::unique_ptr<ui::Screen> screen;
    forms::FormSession* form = nullptr;

    explicit Mounted(std::string_view tableKeys, std::string_view prefill = R"({"rows":[{},{}]})",
                     std::string_view state = "null") {
        addRows(h);
        screen = h.mount(gridDocument(tableKeys, state));
        form = ui::testing::elementOf(*screen, "rec").form.get();
        form->prefill(prefill);
        h.settle();
    }
};

std::string massColumn(std::string_view overlay) {
    return R"("columns":[{"id":"mass","field":"mass",)" + std::string{overlay} + "}]";
}

}  // namespace

TEST_CASE("covd form grid: overlays that read to no usable value leave the schema's mass places and unit",
          "[ui][grid][interpreter][covd]") {
    Mounted const baseline{massColumn(R"("label":"Mass")")};
    forms::FieldState& plain = baseline.form->field("rows[0].mass");
    std::optional<std::uint32_t> const schemaPlaces = plain.places().peek();
    std::size_t const schemaUnit = plain.unit().peek();

    struct Case {
        std::string_view overlay;
        std::string_view state;
    };
    auto const [overlay, state] =
        GENERATE(Case{R"("unit":{"ref":"v"})", R"("")"}, Case{R"("unit":{"ref":"v"})", "5"},
                 Case{R"("unit":{"ref":"v"})", "null"}, Case{R"("decimals":{"ref":"v"})", R"("x")"},
                 Case{R"("decimals":{"ref":"v"})", "-1"}, Case{R"("decimals":{"ref":"v"})", "null"});
    CAPTURE(overlay, state);
    Mounted const m{massColumn(overlay), R"({"rows":[{},{}]})", state};
    forms::FieldState& mass = m.form->field("rows[0].mass");
    CHECK(mass.places().peek() == schemaPlaces);
    CHECK(mass.unit().peek() == schemaUnit);
}

TEST_CASE("covd form grid: a decimals overlay and a unit expression set the row's places and unit",
          "[ui][grid][interpreter][covd]") {
    Mounted const m{massColumn(R"("decimals":1,"unit":"g")")};
    forms::FieldState& mass = m.form->field("rows[0].mass");
    CHECK(mass.places().peek() == std::optional<std::uint32_t>{1});
    CHECK(mass.spec().units.at(mass.unit().peek()).id == "g");
}

TEST_CASE("covd form grid: a readonly overlay freezes a cell only when it reads to the boolean true",
          "[ui][grid][interpreter][covd]") {
    struct Case {
        std::string_view readonly;
        bool frozen;
    };
    auto const [readonly, frozen] = GENERATE(
        Case{R"({"fn":"eq","args":[1,1]})", true}, Case{R"({"fn":"eq","args":[1,2]})", false},
        Case{R"({"fn":"coalesce","args":[{"ref":"row.note"},"yes"]})", false},
        Case{R"({"fn":"coalesce","args":[{"ref":"row.note"},1]})", false}, Case{R"({"ref":"row.note"})", false});
    CAPTURE(readonly);
    Mounted m{R"("columns":[{"id":"note","field":"note","readonly":)" + std::string{readonly} + "}]"};
    int const table = m.h.byTestId("Table", "t");
    m.h.backend.clickCell(table, 0, 0);
    m.h.settle();
    m.h.backend.typeInto(table, "z");
    m.h.settle();
    bool const editing = ui::testing::elementValue(m.h, "grid", "editing") != ui::Value{};
    CHECK(editing == !frozen);
}

TEST_CASE("covd form grid: a placeholder overlay is read per row", "[ui][grid][interpreter][covd]") {
    Mounted const m{
        R"("columns":[{"id":"note","field":"note","placeholder":{"fn":"coalesce","args":[{"ref":"row.note"},"none"]}}])",
        R"({"rows":[{"note":"kept"},{}]})"};
    CHECK(m.form->field("rows[0].note").placeholder() == "kept");
    CHECK(m.form->field("rows[1].note").placeholder() == "none");
}

TEST_CASE("covd form grid: a column that names a member by expression reports a name that is none",
          "[ui][grid][interpreter][covd]") {
    Mounted m{
        R"("columns":[{"id":"any","field":{"if":[{"fn":"eq","args":[{"fn":"coalesce","args":[{"ref":"row.note"},""]},"x"]},"nosuch","note"]}}])",
        R"({"rows":[{"note":"x"},{"note":"y"}]})"};
    bool reported = false;
    for (ui::Refusal const& refusal : m.h.diagnostics) {
        reported = reported || refusal.message().contains("`nosuch` names no member of the row");
    }
    CHECK(reported);
    CHECK(ui::testing::cellText(m.h, 2, "any") == "y");
}

TEST_CASE("covd form grid: a grid that lists no columns shows every member of the row in schema order",
          "[ui][grid][interpreter][covd]") {
    Mounted m{R"("configurable":false)"};
    int const table = m.h.byTestId("Table", "t");
    CHECK(m.h.backend.headerText(table, 0) == "Mass");
    CHECK(m.h.backend.headerText(table, 1) == "Choice");
    CHECK(m.h.backend.headerText(table, 2) == "Note");
}

TEST_CASE("covd form grid: a display column shows its value read with the row's record",
          "[ui][grid][interpreter][covd]") {
    Mounted m{
        R"("columns":[{"id":"note","field":"note"},{"id":"shown","value":{"fn":"coalesce","args":[{"ref":"row.note"},"-"]}}])",
        R"({"rows":[{"note":"kept"},{}]})"};
    CHECK(ui::testing::cellText(m.h, 1, "shown") == "kept");
    CHECK(ui::testing::cellText(m.h, 2, "shown") == "-");
}
