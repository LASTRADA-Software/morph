// SPDX-License-Identifier: Apache-2.0

// The cell a form member contributes to a table row: the column kind of every field kind, and the cell of every member
// kind under every column kind it can be read as.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <functional>
#include <morph/forms/engine/form_session.hpp>
#include <morph/table/data_source.hpp>
#include <morph/ui/form_rows.hpp>
#include <morph/ui/grid.hpp>
#include <string>
#include <string_view>
#include <variant>

#include "grid_test_support.hpp"

namespace {

using namespace morph;
using morph::test::grid::GridFixture;
using morph::test::grid::GridSpec;
using table::ColumnKind;

constexpr std::string_view kMembers = R"({"type":"object","title":"Members","properties":{
  "rows":{"type":"array","title":"Rows","x-order":0,"items":{"type":"object","properties":{
    "count":{"type":["integer","null"],"title":"Count","x-order":0},
    "dec":{"type":["object","null"],"title":"Dec","x-decimalPlaces":2,"x-order":1},
    "flag":{"type":"boolean","title":"Flag","x-order":2},
    "day":{"type":["string","null"],"format":"date","title":"Day","x-order":3},
    "moment":{"type":["string","null"],"format":"date-time","title":"Moment","x-order":4},
    "word":{"type":["string","null"],"title":"Word","x-order":5},
    "sinceDay":{"type":["integer","null"],"x-widget":"epochDays","title":"Since day","x-order":6},
    "sinceSecond":{"type":["integer","null"],"x-widget":"epochSeconds","title":"Since second","x-order":7}}}}}})";

GridFixture membersFixture(std::string body) {
    return GridFixture{GridSpec{.schema = std::string{kMembers}, .body = std::move(body), .array = "rows"}};
}

template <class T>
T const* cellAs(table::Cell const& cell) {
    return std::get_if<T>(&cell);
}

}  // namespace

TEST_CASE("cov form rows: every field kind maps to a column kind", "[ui][grid][cov]") {
    using forms::FieldKind;
    CHECK(ui::detail::columnKindOf(FieldKind::Integer) == ColumnKind::Integer);
    CHECK(ui::detail::columnKindOf(FieldKind::Slider) == ColumnKind::Integer);
    CHECK(ui::detail::columnKindOf(FieldKind::Decimal) == ColumnKind::Decimal);
    CHECK(ui::detail::columnKindOf(FieldKind::Quantity) == ColumnKind::Decimal);
    CHECK(ui::detail::columnKindOf(FieldKind::Number) == ColumnKind::Number);
    CHECK(ui::detail::columnKindOf(FieldKind::Date) == ColumnKind::Date);
    CHECK(ui::detail::columnKindOf(FieldKind::EpochDate) == ColumnKind::Date);
    CHECK(ui::detail::columnKindOf(FieldKind::DateTime) == ColumnKind::DateTime);
    CHECK(ui::detail::columnKindOf(FieldKind::EpochDateTime) == ColumnKind::DateTime);
    CHECK(ui::detail::columnKindOf(FieldKind::Boolean) == ColumnKind::Bool);
    CHECK(ui::detail::columnKindOf(FieldKind::Text) == ColumnKind::Text);
    CHECK(ui::detail::columnKindOf(FieldKind::Multiline) == ColumnKind::Text);
    CHECK(ui::detail::columnKindOf(FieldKind::Enum) == ColumnKind::Text);
    CHECK(ui::detail::columnKindOf(FieldKind::Choice) == ColumnKind::Text);
    CHECK(ui::detail::columnKindOf(FieldKind::Array) == ColumnKind::Text);
    CHECK(ui::detail::columnKindOf(FieldKind::Object) == ColumnKind::Text);
    CHECK(ui::detail::columnKindOf(FieldKind::ObjectArray) == ColumnKind::Text);
}

TEST_CASE("cov form rows: a member with no value is an empty cell", "[ui][grid][cov]") {
    GridFixture f = membersFixture(R"({"rows":[{}]})");
    forms::FieldState const& count = f.form->field("rows[0].count");
    CHECK(std::holds_alternative<std::monostate>(ui::detail::memberCell(count, ColumnKind::Integer)));
}

TEST_CASE("cov form rows: an unparsable draft stays its text", "[ui][grid][cov]") {
    GridFixture f = membersFixture(R"({"rows":[{}]})");
    f.form->field("rows[0].count").text().set("abc");
    f.h.settle();
    forms::FieldState const& count = f.form->field("rows[0].count");
    table::Cell const cell = ui::detail::memberCell(count, ColumnKind::Integer);
    REQUIRE(cellAs<std::string>(cell) != nullptr);
    CHECK(*cellAs<std::string>(cell) == "abc");
}

TEST_CASE("cov form rows: an integer member reads under each numeric kind", "[ui][grid][cov]") {
    GridFixture f = membersFixture(R"({"rows":[{"count":7}]})");
    forms::FieldState const& count = f.form->field("rows[0].count");
    table::Cell const asInt = ui::detail::memberCell(count, ColumnKind::Integer);
    REQUIRE(cellAs<std::int64_t>(asInt) != nullptr);
    CHECK(*cellAs<std::int64_t>(asInt) == 7);
    table::Cell const asDecimal = ui::detail::memberCell(count, ColumnKind::Decimal);
    REQUIRE(cellAs<math::Rational>(asDecimal) != nullptr);
    CHECK(cellAs<math::Rational>(asDecimal)->toDouble() == 7.0);
    table::Cell const asNumber = ui::detail::memberCell(count, ColumnKind::Number);
    REQUIRE(cellAs<double>(asNumber) != nullptr);
    CHECK(*cellAs<double>(asNumber) == 7.0);
    table::Cell const asDate = ui::detail::memberCell(count, ColumnKind::Date);
    REQUIRE(cellAs<std::int64_t>(asDate) != nullptr);
    CHECK(*cellAs<std::int64_t>(asDate) == 7);
    table::Cell const asInstant = ui::detail::memberCell(count, ColumnKind::DateTime);
    REQUIRE(cellAs<std::int64_t>(asInstant) != nullptr);
    CHECK(*cellAs<std::int64_t>(asInstant) == 7);
    table::Cell const asBool = ui::detail::memberCell(count, ColumnKind::Bool);
    REQUIRE(cellAs<std::string>(asBool) != nullptr);  // not a Boolean: the draft stays
    CHECK(*cellAs<std::string>(asBool) == "7");
}

TEST_CASE("cov form rows: a decimal member reads as an exact value and as a double", "[ui][grid][cov]") {
    GridFixture f = membersFixture(R"({"rows":[{"dec":{"num":125,"den":100,"dp":2}}]})");
    forms::FieldState const& dec = f.form->field("rows[0].dec");
    table::Cell const exact = ui::detail::memberCell(dec, ColumnKind::Decimal);
    REQUIRE(cellAs<math::Rational>(exact) != nullptr);
    CHECK(cellAs<math::Rational>(exact)->toDouble() == 1.25);
    table::Cell const inexact = ui::detail::memberCell(dec, ColumnKind::Number);
    REQUIRE(cellAs<double>(inexact) != nullptr);
    CHECK(*cellAs<double>(inexact) == 1.25);
    table::Cell const asInt = ui::detail::memberCell(dec, ColumnKind::Integer);
    REQUIRE(cellAs<std::string>(asInt) != nullptr);  // not an integer: the draft stays
    table::Cell const asText = ui::detail::memberCell(dec, ColumnKind::Text);
    REQUIRE(cellAs<std::string>(asText) != nullptr);
    CHECK(*cellAs<std::string>(asText) == (*f.form->field("rows[0].dec").encoded().get())->json);
}

TEST_CASE("cov form rows: a boolean member reads as a flag", "[ui][grid][cov]") {
    GridFixture f = membersFixture(R"({"rows":[{"flag":true}]})");
    forms::FieldState const& flag = f.form->field("rows[0].flag");
    table::Cell const cell = ui::detail::memberCell(flag, ColumnKind::Bool);
    REQUIRE(cellAs<bool>(cell) != nullptr);
    CHECK(*cellAs<bool>(cell));
    table::Cell const asInt = ui::detail::memberCell(flag, ColumnKind::Integer);
    CHECK(cellAs<std::int64_t>(asInt) == nullptr);
}

TEST_CASE("cov form rows: a date member reads as days since the epoch", "[ui][grid][cov]") {
    GridFixture f = membersFixture(R"({"rows":[{"day":"1970-01-11","sinceDay":12}]})");
    table::Cell const fromText = ui::detail::memberCell(f.form->field("rows[0].day"), ColumnKind::Date);
    REQUIRE(cellAs<std::int64_t>(fromText) != nullptr);
    CHECK(*cellAs<std::int64_t>(fromText) == 10);
    table::Cell const fromEpoch = ui::detail::memberCell(f.form->field("rows[0].sinceDay"), ColumnKind::Date);
    REQUIRE(cellAs<std::int64_t>(fromEpoch) != nullptr);
    CHECK(*cellAs<std::int64_t>(fromEpoch) == 12);
}

TEST_CASE("cov form rows: a date-time member reads as seconds since the epoch", "[ui][grid][cov]") {
    GridFixture f = membersFixture(R"({"rows":[{"moment":"1970-01-01T00:01:40Z","sinceSecond":90}]})");
    table::Cell const fromInstant = ui::detail::memberCell(f.form->field("rows[0].moment"), ColumnKind::DateTime);
    REQUIRE(cellAs<std::int64_t>(fromInstant) != nullptr);
    CHECK(*cellAs<std::int64_t>(fromInstant) == 100);
    table::Cell const fromEpoch = ui::detail::memberCell(f.form->field("rows[0].sinceSecond"), ColumnKind::DateTime);
    REQUIRE(cellAs<std::int64_t>(fromEpoch) != nullptr);
    CHECK(*cellAs<std::int64_t>(fromEpoch) == 90);
    table::Cell const asDate = ui::detail::memberCell(f.form->field("rows[0].moment"), ColumnKind::Date);
    CHECK(cellAs<std::string>(asDate) != nullptr);  // not a day: the draft stays
}

TEST_CASE("cov form rows: text kinds read the string or the encoded JSON", "[ui][grid][cov]") {
    GridFixture f = membersFixture(R"({"rows":[{"word":"hello","count":3}]})");
    for (ColumnKind const kind :
         {ColumnKind::Text, ColumnKind::Key, ColumnKind::Custom, ColumnKind::Measure, ColumnKind::Quantity}) {
        table::Cell const word = ui::detail::memberCell(f.form->field("rows[0].word"), kind);
        REQUIRE(cellAs<std::string>(word) != nullptr);
        CHECK(*cellAs<std::string>(word) == "hello");
        table::Cell const count = ui::detail::memberCell(f.form->field("rows[0].count"), kind);
        REQUIRE(cellAs<std::string>(count) != nullptr);
        CHECK(*cellAs<std::string>(count) == "3");  // the encoded JSON of an integer
    }
}

TEST_CASE("cov form rows: a per-row member is its string, else its draft, else empty", "[ui][grid][cov]") {
    GridFixture f = membersFixture(R"({"rows":[{"word":"hello","count":3},{}]})");
    table::Cell const word = ui::detail::perRowCell(f.form->field("rows[0].word"));
    REQUIRE(cellAs<std::string>(word) != nullptr);
    CHECK(*cellAs<std::string>(word) == "hello");
    table::Cell const count = ui::detail::perRowCell(f.form->field("rows[0].count"));
    REQUIRE(cellAs<std::string>(count) != nullptr);
    CHECK(*cellAs<std::string>(count) == "3");
    CHECK(std::holds_alternative<std::monostate>(ui::detail::perRowCell(f.form->field("rows[1].word"))));
}

namespace {

std::vector<table::ColumnInfo> infosOf(std::vector<std::string> const& ids) {
    std::vector<table::ColumnInfo> infos;
    for (std::string const& id : ids) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): a fixture list built once
        infos.push_back(table::ColumnInfo{.id = id, .kind = ColumnKind::Text});
    }
    return infos;
}

using PerRow = std::vector<std::function<std::string(forms::RowState const&)>>;

}  // namespace

TEST_CASE("cov form rows: a source is refused what does not match the form's collection", "[r2][ui][grid][cov]") {
    using Catch::Matchers::ContainsSubstring;
    GridFixture f = membersFixture(R"({"rows":[{"word":"a"}]})");
    forms::FormSession& form = *f.form;
    // Not a collection.
    CHECK_THROWS_WITH((ui::FormRowsSource{form, "rows[0].word", infosOf({"word"}), {}, {}}),
                      ContainsSubstring("is not a collection"));
    // More members than columns.
    CHECK_THROWS_WITH((ui::FormRowsSource{form, "rows", infosOf({"word"}), {"word", "count"}, {}}),
                      ContainsSubstring("one member per column"));
    // A reader per row for some columns only.
    CHECK_THROWS_WITH((ui::FormRowsSource{form, "rows", infosOf({"word", "count"}), {}, PerRow(1)}),
                      ContainsSubstring("one per-row member reader per column"));
    // A member the rows do not have.
    CHECK_THROWS_WITH((ui::FormRowsSource{form, "rows", infosOf({"nope"}), {}, {}}),
                      ContainsSubstring("no member `nope` in `rows`"));
    // A column with its own reader names no member of the rows.
    PerRow readers(1);
    readers.front() = [](forms::RowState const&) { return std::string{"word"}; };
    CHECK_NOTHROW((ui::FormRowsSource{form, "rows", infosOf({"nope"}), {"nope"}, std::move(readers)}));
    // Columns alone name their members.
    ui::FormRowsSource const named{form, "rows", infosOf({"word", "count"}), {}, {}};
    CHECK(named.columns().size() == 2);
}

TEST_CASE("cov form rows: a grid is refused a collection it cannot edit or a member it lacks", "[r2][ui][grid][cov]") {
    using Catch::Matchers::ContainsSubstring;
    GridFixture f = membersFixture(R"({"rows":[{}]})");
    auto const columnOf = [](const std::string& id, std::string field) {
        return ui::GridColumn{.id = id, .label = id, .field = std::move(field), .value = {}};
    };
    std::vector<ui::GridColumn> columns;
    columns.push_back(columnOf("word", "word"));
    CHECK_THROWS_WITH((ui::FormGridSource{*f.form, "rows[0].word", columns, ui::FormatContext{}}),
                      ContainsSubstring("is not a collection"));
    columns.push_back(columnOf("nope", "nope"));
    CHECK_THROWS_WITH((ui::FormGridSource{*f.form, "rows", columns, ui::FormatContext{}}),
                      ContainsSubstring("no member `nope` in `rows`"));
    columns.pop_back();
    columns.push_back(
        ui::GridColumn{.id = "shown", .label = "shown", .field = {}, .value = [](forms::RowState const&) {
                           return std::string{"v"};
                       }});
    ui::FormGridSource const source{*f.form, "rows", columns, ui::FormatContext{}};
    ui::Key const first{std::int64_t{1}};
    CHECK(source.text(first, "shown") == "v");  // a display column shows what its reader gives
    CHECK(source.readOnly(first, "shown"));
    CHECK(source.draft(first, "shown").empty());
}
