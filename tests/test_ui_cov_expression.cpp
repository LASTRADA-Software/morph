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

TEST_CASE("cov expression: the functions give null and report what they cannot compute", "[ui][expr][cov][r3]") {
    for (
        Case const& item : std::vector<Case>{
            {R"({"fn":"add","args":[1,"x"]})", "{}", R"(null | an arithmetic operand is not a number)"},
            {R"({"fn":"add","args":[9223372036854775807,1]})", "{}", R"(null | the result overflows)"},
            {R"({"fn":"sub","args":[-9223372036854775807,9223372036854775807]})", "{}",
             R"(null | the result overflows)"},
            {R"({"fn":"mul","args":[9223372036854775807,2]})", "{}", R"(null | the result overflows)"},
            {R"({"fn":"div","args":[1,0]})", "{}", R"(null | division by zero)"},
            {R"({"fn":"div","args":[1,"x"]})", "{}", R"(null | an arithmetic operand is not a number)"},
            {R"({"fn":"div","args":[7,2]})", "{}", R"({"num":7,"den":2,"dp":0})"},
            {R"({"fn":"neg","args":["x"]})", "{}", R"(null | `neg` takes a number)"},
            {R"({"fn":"neg","args":[-9223372036854775807]})", "{}", R"(9223372036854775807)"},
            {R"({"fn":"abs","args":["x"]})", "{}", R"(null | `abs` takes a number)"},
            {R"({"fn":"abs","args":[-3]})", "{}", R"(3)"},
            {R"({"fn":"abs","args":[{"lit":{"num":-5,"den":2,"dp":1}}]})", "{}", R"({"num":5,"den":2,"dp":1})"},
            {R"({"fn":"round","args":[{"lit":{"num":15,"den":10,"dp":1}},0]})", "{}", R"({"num":2,"den":1,"dp":0})"},
            {R"({"fn":"round","args":[{"lit":{"num":25,"den":10,"dp":1}},0,"halfUp"]})", "{}",
             R"({"num":3,"den":1,"dp":0})"},
            {R"({"fn":"round","args":[{"lit":{"num":25,"den":10,"dp":1}},19]})", "{}",
             R"(null | `round` takes a number and places from 0 to 18)"},
            {R"({"fn":"round","args":["x",1]})", "{}", R"(null | `round` takes a number and places from 0 to 18)"},
            {R"({"fn":"sqrt","args":[-4,2]})", "{}", R"(null | the square root of a negative number)"},
            {R"({"fn":"sqrt","args":["x",2]})", "{}",
             R"(null | `sqrt` takes a plain number, places from 0 to 18 and a rounding mode)"},
            {R"({"fn":"sqrt","args":[16,0]})", "{}", R"({"num":4,"den":1,"dp":0})"},
            {R"({"fn":"sqrt","args":[{"lit":{"num":1,"den":4,"dp":2}},2]})", "{}", R"({"num":1,"den":2,"dp":2})"},
            {R"({"fn":"stdev","args":[{"lit":[1,2,3,4]},3]})", "{}", R"({"num":1291,"den":1000,"dp":3})"},
            {R"({"fn":"stdev","args":[{"lit":[9223372036854775807,1]},2]})", "{}",
             R"(null | the statistics overflow)"},
            {R"({"fn":"stdev","args":[{"lit":[3000000000,1,2]},2]})", "{}", R"({"num":17320508067,"den":10,"dp":2})"},
            {R"({"fn":"stdev","args":[{"lit":[{"num":9223372036854775807,"den":3,"dp":0},{"num":1,"den":7,"dp":0}]},4]})",
             "{}", R"(null | the statistics overflow)"},
            {R"({"fn":"stdev","args":[5,2]})", "{}",
             R"(null | the statistics take a list, places from 0 to 18 and a rounding mode)"},
            {R"({"fn":"stdev","args":[{"lit":[1,2]},2,"halfUp"]})", "{}", R"({"num":71,"den":100,"dp":2})"},
            {R"({"fn":"stdev","args":[{"lit":[1,2]},18]})", "{}",
             R"({"num":176776695296636881,"den":250000000000000000,"dp":18})"},
            {R"({"fn":"rsd","args":[{"lit":[0,0]},2]})", "{}", R"(null | the RSD of values whose mean is zero)"},
            {R"({"fn":"rsd","args":[{"lit":[10,12,14]},2]})", "{}", R"({"num":1667,"den":100,"dp":2})"},
            {R"({"fn":"rsd","args":[{"lit":[1,2]},19]})", "{}",
             R"(null | the statistics take a list, places from 0 to 18 and a rounding mode)"},
            {R"({"fn":"mean","args":[{"lit":[]}]})", "{}", R"(null)"},
            {R"({"fn":"mean","args":[{"lit":[1,"x"]}]})", "{}", R"(null | an element is not a number)"},
            {R"({"fn":"mean","args":[{"lit":[1,2]}]})", "{}", R"(2)"},
            {R"({"fn":"mean","args":[5]})", "{}", R"(null | the argument is not a list)"},
            {R"({"fn":"sum","args":[{"lit":[9223372036854775807,1]}]})", "{}", R"(null | the sum overflows)"},
            {R"({"fn":"sum","args":[{"lit":[1,null,2]}]})", "{}", R"(null)"},
            {R"({"fn":"min","args":[{"lit":[]}]})", "{}", R"(null)"},
            {R"({"fn":"max","args":[{"lit":[1,"a"]}]})", "{}",
             R"(null | the elements are not of one comparable kind)"},
            {R"({"fn":"min","args":[{"lit":[3,1,2]}]})", "{}", R"(1)"},
            {R"({"fn":"count","args":[5]})", "{}", R"(null | the argument is not a list)"},
            {R"({"fn":"first","args":[{"lit":[]}]})", "{}", R"(null)"},
            {R"({"fn":"first","args":["x"]})", "{}", R"(null | the argument is not a list)"},
            {R"({"fn":"length","args":[5]})", "{}", R"(null | `length` takes a string)"},
            {R"({"fn":"length","args":["héllo"]})", "{}", R"(5)"},
            {R"({"fn":"trim","args":[5]})", "{}", R"(null | `trim` takes a string)"},
            {R"({"fn":"trim","args":["  x "]})", "{}", R"("x")"},
            {R"({"fn":"concat","args":["a",5,null,true]})", "{}", R"(null)"},
            {R"({"fn":"includes","args":[5,1]})", "{}", R"(null | the first argument is not a list)"},
            {R"({"fn":"includes","args":[{"lit":[1,2]},2]})", "{}", R"(true)"},
            {R"({"fn":"indexOf","args":[5,1]})", "{}", R"(null | the first argument is not a list)"},
            {R"({"fn":"indexOf","args":[{"lit":[1,2]},3]})", "{}", R"(null)"},
            {R"({"fn":"slice","args":[{"lit":[1,2,3]},1]})", "{}", R"([2,3])"},
            {R"({"fn":"slice","args":[{"lit":[1,2,3]},-1,2]})", "{}",
             R"(null | `slice` takes a list, a start and a count of 0 or more)"},
            {R"({"fn":"slice","args":[{"lit":[1,2,3]},2,1]})", "{}", R"([3])"},
            {R"({"fn":"slice","args":[{"lit":[1,2,3]},0,9]})", "{}", R"([1,2,3])"},
            {R"({"fn":"slice","args":["x",0,1]})", "{}",
             R"(null | `slice` takes a list, a start and a count of 0 or more)"},
            {R"({"fn":"concatLists","args":[{"lit":[1]},5]})", "{}", R"(null | `concatLists` takes lists)"},
            {R"({"fn":"concatLists","args":[{"lit":[1]},{"lit":[2]}]})", "{}", R"([1,2])"},
            {R"({"fn":"number","args":["x"]})", "{}", R"(null | `number` takes a number)"},
            {R"({"fn":"number","args":["12.5"]})", "{}", R"(null | `number` takes a number)"},
            {R"({"fn":"number","args":[true]})", "{}", R"(null | `number` takes a number)"},
            {R"({"fn":"stdev","args":[{"lit":[1000000000000,1,2,3000000000000]},18]})", "{}",
             R"(null | the statistics overflow)"},
            {R"({"fn":"rsd","args":[{"lit":[1000000000000,1,2,3000000000000]},18]})", "{}",
             R"(null | the statistics overflow)"},
            {R"({"fn":"rsd","args":[{"lit":[{"num":5,"den":1,"dp":0},-5]},2]})", "{}",
             R"(null | the RSD of values whose mean is zero)"},
            {R"({"fn":"stdev","args":[{"lit":[{"num":1,"den":1,"dp":0,"unit":"kg"},{"num":3,"den":1,"dp":0,"unit":"g"}]},2]})",
             "{}", R"(null | the elements' units do not convert)"},
            {R"({"fn":"stdev","args":[{"lit":[1,{"num":3,"den":1,"dp":0,"unit":"g"}]},2]})", "{}",
             R"(null | quantities and plain numbers do not combine)"},
            {R"({"fn":"format","args":["{0} and {1}",1]})", "{}",
             R"(null | `format`'s template has a placeholder with no argument)"},
            {R"({"fn":"format","args":[5,1]})", "{}", R"(null | `format`'s template is a string)"},
            {R"({"fn":"format","args":["{0}",{"lit":[1]}]})", "{}", R"(null | `format` cannot show argument 2)"},
            {R"({"fn":"date","args":["2026-10-20","long"]})", "{}", R"("2026-10-20")"},
            {R"({"fn":"date","args":[5]})", "{}",
             R"(null | the date functions take a date, a timestamp or ISO text, and a style)"},
            {R"({"fn":"date","args":["2026-02-30"]})", "{}",
             R"(null | the date functions take a date, a timestamp or ISO text, and a style)"},
            {R"({"fn":"dateTime","args":["2026-10-20T10:00:00Z","short"]})", "{}", R"("2026-10-20 10:00")"},
            {R"({"fn":"dateTime","args":["nope"]})", "{}",
             R"(null | the date functions take a date, a timestamp or ISO text, and a style)"},
            {R"({"fn":"dateAdd","args":["2026-01-31",1,"months"]})", "{}", R"("2026-02-28")"},
            {R"({"fn":"dateAdd","args":["2026-01-31","x","months"]})", "{}",
             R"(null | `dateAdd` takes a date, a timestamp or ISO text, an int and a unit)"},
            {R"({"fn":"dateAdd","args":["2026-01-31",1,5]})", "{}",
             R"(null | `dateAdd` takes a date, a timestamp or ISO text, an int and a unit)"},
            {R"({"fn":"dateAdd","args":["2026-01-31",99999999,"years"]})", "{}", R"(null | the date is out of range)"},
            {R"({"fn":"dateAdd","args":["9999-12-31",1,"days"]})", "{}", R"(null | the date is out of range)"},
            {R"({"fn":"dateAdd","args":["0001-01-01",-1,"days"]})", "{}", R"(null | the date is out of range)"},
            {R"({"fn":"dateAdd","args":["2026-01-31T10:00:00Z",90,"minutes"]})", "{}",
             R"("2026-01-31T11:30:00.000Z")"},
            {R"({"fn":"dateAdd","args":["",1,"days"]})", "{}", R"(null)"},
            {R"({"fn":"coalesce","args":[null,null]})", "{}", R"(null)"},
            {R"({"fn":"engaged","args":["x"]})", "{}", R"(true)"},
            {R"({"fn":"engaged","args":[""]})", "{}", R"(true)"},
            {R"({"fn":"isEmpty","args":[{"lit":[]}]})", "{}", R"(true)"},
            {R"({"fn":"contains","args":["abc","b"]})", "{}", R"(true)"},
            {R"({"fn":"contains","args":[5,"b"]})", "{}", R"(null | `contains` takes two strings)"},
            {R"({"fn":"lt","args":[1,"a"]})", "{}", R"(null | the operands are not of one comparable kind)"},
            {R"({"fn":"lt","args":["a","b"]})", "{}", R"(true)"},
            {R"({"fn":"ge","args":[{"lit":{"num":1,"den":2,"dp":1}},0]})", "{}", R"(true)"},
            {R"({"fn":"eq","args":[{"lit":[1]},{"lit":[1]}]})", "{}", R"(true)"},
            {R"({"fn":"and","args":[true,"x"]})", "{}", R"(null | a logical operand is not a bool)"},
            {R"({"fn":"or","args":[false,null]})", "{}", R"(null)"},
            {R"({"fn":"not","args":["x"]})", "{}", R"(null | `not` takes a bool)"},
            {R"({"fn":"any","args":[{"lit":[false,true]}]})", "{}", R"(true)"},
            {R"({"fn":"all","args":[{"lit":[true,1]}]})", "{}", R"(null | an element is not a bool)"},
            {R"({"fn":"any","args":[5]})", "{}", R"(null | the argument is not a list)"}}) {
        INFO(item.expr);
        CHECK(run(item.expr, item.names) == item.expected);
    }
}
