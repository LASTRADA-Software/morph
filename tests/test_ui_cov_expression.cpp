// SPDX-License-Identifier: Apache-2.0

// expr/1 over inputs a document can only compute or spell wrongly: each case is an expression, the names it reads, the
// value it gives as JSON and the reports it makes.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace ui = morph::ui;

namespace {

// An expression's value as JSON text, then ` | ` and the reason of each report; `refused: ` and the reason when the
// parser refuses it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): an expression, then the names it reads, as JSON text.
std::string run(std::string_view expr, std::string_view names = "{}") {
    auto const json = ui::parseJson(expr);
    REQUIRE(json.has_value());
    auto const parsed = ui::parseExpression(*json);
    if (!parsed) {
        return "refused: " + parsed.error().reason;
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

struct Case {
    std::string_view expr;
    std::string_view names;
    std::string_view expected;
};

}  // namespace

TEST_CASE("cov expression: arguments a document can only compute are reported and give null", "[ui][expr][cov][r3]") {
    for (
        Case const& item : std::vector<Case>{
            {R"({"fn":"money","args":[5,"EUR","code"]})", R"({})", R"("5.00 EUR")"},
            {R"({"fn":"money","args":[5,"EUR","symbol"]})", R"({})", R"("5.00 EUR")"},
            {R"({"fn":"money","args":[5,"EUR","words"]})", R"({})", R"(null | `money`'s form is "symbol" or "code")"},
            {R"({"fn":"money","args":[5,"EUR",7]})", R"({})", R"(null | `money`'s form is "symbol" or "code")"},
            {R"({"fn":"money","args":["x","EUR"]})", R"({})", R"(null | `money` takes an amount and a currency code)"},
            {R"({"fn":"parseDecimal","args":["1.5",19]})", R"({})",
             R"(null | `parseDecimal` takes text and places from 0 to 18)"},
            {R"({"fn":"parseDecimal","args":["1.5",-1]})", R"({})",
             R"(null | `parseDecimal` takes text and places from 0 to 18)"},
            {R"({"fn":"parseDecimal","args":[5,2]})", R"({})",
             R"(null | `parseDecimal` takes text and places from 0 to 18)"},
            {R"({"fn":"parseDecimal","args":["1.5","x"]})", R"({})",
             R"(null | `parseDecimal` takes text and places from 0 to 18)"},
            {R"({"fn":"dateRange","args":["2026-10-20","2026-10-24"]})", R"({})", R"("20–24 Oct 2026")"},
            {R"({"fn":"dateRange","args":["2026-10-20","2026-13-40"]})", R"({})",
             R"(null | `dateRange` takes two dates, timestamps or ISO text)"},
            {R"({"fn":"dateRange","args":["2026-10-20",""]})", R"({})", R"(null)"},
            {R"({"fn":"dateRange","args":["","2026-10-20"]})", R"({})", R"(null)"},
            {R"({"fn":"dateRange","args":["2026-10-20",5]})", R"({})",
             R"(null | `dateRange` takes two dates, timestamps or ISO text)"},
            {R"({"fn":"dateRange","args":["2026-10-20","2026-1x-20"]})", R"({})",
             R"(null | `dateRange` takes two dates, timestamps or ISO text)"},
            {R"({"fn":"dateRange","args":["2026-10-20","2026-10-24T10:00:00Z"]})", R"({})", R"("20–24 Oct 2026")"},
            {R"({"fn":"dateAdd","args":["2026-01-01",1,"fortnights"]})", R"({})",
             R"(null | `dateAdd` takes a date, a timestamp or ISO text, an int and a unit)"},
            {R"({"fn":"dateAdd","args":[5,1,"days"]})", R"({})",
             R"(null | `dateAdd` takes a date, a timestamp or ISO text, an int and a unit)"},
            {R"({"fn":"refusalText","args":[{"ref":"r"}]})",
             R"({"r":{"code":"a.b","args":[{"name":"x","value":"1"},5,{"name":"y"}]}})",
             R"("This was refused." | an argument of the message is a record of a string `name` and `value`)"},
            {R"({"fn":"refusalText","args":[{"ref":"r"}]})", R"({"r":5})",
             R"(null | `refusalText` takes a refusal, a record with a string `code`)"},
            {R"({"fn":"refusalText","args":[{"ref":"r"}]})", R"({"r":{"code":3}})",
             R"(null | `refusalText` takes a refusal, a record with a string `code`)"},
            {R"({"fn":"refusalText","args":[{"ref":"r"}]})", R"({"r":{"code":"a.b","args":"x"}})",
             R"("This was refused." | the catalogue has no message for the refusal code `a.b`)"},
            {R"({"t":"k","fallback":"hello {$n}","args":{"n":"1"}})", R"({})", R"("hello 1")"},
            {R"({"t":"k","fallback":"hello {$n}","args":{"expr":{"ref":"l"}}})", R"({"l":[{"name":"n","value":"2"}]})",
             R"("hello 2")"},
            {R"({"t":"k","fallback":"hello {n}","args":{"n":"1"}})", R"({})",
             R"("hello {n}" | the fallback is outside the ui/1 subset: only variable placeholders `{$name}` are in the ui/1 subset)"},
            {R"({"t":"k","fallback":"hello {n}","args":{"expr":{"ref":"l"}}})",
             R"({"l":[{"name":"n","value":"1"},3]})",
             R"("hello {n}" | an argument of the message is a record of a string `name` and `value`)"},
            {R"({"t":"k","fallback":"hello {n}","args":{"expr":{"ref":"l"}}})", R"({"l":5})",
             R"(null | the arguments of `t` are a record, or a list of `{name, value}` records)"},
            {R"({"t":{"expr":{"ref":"k"}},"fallback":"f"})", R"({"k":"key"})",
             R"("f" | the catalogue has no message for the key `key`)"},
            {R"({"t":{"expr":{"bogus":1}},"fallback":"f"})", R"({})",
             R"(refused: `bogus` is not an expression form of expr/1)"},
            {R"({"t":"k"})", R"({})", R"(refused: `t` needs its `fallback` text as a string)"},
            {R"({"t":"k","fallback":"f","args":{"expr":{"bogus":1}}})", R"({})",
             R"(refused: `bogus` is not an expression form of expr/1)"},
            {R"({"t":"k","fallback":"f","args":{"n":{"bogus":1}}})", R"({})",
             R"(refused: `bogus` is not an expression form of expr/1)"},
            {R"({"fn":"sort","args":[1,2,3]})", R"({})", R"(refused: the function `sort` is not in expr/1)"}}) {
        INFO(item.expr << " with " << item.names);
        CHECK(run(item.expr, item.names) == item.expected);
    }
}
