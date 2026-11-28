// SPDX-License-Identifier: Apache-2.0

// The conversions between a table's cells and the values and JSON a window or a document's rows hold: the vote that
// infers a column's kind, a cell read as each kind, a typed page read from its JSON, and the cells' equality.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <limits>
#include <map>
#include <memory>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/query.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace table = morph::table;
namespace ui = morph::ui;
namespace detail = morph::ui::detail;
using table::Cell;
using table::ColumnKind;
using ui::Value;
using ui::ValueRecord;

morph::math::Rational dec(std::string_view text) { return table::parseDecimal(text).value(); }

glz::generic_u64 json(std::string_view text) {
    glz::generic_u64 out;
    REQUIRE_FALSE(glz::read_json(out, text));
    return out;
}

Value record(std::initializer_list<std::pair<std::string, Value>> fields) {
    ValueRecord out;
    for (auto const& [name, value] : fields) {
        out.insert_or_assign(name, value);
    }
    return Value{std::move(out)};
}

std::shared_ptr<table::UnitRegistry const> massUnits() {
    table::UnitRegistryBuilder builder;
    REQUIRE(builder.add(
        table::UnitInfo{.id = "kg", .symbol = "kg", .dimension = "mass", .toBase = dec("1"), .decimals = 3}));
    return std::move(builder).build();
}
}  // namespace

TEST_CASE("cov rows pure: exactPlacesOf counts the places a denominator needs", "[ui][table][cov]") {
    CHECK(detail::exactPlacesOf(1) == 0);
    CHECK(detail::exactPlacesOf(2) == 1);
    CHECK(detail::exactPlacesOf(4) == 2);
    CHECK(detail::exactPlacesOf(8) == 3);
    CHECK(detail::exactPlacesOf(1000) == 3);
    CHECK(detail::exactPlacesOf(3) == morph::math::kMaxDecimalPlaces);  // never a decimal
}

TEST_CASE("cov rows pure: measureOf reads a quantity or a three-field record and nothing else", "[ui][table][cov]") {
    // The parts point into the value read, so it outlives them.
    Value const kilos{ui::Quantity{.value = dec("2.5"), .unit = "kg"}};
    auto const quantity = detail::measureOf(kilos);
    REQUIRE(quantity.has_value());
    CHECK(quantity->amount == dec("2.5"));
    CHECK(*quantity->unit == "kg");
    Value const wire =
        record({{"num", Value{std::int64_t{5}}}, {"den", Value{std::int64_t{2}}}, {"unit", Value{std::string{"g"}}}});
    auto const parts = detail::measureOf(wire);
    REQUIRE(parts.has_value());
    CHECK(parts->amount == dec("2.5"));
    CHECK(parts->amount.decimalPlaces.value == 1);
    CHECK(*parts->unit == "g");
    CHECK_FALSE(detail::measureOf(Value{std::int64_t{1}}).has_value());
    CHECK_FALSE(
        detail::measureOf(record({{"num", Value{std::int64_t{5}}}, {"den", Value{std::int64_t{2}}}})).has_value());
    CHECK_FALSE(detail::measureOf(record({{"num", Value{std::int64_t{5}}},
                                          {"den", Value{std::int64_t{0}}},
                                          {"unit", Value{std::string{"g"}}}}))
                    .has_value());
    CHECK_FALSE(detail::measureOf(record({{"num", Value{std::string{"5"}}},
                                          {"den", Value{std::int64_t{2}}},
                                          {"unit", Value{std::string{"g"}}}}))
                    .has_value());
    CHECK_FALSE(detail::measureOf(record({{"num", Value{std::int64_t{5}}},
                                          {"den", Value{std::int64_t{2}}},
                                          {"unit", Value{std::int64_t{3}}}}))
                    .has_value());
    CHECK_FALSE(
        detail::measureOf(
            record({{"a", Value{std::int64_t{5}}}, {"b", Value{std::int64_t{2}}}, {"c", Value{std::string{"g"}}}}))
            .has_value());
}

TEST_CASE("cov rows pure: voteOf names the column kind a value points to", "[ui][table][cov]") {
    using detail::ValueVote;
    CHECK(detail::voteOf(Value{}) == ValueVote::None);
    CHECK(detail::voteOf(Value{ui::ValueList{}}) == ValueVote::None);
    CHECK(detail::voteOf(Value{true}) == ValueVote::Bool);
    CHECK(detail::voteOf(Value{std::int64_t{1}}) == ValueVote::Integer);
    CHECK(detail::voteOf(Value{ui::Decimal{dec("1.5")}}) == ValueVote::Decimal);
    CHECK(detail::voteOf(Value{std::string{"a"}}) == ValueVote::Text);
    CHECK(detail::voteOf(Value{morph::time::DateTime{}}) == ValueVote::DateTime);
    CHECK(detail::voteOf(Value{ui::Quantity{.value = dec("1"), .unit = "kg"}}) == ValueVote::Measure);
    CHECK(detail::voteOf(record({{"num", Value{std::int64_t{1}}},
                                 {"den", Value{std::int64_t{1}}},
                                 {"unit", Value{std::string{"g"}}}})) == ValueVote::Measure);
    CHECK(detail::voteOf(record({{"a", Value{std::int64_t{1}}}})) == ValueVote::None);
}

TEST_CASE("cov rows pure: inferredKind follows the votes", "[ui][table][cov]") {
    using detail::ValueVote;
    auto const kind = [](std::vector<ValueVote> const& votes) {
        std::array<std::size_t, detail::kValueVotes> counts{};
        for (ValueVote const vote : votes) {
            ++counts.at(static_cast<std::size_t>(vote));
        }
        return detail::inferredKind(counts, votes);
    };
    CHECK(kind({}) == ColumnKind::Text);
    CHECK(kind({ValueVote::None, ValueVote::None}) == ColumnKind::Text);
    CHECK(kind({ValueVote::None, ValueVote::Integer, ValueVote::Integer}) == ColumnKind::Integer);
    CHECK(kind({ValueVote::Bool}) == ColumnKind::Bool);
    CHECK(kind({ValueVote::DateTime}) == ColumnKind::DateTime);
    CHECK(kind({ValueVote::Measure}) == ColumnKind::Measure);
    CHECK(kind({ValueVote::Integer, ValueVote::Decimal}) == ColumnKind::Decimal);
    CHECK(kind({ValueVote::Decimal, ValueVote::Integer}) == ColumnKind::Decimal);
    // With a third kind the first vote decides, and an integer first with decimals present reads as decimals.
    CHECK(kind({ValueVote::Text, ValueVote::Integer, ValueVote::Decimal}) == ColumnKind::Text);
    CHECK(kind({ValueVote::Integer, ValueVote::Text, ValueVote::Decimal}) == ColumnKind::Decimal);
    CHECK(kind({ValueVote::Integer, ValueVote::Text}) == ColumnKind::Integer);
}

TEST_CASE("cov rows pure: convertedCell reads a value as each kind and flags what does not fit", "[ui][table][cov]") {
    auto const units = massUnits();
    auto const ok = [&](Value const& value, ColumnKind kind) {
        auto const converted = detail::convertedCell(value, kind, units.get());
        CHECK_FALSE(converted.invalid);
        return converted.cell;
    };
    auto const bad = [&](Value const& value, ColumnKind kind) {
        return detail::convertedCell(value, kind, units.get()).invalid;
    };
    CHECK(std::holds_alternative<std::monostate>(detail::convertedCell(Value{}, ColumnKind::Integer, nullptr).cell));
    CHECK_FALSE(detail::convertedCell(Value{}, ColumnKind::Integer, nullptr).invalid);
    CHECK(ok(Value{std::string{"a"}}, ColumnKind::Text) == Cell{std::string{"a"}});
    CHECK(ok(Value{std::string{"a"}}, ColumnKind::Custom) == Cell{std::string{"a"}});
    CHECK(bad(Value{std::int64_t{1}}, ColumnKind::Text));
    CHECK(std::holds_alternative<std::monostate>(
        detail::convertedCell(Value{std::int64_t{1}}, ColumnKind::Text, nullptr).cell));
    CHECK(ok(Value{std::int64_t{7}}, ColumnKind::Integer) == Cell{std::int64_t{7}});
    CHECK(ok(Value{std::int64_t{7}}, ColumnKind::Key) == Cell{std::int64_t{7}});
    CHECK(bad(Value{std::string{"7"}}, ColumnKind::Integer));
    CHECK(ok(Value{std::int64_t{7}}, ColumnKind::Decimal) ==
          Cell{morph::math::Rational{7, morph::math::DecimalPlaces{0}}});
    CHECK(ok(Value{ui::Decimal{dec("1.5")}}, ColumnKind::Quantity) == Cell{dec("1.5")});
    CHECK(bad(Value{true}, ColumnKind::Decimal));
    CHECK(ok(Value{std::int64_t{3}}, ColumnKind::Number) == Cell{3.0});
    CHECK(ok(Value{ui::Decimal{dec("0.5")}}, ColumnKind::Number) == Cell{0.5});
    CHECK(bad(Value{std::string{"x"}}, ColumnKind::Number));
    // 1970-01-01T00:00:01.999Z is the second 1; a time before the epoch rounds down.
    auto const at = [](std::int64_t ms) {
        return Value{
            morph::time::DateTime{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{ms}}}};
    };
    CHECK(ok(at(1999), ColumnKind::DateTime) == Cell{std::int64_t{1}});
    CHECK(ok(at(-1), ColumnKind::DateTime) == Cell{std::int64_t{-1}});
    CHECK(bad(Value{std::int64_t{5}}, ColumnKind::DateTime));
    CHECK(ok(at(86'400'000), ColumnKind::Date) == Cell{std::int64_t{1}});
    CHECK(ok(at(-1), ColumnKind::Date) == Cell{std::int64_t{-1}});
    CHECK(ok(Value{std::string{"2024-10-04"}}, ColumnKind::Date) == Cell{std::int64_t{20'000}});
    CHECK(bad(Value{std::string{"not a date"}}, ColumnKind::Date));
    CHECK(bad(Value{std::int64_t{5}}, ColumnKind::Date));
    CHECK(ok(Value{true}, ColumnKind::Bool) == Cell{true});
    CHECK(bad(Value{std::int64_t{1}}, ColumnKind::Bool));
    auto const kg = units->find("kg");
    REQUIRE(kg.has_value());
    CHECK(ok(Value{ui::Quantity{.value = dec("2"), .unit = "kg"}}, ColumnKind::Measure) ==
          Cell{table::Measure{.amount = dec("2"), .unit = *kg}});
    CHECK(bad(Value{ui::Quantity{.value = dec("2"), .unit = "lb"}}, ColumnKind::Measure));
    CHECK(detail::convertedCell(Value{ui::Quantity{.value = dec("2"), .unit = "kg"}}, ColumnKind::Measure, nullptr)
              .invalid);
    CHECK(bad(Value{std::int64_t{1}}, ColumnKind::Measure));
    CHECK(bad(Value{std::int64_t{1}}, static_cast<ColumnKind>(99)));
}

TEST_CASE("cov rows pure: sameCell and sameObject tell a change from a copy", "[ui][table][cov]") {
    CHECK(ui::detail::sameCell(Cell{std::int64_t{1}}, Cell{std::int64_t{1}}));
    CHECK_FALSE(ui::detail::sameCell(Cell{std::int64_t{1}}, Cell{std::int64_t{2}}));
    CHECK_FALSE(ui::detail::sameCell(Cell{std::int64_t{1}}, Cell{true}));
    // Equal in value, shown in other places: a change.
    CHECK(ui::detail::sameCell(Cell{dec("1.5")}, Cell{dec("1.5")}));
    CHECK_FALSE(ui::detail::sameCell(Cell{dec("1.5")}, Cell{dec("1.50")}));
    table::Measure const one{.amount = dec("1.5"), .unit = 1};
    table::Measure const padded{.amount = dec("1.50"), .unit = 1};
    CHECK(ui::detail::sameCell(Cell{one}, Cell{one}));
    CHECK_FALSE(ui::detail::sameCell(Cell{one}, Cell{padded}));

    Value const list{ui::ValueList{Value{std::int64_t{1}}}};
    Value const& copy = list;
    CHECK(ui::detail::sameObject(list, copy));
    CHECK_FALSE(ui::detail::sameObject(list, Value{ui::ValueList{Value{std::int64_t{1}}}}));
    Value const fields = record({{"a", Value{std::int64_t{1}}}});
    Value const& same = fields;
    CHECK(ui::detail::sameObject(fields, same));
    CHECK_FALSE(ui::detail::sameObject(fields, record({{"a", Value{std::int64_t{1}}}})));
    CHECK_FALSE(ui::detail::sameObject(fields, list));
    CHECK(ui::detail::sameObject(Value{std::int64_t{3}}, Value{std::int64_t{3}}));
    CHECK_FALSE(ui::detail::sameObject(Value{std::int64_t{3}}, Value{std::int64_t{4}}));
}

TEST_CASE("cov rows pure: dates, instants and doubles become the values a row shows", "[ui][table][cov]") {
    CHECK(*detail::dateValue(20'000).asString() == "2024-10-04");
    CHECK(*detail::dateValue(2'932'896).asString() == "9999-12-31");
    CHECK(*detail::dateValue(2'932'897).asString() == "2932897");
    CHECK(*detail::dateValue(-719'529).asString() == "-719529");
    auto const instant = detail::dateTimeValue(90);
    REQUIRE(instant.asTimestamp() != nullptr);
    CHECK(instant.asTimestamp()->value.time_since_epoch().count() == 90'000);
    CHECK(*detail::dateTimeValue(std::numeric_limits<std::int64_t>::max()).asString() == "9223372036854775807");
    CHECK(*detail::dateTimeValue(std::numeric_limits<std::int64_t>::min()).asString() == "-9223372036854775808");
    REQUIRE(detail::doubleValue(0.25).asDecimal() != nullptr);
    CHECK(detail::doubleValue(0.25).asDecimal()->value == dec("0.25"));
    REQUIRE(detail::doubleValue(std::nan("")).asString() != nullptr);
    CHECK(*detail::doubleValue(std::nan("")).asString() == "nan");
    CHECK(*detail::doubleValue(std::numeric_limits<double>::infinity()).asString() == "inf");
    CHECK(*detail::doubleValue(1e300).asString() == "1e+300");
}

TEST_CASE("cov rows pure: cellValue shows each kind of cell as a value", "[ui][table][cov]") {
    auto const units = massUnits();
    auto const kg = *units->find("kg");
    auto const column = [](ColumnKind kind) { return table::ColumnInfo{.id = "c", .kind = kind}; };
    CHECK(ui::cellValue(Cell{}, column(ColumnKind::Text)).isNull());
    CHECK(ui::cellValue(Cell{table::Unloaded{}}, column(ColumnKind::Text)).isNull());
    CHECK(*ui::cellValue(Cell{std::int64_t{20'000}}, column(ColumnKind::Date)).asString() == "2024-10-04");
    CHECK(ui::cellValue(Cell{std::int64_t{5}}, column(ColumnKind::DateTime)).asTimestamp() != nullptr);
    CHECK(*ui::cellValue(Cell{std::int64_t{5}}, column(ColumnKind::Integer)).asInt() == 5);
    CHECK(ui::cellValue(Cell{dec("1.5")}, column(ColumnKind::Decimal)).asDecimal() != nullptr);
    CHECK(ui::cellValue(Cell{0.5}, column(ColumnKind::Number)).asDecimal() != nullptr);
    CHECK(*ui::cellValue(Cell{std::string{"x"}}, column(ColumnKind::Text)).asString() == "x");
    CHECK(*ui::cellValue(Cell{true}, column(ColumnKind::Bool)).asBool());
    auto const measure = Cell{table::Measure{.amount = dec("2"), .unit = kg}};
    auto const shown = ui::cellValue(measure, column(ColumnKind::Measure), units.get());
    REQUIRE(shown.asQuantity() != nullptr);
    CHECK(shown.asQuantity()->unit == "kg");
    CHECK(ui::cellValue(measure, column(ColumnKind::Measure)).asDecimal() != nullptr);
    CHECK(ui::cellValue(Cell{table::Measure{.amount = dec("2"), .unit = 99}}, column(ColumnKind::Measure), units.get())
              .asDecimal() != nullptr);
    auto const grams = table::QuantityCell{.amount = dec("250"), .toCanonical = dec("0.001")};
    CHECK(ui::cellValue(Cell{grams}, column(ColumnKind::Quantity)).asDecimal()->value == dec("0.25"));
    morph::math::Rational const huge{std::numeric_limits<std::int64_t>::max(), morph::math::DecimalPlaces{0}};
    auto const overflowing = table::QuantityCell{.amount = huge, .toCanonical = dec("1000")};
    CHECK(ui::cellValue(Cell{overflowing}, column(ColumnKind::Quantity)).asString() != nullptr);
}

TEST_CASE("cov rows pure: jsonInteger and jsonDecimal read only what an exact number holds", "[ui][table][cov]") {
    CHECK(detail::jsonInteger(json("5")) == 5);
    CHECK(detail::jsonInteger(json("-5")) == -5);
    CHECK(detail::jsonInteger(json("9223372036854775807")) == std::numeric_limits<std::int64_t>::max());
    CHECK_FALSE(detail::jsonInteger(json("9223372036854775808")).has_value());
    CHECK_FALSE(detail::jsonInteger(json("1.5")).has_value());
    CHECK_FALSE(detail::jsonInteger(json(R"("5")")).has_value());
    auto const exact = detail::jsonDecimal(json(R"({"num":5,"den":2,"dp":1})"));
    REQUIRE(exact.has_value());
    CHECK(exact->amount == dec("2.5"));
    CHECK(exact->unit == nullptr);
    // The unit points into the member read, so it outlives the parts.
    auto const kilos = json(R"({"num":5,"den":2,"dp":1,"unit":"kg"})");
    auto const measure = detail::jsonDecimal(kilos);
    REQUIRE(measure.has_value());
    CHECK(*measure->unit == "kg");
    CHECK_FALSE(detail::jsonDecimal(json("5")).has_value());
    CHECK_FALSE(detail::jsonDecimal(json(R"({"num":5,"den":2})")).has_value());
    CHECK_FALSE(detail::jsonDecimal(json(R"({"num":5,"den":2,"x":1})")).has_value());
    CHECK_FALSE(detail::jsonDecimal(json(R"({"num":5,"den":0,"dp":1})")).has_value());
    CHECK_FALSE(detail::jsonDecimal(json(R"({"num":5,"den":2,"dp":-1})")).has_value());
    CHECK_FALSE(detail::jsonDecimal(json(R"({"num":5,"den":2,"dp":19})")).has_value());
    CHECK_FALSE(detail::jsonDecimal(json(R"({"num":"5","den":2,"dp":1})")).has_value());
    CHECK_FALSE(detail::jsonDecimal(json(R"({"num":5,"den":2,"dp":1,"unit":3})")).has_value());
    CHECK_FALSE(detail::jsonDecimal(json(R"({"num":5,"den":2,"dp":1,"other":"kg"})")).has_value());
}

TEST_CASE("cov rows pure: jsonVoteOf and inferredJsonKind name a column's kind from its JSON", "[ui][table][cov]") {
    using detail::JsonVote;
    CHECK(detail::jsonVoteOf(json("true")) == JsonVote::Bool);
    CHECK(detail::jsonVoteOf(json(R"("a")")) == JsonVote::Text);
    CHECK(detail::jsonVoteOf(json("3")) == JsonVote::Integer);
    CHECK(detail::jsonVoteOf(json("3.5")) == JsonVote::Number);
    CHECK(detail::jsonVoteOf(json(R"({"num":1,"den":1,"dp":0})")) == JsonVote::Decimal);
    CHECK(detail::jsonVoteOf(json(R"({"num":1,"den":1,"dp":0,"unit":"g"})")) == JsonVote::Measure);
    CHECK(detail::jsonVoteOf(json("null")) == JsonVote::None);
    CHECK(detail::jsonVoteOf(json(R"({"a":1})")) == JsonVote::None);
    auto const kindOf = [](std::string_view rows) {
        glz::generic_u64 const parsed = json(rows);
        auto const* const list = parsed.get_if<glz::generic_u64::array_t>();
        REQUIRE(list != nullptr);
        return detail::inferredJsonKind(*list, "v");
    };
    CHECK(kindOf("[]") == ColumnKind::Text);
    CHECK(kindOf(R"([{"v":null},{"w":1},5])") == ColumnKind::Text);
    CHECK(kindOf(R"([{"v":1},{"v":2}])") == ColumnKind::Integer);
    CHECK(kindOf(R"([{"v":1},{"v":{"num":3,"den":2,"dp":1}}])") == ColumnKind::Decimal);
    CHECK(kindOf(R"([{"v":1},{"v":2.5}])") == ColumnKind::Number);
    CHECK(kindOf(R"([{"v":"a"},{"v":2}])") == ColumnKind::Text);
    CHECK(kindOf(R"([{"v":2},{"v":"a"}])") == ColumnKind::Integer);
    CHECK(kindOf(R"([{"v":true},{"v":false}])") == ColumnKind::Bool);
}

TEST_CASE("cov rows pure: typedCell reads a JSON member as the column's kind", "[ui][table][cov]") {
    auto const units = massUnits();
    std::map<table::UnitId, std::string> named;
    auto const cell = [&](std::string_view text, ColumnKind kind, table::UnitRegistry const* registry = nullptr) {
        return detail::typedCell(json(text), kind, registry, named);
    };
    CHECK(std::holds_alternative<std::monostate>(cell("null", ColumnKind::Text)));
    CHECK(cell("true", ColumnKind::Bool) == Cell{true});
    CHECK(cell("3", ColumnKind::Integer) == Cell{std::int64_t{3}});
    CHECK(cell("3", ColumnKind::Decimal) == Cell{morph::math::Rational{3, morph::math::DecimalPlaces{0}}});
    CHECK(cell("3", ColumnKind::Number) == Cell{3.0});
    CHECK(cell("2.5", ColumnKind::Number) == Cell{2.5});
    CHECK(cell("18446744073709551615", ColumnKind::Number) == Cell{18446744073709551615.0});
    CHECK(cell(R"("2024-10-04")", ColumnKind::Date) == Cell{std::int64_t{20'000}});
    CHECK(cell(R"("nope")", ColumnKind::Date) == Cell{std::string{"nope"}});
    CHECK(cell(R"("2024-10-04T00:00:01Z")", ColumnKind::DateTime) == Cell{std::int64_t{1'728'000'001}});
    CHECK(cell(R"("nope")", ColumnKind::DateTime) == Cell{std::string{"nope"}});
    CHECK(cell(R"("text")", ColumnKind::Text) == Cell{std::string{"text"}});
    CHECK(cell(R"({"num":5,"den":2,"dp":1})", ColumnKind::Decimal) == Cell{dec("2.5")});
    auto const measure = cell(R"({"num":5,"den":2,"dp":1,"unit":"kg"})", ColumnKind::Measure, units.get());
    REQUIRE(std::holds_alternative<table::Measure>(measure));
    CHECK(std::get<table::Measure>(measure).unit == *units->find("kg"));
    CHECK(named.at(*units->find("kg")) == "kg");
    auto const unknown = cell(R"({"num":5,"den":2,"dp":1,"unit":"stone"})", ColumnKind::Measure, units.get());
    CHECK(std::get<table::Measure>(unknown).unit == table::kUnknownUnit);
    auto const noRegistry = cell(R"({"num":5,"den":2,"dp":1,"unit":"kg"})", ColumnKind::Measure);
    CHECK(std::get<table::Measure>(noRegistry).unit == table::kUnknownUnit);
    // A measure in a column that is not one, and any other JSON, shows as its own text.
    CHECK(cell(R"({"num":5,"den":2,"dp":1,"unit":"kg"})", ColumnKind::Text) ==
          Cell{std::string{R"({"num":5,"den":2,"dp":1,"unit":"kg"})"}});
    CHECK(cell("[1,2]", ColumnKind::Text) == Cell{std::string{"[1,2]"}});
    CHECK(detail::typedKey(json("7")) == table::RowId{std::int64_t{7}});
    CHECK(detail::typedKey(json(R"("k")")) == table::RowId{std::string{"k"}});
    CHECK_FALSE(detail::typedKey(json("[1]")).has_value());
}

TEST_CASE("cov rows pure: a typed page is refused for the shapes it cannot place", "[ui][table][cov]") {
    table::TableDescription d;
    d.id = "t";
    d.key = "id";
    d.columns = {table::TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}},
                 table::TableColumn{.info = {.id = "name", .kind = ColumnKind::Text}}};
    auto const read = [&](std::string_view page) { return ui::readTypedPage(page, d, nullptr); };
    auto const good = read(R"({"rows":[{"id":1,"name":"a"},{"id":2}],"total":2,"indices":[4,5]})");
    REQUIRE(good.has_value());
    CHECK(good->keys == std::vector<table::RowId>{table::RowId{std::int64_t{1}}, table::RowId{std::int64_t{2}}});
    CHECK(good->rows.at(0).at(1) == Cell{std::string{"a"}});
    CHECK(std::holds_alternative<std::monostate>(good->rows.at(1).at(1)));
    CHECK(good->indices == std::vector<std::int64_t>{4, 5});
    CHECK(good->total == 2);
    auto const refusal = [&](std::string_view page) {
        auto const result = read(page);
        REQUIRE_FALSE(result.has_value());
        return result.error().message;
    };
    CHECK(refusal("not json").starts_with("not a page"));
    CHECK(
        refusal(R"({"rows":[{"id":1},{"id":2}],"unloadedRows":[{"index":1},{"index":1}]})").contains("out of order"));
    CHECK(refusal(R"({"rows":[{"id":1}],"unloadedRows":[{"index":5}]})").contains("out of order"));
    CHECK(refusal(R"({"rows":[{"id":1},{"id":2}],"indices":[1]})").contains("indices"));
    CHECK(refusal(R"({"rows":[{"id":1}],"indices":[-1]})").contains("negative"));
    CHECK(refusal(R"({"rows":[5]})").contains("is not an object"));
    CHECK(refusal(R"({"rows":[{"name":"a"}]})").contains("has no key `id`"));
    CHECK(refusal(R"({"rows":[{"id":[1]}]})").contains("has no key"));
    // A page with a valid unloaded row keeps it.
    auto const withUnloaded = read(R"({"rows":[{"id":1},{"id":2}],"unloadedRows":[{"index":1,"levels":["l"]}]})");
    REQUIRE(withUnloaded.has_value());
    CHECK(withUnloaded->unloadedRows.size() == 1);
    // A measure column builds its units from the description when none are given.
    table::TableDescription withUnits = d;
    withUnits.columns.push_back(table::TableColumn{.info = {.id = "w", .kind = ColumnKind::Measure}});
    withUnits.units = {
        table::UnitInfo{.id = "kg", .symbol = "kg", .dimension = "mass", .toBase = dec("1"), .decimals = 3}};
    auto const measures =
        ui::readTypedPage(R"({"rows":[{"id":1,"w":{"num":3,"den":1,"dp":0,"unit":"kg"}}]})", withUnits, nullptr);
    REQUIRE(measures.has_value());
    CHECK(std::holds_alternative<table::Measure>(measures->rows.at(0).at(2)));
    CHECK(std::get<table::Measure>(measures->rows.at(0).at(2)).unit != table::kUnknownUnit);
}

TEST_CASE("cov rows pure: unlistedUnits lists each unit the rows bring that the registry lacks, up to a room",
          "[ui][table][cov]") {
    table::TableDescription d;
    d.id = "t";
    d.key = "id";
    d.columns = {table::TableColumn{.info = {.id = "id", .kind = ColumnKind::Integer}},
                 table::TableColumn{.info = {.id = "w", .kind = ColumnKind::Measure}}};
    auto const registry = massUnits();
    glz::generic_u64 const rows = json(
        R"([{"id":1,"w":{"num":1,"den":1,"dp":2,"unit":"stone"}},{"id":2,"w":{"num":1,"den":1,"dp":0,"unit":"kg"}},
                 {"id":3,"w":{"num":1,"den":1,"dp":0,"unit":"stone"}},{"id":4,"w":{"num":1,"den":1,"dp":0,"unit":""}},
                 {"id":5},7,{"id":6,"w":{"num":1,"den":1,"dp":0,"unit":"furlong"}},{"id":7,"w":3}])");
    auto const* const list = rows.get_if<glz::generic_u64::array_t>();
    REQUIRE(list != nullptr);
    auto const unlisted = detail::unlistedUnits(*list, d, registry.get(), 8);
    REQUIRE(unlisted.size() == 2);
    CHECK(unlisted.at(0).id == "stone");
    CHECK(unlisted.at(0).decimals == 2);
    CHECK(unlisted.at(1).id == "furlong");
    CHECK(detail::unlistedUnits(*list, d, registry.get(), 1).size() == 1);
    CHECK(detail::unlistedUnits(*list, d, nullptr, 8).size() == 3);  // with no registry kg is unlisted too
    CHECK(detail::unlistedUnits(*list, d, nullptr, 0).empty());
}

namespace {

table::RowId id(std::int64_t key) { return table::RowId{key}; }

ui::ActionInfo rowsAction(std::vector<std::string> paths, std::string recordName = "R") {
    ui::ActionInfo info;
    info.records.push_back(ui::ActionRecordInfo{
        .record = std::move(recordName), .keyField = std::nullopt, .paths = std::move(paths), .open = {}});
    return info;
}
}  // namespace

TEST_CASE("cov rows pure: the rows a reply changed and failed are read along the catalog's paths",
          "[ui][table][cov]") {
    Value const reply = record({
        {"rows", Value{ui::ValueList{record({{"key", Value{std::int64_t{1}}}}),
                                     record({{"key", Value{std::string{"b"}}}, {"from", Value{std::int64_t{9}}}}),
                                     record({{"key", Value{true}}}), record({{"nokey", Value{std::int64_t{1}}}})}}},
        {"failed",
         Value{ui::ValueList{record({{"key", Value{std::int64_t{2}}}, {"message", Value{std::string{"no"}}}}),
                             record({{"key", Value{std::int64_t{3}}}}),
                             record({{"key", Value{std::int64_t{4}}}, {"message", Value{std::int64_t{5}}}}),
                             record({{"message", Value{std::string{"orphan"}}}})}}},
        {"inner", record({{"rows", Value{ui::ValueList{record({{"key", Value{std::int64_t{7}}}})}}}})},
    });
    auto const top = ui::changedRowsOf(reply, rowsAction({"rows"}), "R");
    REQUIRE(top.rows.size() == 2);
    CHECK(top.rows.at(0).key == id(1));
    CHECK_FALSE(top.rows.at(0).from.has_value());
    CHECK(top.rows.at(1).key == table::RowId{std::string{"b"}});
    CHECK(top.rows.at(1).from == id(9));
    REQUIRE(top.failed.size() == 3);
    CHECK(top.failed.at(0).message == "no");
    CHECK(top.failed.at(1).message.empty());
    CHECK(top.failed.at(2).message.empty());  // a message that is no text is none
    // A path under a member, a path that is not a rows path, a record the reply is not about, a path that is absent.
    auto const nested = ui::changedRowsOf(reply, rowsAction({"inner.rows"}), "R");
    REQUIRE(nested.rows.size() == 1);
    CHECK(nested.rows.front().key == id(7));
    CHECK(ui::changedRowsOf(reply, rowsAction({"other"}), "R").rows.empty());
    CHECK(ui::changedRowsOf(reply, rowsAction({"rows"}), "S").rows.empty());
    CHECK(ui::changedRowsOf(reply, rowsAction({"missing.rows"}), "R").rows.empty());
    CHECK(ui::changedRowsOf(Value{std::int64_t{1}}, rowsAction({"rows"}), "R").rows.empty());
    CHECK(ui::changedRowsOf(record({{"rows", Value{std::int64_t{3}}}}), rowsAction({"rows"}), "R").rows.empty());
}

TEST_CASE("cov rows pure: a run's rows are busy until it settles and a moved key keeps its state",
          "[ui][table][cov]") {
    ui::RowStates states;
    states.busyKeys(1, {id(1), id(2), id(1)});
    CHECK(states.isBusy(id(1)));
    CHECK_FALSE(states.isBusy(id(3)));
    CHECK(states.busyCount() == 2);
    // A key moved to a key the run holds already leaves one; a free new key takes the old one's place.
    states.move(id(1), id(1));
    states.move(id(1), id(2));
    CHECK(states.busyCount() == 1);
    CHECK(states.isBusy(id(2)));
    states.move(id(2), id(5));
    CHECK(states.isBusy(id(5)));
    CHECK_FALSE(states.isBusy(id(2)));
    states.settle(1);
    CHECK_FALSE(states.isBusy(id(5)));

    // A view run holds every row but those it excepts; its count and the key runs' keys that it does not leave out.
    states.busyView(2, {id(3)}, 10);
    CHECK(states.isBusy(id(4)));
    CHECK_FALSE(states.isBusy(id(3)));
    states.busyKeys(3, {id(3), id(4)});
    CHECK(states.isBusy(id(3)));
    CHECK(states.busyCount() == 10 + 1);  // id 4 is in the view run already
    states.busyView(4, {}, -5);
    CHECK(states.busyCount() >= 10);
    // A moved key: the view run holds it when it held the old one, and excepts the old key.
    states.move(id(4), id(40));
    CHECK(states.isBusy(id(40)));
    states.settle(2);
    states.settle(3);
    states.settle(4);
    CHECK(states.busyCount() == 0);

    std::vector<table::FailedRow> const failed{{.key = id(1), .message = "a"}, {.key = id(2), .message = "b"}};
    states.fail(failed);
    states.fail(std::vector<table::FailedRow>{{.key = id(1), .message = "again"}});
    CHECK(states.failure(id(1)) == "again");
    CHECK_FALSE(states.failure(id(9)).has_value());
    CHECK(states.failedKeys() == std::vector<table::RowId>{id(1), id(2)});
    states.move(id(1), id(2));  // the new key is failed already: the old one goes
    CHECK(states.failedKeys() == std::vector<table::RowId>{id(2)});
    states.move(id(2), id(6));
    CHECK(states.failedKeys() == std::vector<table::RowId>{id(6)});
    states.move(id(77), id(78));  // nothing held under it
    CHECK(states.failedKeys() == std::vector<table::RowId>{id(6)});
}

TEST_CASE("cov rows pure: the select-all refusal and the row count say what is still loading", "[ui][table][cov]") {
    auto const en = ui::FormatContext{};
    CHECK(ui::selectAllRefusalText({.undecided = 1, .levels = {}}, en) ==
          "Select all is available once 1 row is decided");
    CHECK(ui::selectAllRefusalText({.undecided = 3, .levels = {}}, en) ==
          "Select all is available once 3 rows are decided");
    CHECK(ui::selectAllRefusalText({.undecided = 0, .levels = {}}, en) ==
          "Select all is available once the rows have loaded");
    CHECK(ui::selectAllRefusalText({.undecided = 0, .levels = {"a", "b"}}, en) ==
          "Select all is available once a and b data has loaded");

    ui::TableCountsView counts;
    CHECK(ui::countsText(counts, 0, en).empty());
    counts.view = ui::TableTotal{.rows = 5, .kind = table::TotalKind::Exact, .known = true};
    CHECK(ui::countsText(counts, 0, en) == "5 rows");
    counts.view.rows = 1;
    CHECK(ui::countsText(counts, 0, en) == "1 row");
    CHECK(ui::countsText(counts, 1, en) == "1 of 1 selected");
    counts.view.rows = 5;
    CHECK(ui::countsText(counts, 2, en) == "2 of 5 selected");
    counts.view.kind = table::TotalKind::AtLeast;
    CHECK(ui::countsText(counts, 0, en) == "5 or more");
    CHECK(ui::countsText(counts, 2, en) == "2 of 5 or more selected");
    counts.undecided = 4;
    CHECK(ui::countsText(counts, 2, en) == "5 so far, 4 loading");
}
