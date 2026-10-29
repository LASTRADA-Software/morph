// SPDX-License-Identifier: Apache-2.0

// The edges of expr/1's numeric, list and date functions: arguments a document can only compute, which the parser
// cannot refuse, and results that do not fit. Each evaluates to `null` with one report naming why.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <string_view>

namespace ui = morph::ui;

namespace {

// An expression's value as JSON text, then ` | ` and the reason of each report.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): an expression, then the names it reads, as JSON text.
std::string evaluated(std::string_view expr, std::string_view names = "{}") {
    auto const json = ui::parseJson(expr);
    REQUIRE(json.has_value());
    auto const parsed = ui::parseExpression(*json);
    if (!parsed) {
        FAIL("refused: " << parsed.error().message());
    }
    ui::StaticEnvironment env;
    auto const namesJson = ui::parseJson(names);
    REQUIRE(namesJson.has_value());
    auto const record = ui::valueFromJson(*namesJson);
    REQUIRE(record.has_value());
    env.names = *record->asRecord();
    std::string out = glz::write_json(ui::valueToJson(ui::evaluate(**parsed, env))).value_or("?");
    for (ui::Refusal const& report : env.diagnostics) {
        out += " | " + report.reason;
    }
    return out;
}

constexpr std::string_view kSqrtArgs = " | `sqrt` takes a plain number, places from 0 to 18 and a rounding mode";
constexpr std::string_view kStatArgs = " | the statistics take a list, places from 0 to 18 and a rounding mode";

}  // namespace

// Mutation: read any string as halfEven (`sqrt` with mode `up` then answers 1.4).
TEST_CASE("ui expression edges: a rounding mode is halfEven or halfUp, and anything else is a mismatch",
          "[ui][expr]") {
    CHECK(evaluated(R"({"fn": "sqrt", "args": [2, 1, {"ref": "m"}]})", R"({"m": "halfEven"})") ==
          R"({"num":7,"den":5,"dp":1})");
    CHECK(evaluated(R"({"fn": "sqrt", "args": [2, 1, {"ref": "m"}]})", R"({"m": "up"})") ==
          "null" + std::string{kSqrtArgs});
    CHECK(evaluated(R"({"fn": "sqrt", "args": [2, 1, {"ref": "m"}]})", R"({"m": 1})") ==
          "null" + std::string{kSqrtArgs});
}

// Mutation: let `sqrt` take a negative place count (it then answers a root at -1 places).
TEST_CASE("ui expression edges: sqrt takes a plain number and places from 0 to 18", "[ui][expr]") {
    CHECK(evaluated(R"({"fn": "sqrt", "args": [2, {"ref": "p"}]})", R"({"p": -1})") ==
          "null" + std::string{kSqrtArgs});
    CHECK(evaluated(R"({"fn": "sqrt", "args": [2, {"ref": "p"}]})", R"({"p": "two"})") ==
          "null" + std::string{kSqrtArgs});
    CHECK(evaluated(R"({"fn": "sqrt", "args": [{"ref": "x"}, 2]})", R"({"x": "four"})") ==
          "null" + std::string{kSqrtArgs});
    // 9 * 10^18 scaled by 10^36 for 18 places is past 128 bits.
    CHECK(evaluated(R"({"fn": "sqrt", "args": [9000000000000000000, 18]})") == "null | the result overflows");
}

// Mutation: let the statistics take a negative place count (stdev then answers at -1 places).
TEST_CASE("ui expression edges: stdev and rsd take a list, places from 0 to 18 and a mode", "[ui][expr]") {
    CHECK(evaluated(R"({"fn": "stdev", "args": [{"ref": "xs"}, 2]})", R"({"xs": 4})") ==
          "null" + std::string{kStatArgs});
    CHECK(evaluated(R"({"fn": "rsd", "args": [{"lit": [1, 2]}, {"ref": "p"}]})", R"({"p": -1})") ==
          "null" + std::string{kStatArgs});
    CHECK(evaluated(R"({"fn": "stdev", "args": [{"lit": [1, 2]}, {"ref": "p"}]})", R"({"p": 19})") ==
          "null" + std::string{kStatArgs});
    CHECK(evaluated(R"({"fn": "stdev", "args": [{"lit": [1, 2]}, {"ref": "p"}]})", R"({"p": "two"})") ==
          "null" + std::string{kStatArgs});
}

// Mutation: drop the start check (a negative start then reads from the end).
TEST_CASE("ui expression edges: slice takes a whole start and count of 0 or more", "[ui][expr]") {
    constexpr std::string_view kSlice = " | `slice` takes a list, a start and a count of 0 or more";
    CHECK(evaluated(R"({"fn": "slice", "args": [{"lit": [1, 2, 3]}, 0, {"ref": "n"}]})", R"({"n": 0})") == "[]");
    CHECK(evaluated(R"({"fn": "slice", "args": [{"lit": [1, 2, 3]}, 0, {"ref": "n"}]})", R"({"n": -1})") ==
          "null" + std::string{kSlice});
    CHECK(evaluated(R"({"fn": "slice", "args": [{"lit": [1, 2, 3]}, 0, {"ref": "n"}]})", R"({"n": "all"})") ==
          "null" + std::string{kSlice});
    CHECK(evaluated(R"({"fn": "slice", "args": [{"lit": [1, 2, 3]}, {"ref": "n"}]})", R"({"n": "one"})") ==
          "null" + std::string{kSlice});
    CHECK(evaluated(R"({"fn": "slice", "args": [{"ref": "xs"}, 0]})", R"({"xs": "abc"})") ==
          "null" + std::string{kSlice});
}

// Mutation: let `sort` read any order as ascending (`up` then sorts).
TEST_CASE("ui expression edges: sort runs over a list, in an order that is asc or desc", "[ui][expr]") {
    CHECK(evaluated(R"({"sort": {"ref": "xs"}, "as": "x", "by": {"ref": "x"}})", R"({"xs": "abc"})") ==
          "null | `sort` runs over a list");
    CHECK(evaluated(R"({"sort": {"lit": [2, 1]}, "as": "x", "by": {"ref": "x"}, "order": {"ref": "o"}})",
                    R"({"o": 1})") == R"(null | the `order` of `sort` is "asc" or "desc")");
    CHECK(evaluated(R"({"sort": {"lit": [1, 2]}, "as": "x", "by": {"ref": "x"}, "order": {"ref": "o"}})",
                    R"({"o": "desc"})") == "[2,1]");
    CHECK(evaluated(R"({"sort": {"lit": [2, 1]}, "as": "x", "by": {"ref": "x"}, "order": {"ref": "o"}})",
                    R"({"o": null})") == "[1,2]");
}

// Mutation: let dateAdd move a date by hours (`2026-11-03` plus 1 hour then answers a date).
TEST_CASE("ui expression edges: dateAdd takes a readable date, an int and a unit, and stays in years 1 to 9999",
          "[ui][expr]") {
    constexpr std::string_view kArgs = " | `dateAdd` takes a date, a timestamp or ISO text, an int and a unit";
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026-11-03", 1, {"ref": "u"}]})", R"({"u": "fortnights"})") ==
          "null" + std::string{kArgs});
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026-11-03", 1, {"ref": "u"}]})", R"({"u": 3})") ==
          "null" + std::string{kArgs});
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026-11-03", {"ref": "n"}, "days"]})", R"({"n": "one"})") ==
          "null" + std::string{kArgs});
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026-1x-03", 1, "days"]})") == "null" + std::string{kArgs});
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026-02-30", 1, "days"]})") == "null" + std::string{kArgs});
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026-11-03", 1, "hours"]})") ==
          "null | a date has no time of day; add hours to a timestamp");
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["9999-12-31", 1, "days"]})") == "null | the date is out of range");
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026-11-03", 3652060, "days"]})") ==
          "null | the date is out of range");
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026-11-03T08:30:00Z", 90, "minutes"]})") ==
          R"("2026-11-03T10:00:00.000Z")");
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["9999-12-31T23:30:00Z", 60, "minutes"]})") ==
          "null | the date is out of range");
}

// Mutation: wrap an index past the end onto the last element (`xs.5` then reads 3).
TEST_CASE("ui expression edges: a path into a list reads an element by its index, and nothing past the end",
          "[ui][expr]") {
    CHECK(evaluated(R"({"ref": "xs.1"})", R"({"xs": [1, 2, 3]})") == "2");
    CHECK(evaluated(R"({"ref": "xs.5"})", R"({"xs": [1, 2, 3]})") == "null");
    CHECK(evaluated(R"({"ref": "xs.first"})", R"({"xs": [1, 2, 3]})") == "null");
    CHECK(evaluated(R"({"ref": "xs.1x"})", R"({"xs": [1, 2, 3]})") == "null");
}

// Mutation: skip the unit conversion of the statistics (the stdev of 1 g and 2 kg then answers 0.71).
TEST_CASE("ui expression edges: the statistics refuse units that do not convert and sums that overflow",
          "[ui][expr]") {
    CHECK(evaluated(R"({"fn": "stdev", "args": [{"lit": [{"num": 1, "den": 1, "dp": 0, "unit": "g"},
        {"num": 2, "den": 1, "dp": 0, "unit": "kg"}]}, 2]})") == "null | the elements' units do not convert");
    // Three denominators near 10^9 have no common one in 64 bits.
    CHECK(evaluated(R"({"fn": "stdev", "args": [{"list": [{"fn": "div", "args": [1, 999999937]},
        {"fn": "div", "args": [1, 999999929]}, {"fn": "div", "args": [1, 999999893]}]}, 2]})") ==
          "null | the statistics overflow");
    // At 18 places the RSD's scale, 10^40, is past 128 bits.
    CHECK(evaluated(R"({"fn": "rsd", "args": [{"lit": [1, 2]}, 18]})") == "null | the statistics overflow");
    CHECK(evaluated(R"({"fn": "stdev", "args": [{"lit": [0, 1000000000000]}, 18]})") ==
          "null | the statistics overflow");
}

// Mutation: add days to an instant as hours (the instant then moves 2 hours).
TEST_CASE("ui expression edges: dateAdd moves an instant by days and weeks at its time of day", "[ui][expr]") {
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026-11-03T08:30:00Z", 2, "days"]})") ==
          R"("2026-11-05T08:30:00.000Z")");
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026-11-03T08:30:00Z", 1, "weeks"]})") ==
          R"("2026-11-10T08:30:00.000Z")");
    // Ten characters that are not `yyyy-MM-dd` are read as ISO text, which this is not.
    CHECK(evaluated(R"({"fn": "dateAdd", "args": ["2026/11/03", 1, "days"]})") ==
          "null | `dateAdd` takes a date, a timestamp or ISO text, an int and a unit");
}

// Mutation: compare a string before a number as equal (the sort then answers the list unchanged).
TEST_CASE("ui expression edges: a sort of a string and a number is refused in either order", "[ui][expr]") {
    CHECK(evaluated(R"({"sort": {"lit": ["a", 1]}, "as": "x", "by": {"ref": "x"}})") ==
          "null | the `by` values of `sort` are not of one comparable kind");
}
