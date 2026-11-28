// SPDX-License-Identifier: Apache-2.0

// expr/1 at the inputs the checker and the evaluator meet least: the types a checked expression has, the dates a
// text can only nearly spell, the row and the refusals an environment holds, and statistics whose exact sums leave
// 64 bits. Each case is an expression with the names it reads and the value or type it gives.

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <glaze/glaze.hpp>
#include <map>
#include <morph/ui/expression.hpp>
#include <morph/ui/value.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui = morph::ui;

namespace {

// An expression's value as JSON text, then ` | ` and the reason of each report; `refused: ` and the reason when the
// parser refuses it. The names' `$row` member is the environment's row and `$refusals` its action refusals.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a test helper; the names say which text is which.
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
    if (auto const row = env.names.find("$row"); row != env.names.end()) {
        env.rowValue = row->second;
    }
    if (auto const refusals = env.names.find("$refusals"); refusals != env.names.end()) {
        env.refusals = *refusals->second.asRecord();
    }
    std::string out = glz::write_json(ui::valueToJson(ui::evaluate(**parsed, env))).value_or("?");
    for (ui::Refusal const& report : env.diagnostics) {
        out += " | " + report.reason;
    }
    return out;
}

// Names typed as the test says; any other name is `any`.
struct Typed final : ui::NameResolver {
    std::map<std::string, ui::Type, std::less<>> types;

    [[nodiscard]] std::expected<ui::Type, std::string> ref(std::string_view head,
                                                           std::span<std::string const> /*path*/) const override {
        auto const found = types.find(head);
        return found == types.end() ? ui::Type::any() : found->second;
    }
    [[nodiscard]] std::expected<ui::Type, std::string> row(std::span<std::string const> /*path*/) const override {
        return ui::Type::any();
    }
    [[nodiscard]] std::expected<ui::Type, std::string> control(ui::ExprKind /*form*/,
                                                               std::string_view /*name*/) const override {
        return ui::Type::any();
    }
    [[nodiscard]] std::expected<ui::Type, std::string> pref(std::string_view /*point*/,
                                                            std::span<std::string const> /*path*/) const override {
        return ui::Type::any();
    }
};

// The type a checked expression has, or `refused: ` and the reason.
std::string checked(std::string_view expr, Typed const& names) {
    auto const json = ui::parseJson(expr);
    REQUIRE(json.has_value());
    auto const parsed = ui::parseExpression(*json);
    if (!parsed) {
        return "refused: " + parsed.error().reason;
    }
    auto const type = ui::checkExpression(**parsed, names);
    return type ? type->name() : "refused: " + type.error().reason;
}

struct Case {
    std::string_view expr;
    std::string_view names;
    std::string_view expected;
};

}  // namespace

TEST_CASE("cov h expression: the environment's row, refusals and the paths of a get", "[covh][ui][expr]") {
    for (Case const& item : std::vector<Case>{
             {.expr = R"({"get":{"row":"a"},"path":"b"})", .names = R"({"$row":{"a":{"b":7}}})", .expected = "7"},
             {.expr = R"({"get":{"row":"a"},"path":"z"})", .names = R"({"$row":{"a":{"b":7}}})", .expected = "null"},
             {.expr = R"({"get":{"row":""},"path":"a"})", .names = R"({"$row":{"a":3}})", .expected = "3"},
             {.expr = R"({"refusal":"save"})",
              .names = R"({"$refusals":{"save":{"code":"x.y"}}})",
              .expected = R"({"code":"x.y"})"},
             {.expr = R"({"refusal":"other"})",
              .names = R"({"$refusals":{"save":{"code":"x.y"}}})",
              .expected = "null"},
             {.expr = R"({"fn":"refusalText","args":[{"ref":"r"}]})",
              .names = R"({"r":{"code":"a.b"}})",
              .expected = R"("This was refused." | the catalogue has no message for the refusal code `a.b`)"},
         }) {
        INFO(item.expr << " with " << item.names);
        CHECK(run(item.expr, item.names) == item.expected);
    }
}

TEST_CASE("cov h expression: a date a text only nearly spells is not a date", "[covh][ui][expr]") {
    constexpr std::string_view kUnreadable = "null | `dateRange` takes two dates, timestamps or ISO text";
    std::vector<std::string> unreadable;
    for (std::string_view const text : {"2026-10x20", "abcd-10-20", "2026-ab-20", "2026-10-ab", "2026-10-2x",
                                        "2026/10/20", "2026-13-01", "2026-02-30"}) {
        unreadable.emplace_back(text);
    }
    for (std::string const& text : unreadable) {
        INFO(text);
        std::string const spelled = R"({"fn":"dateRange","args":[")" + text + R"(","2026-10-20"]})";
        CHECK(run(spelled) == kUnreadable);
        std::string const reversed = R"({"fn":"dateRange","args":["2026-10-20",")" + text + R"("]})";
        CHECK(run(reversed) == kUnreadable);
    }
    CHECK(run(R"({"fn":"dateRange","args":[5,"2026-10-20"]})") == kUnreadable);
    CHECK(run(R"({"fn":"dateRange","args":[5,5]})") == kUnreadable);
}

TEST_CASE("cov h expression: a comprehension over what is not a list says so, and sorts by direction",
          "[covh][ui][expr]") {
    for (Case const& item : std::vector<Case>{
             {.expr = R"({"runs":{"ref":"x"},"as":"i","by":{"ref":"i"}})",
              .names = R"({"x":5})",
              .expected = "null | `runs` runs over a list"},
             {.expr = R"({"runs":{"ref":"x"},"as":"i","by":{"ref":"i"}})",
              .names = R"({"x":null})",
              .expected = "null"},
             {.expr = R"({"sort":{"lit":[2,1,3]},"as":"x","by":{"ref":"x"},"order":"asc"})",
              .names = "{}",
              .expected = "[1,2,3]"},
             {.expr = R"({"sort":{"lit":[2,1,3]},"as":"x","by":{"ref":"x"},"order":"desc"})",
              .names = "{}",
              .expected = "[3,2,1]"},
             {.expr = R"({"sort":{"lit":[2,1,3]},"as":"x","by":{"ref":"x"},"order":"up"})",
              .names = "{}",
              .expected = R"(null | the `order` of `sort` is "asc" or "desc")"},
             {.expr = R"({"sort":{"lit":[2,1,3]},"as":"x","by":{"ref":"x"},"order":5})",
              .names = "{}",
              .expected = R"(null | the `order` of `sort` is "asc" or "desc")"},
             {.expr = R"({"sort":{"lit":[2,1,3]},"as":"x","by":{"ref":"x"},"order":null})",
              .names = "{}",
              .expected = "[1,2,3]"},
         }) {
        INFO(item.expr << " with " << item.names);
        CHECK(run(item.expr, item.names) == item.expected);
    }
}

TEST_CASE("cov h expression: the type of a checked expression follows its arguments", "[covh][ui][expr]") {
    Typed names;
    names.types.emplace("ints", ui::Type::listOf(ui::Type::of(ui::TypeKind::Int)));
    names.types.emplace("at", ui::Type::of(ui::TypeKind::Timestamp));
    names.types.emplace("text", ui::Type::of(ui::TypeKind::String));
    names.types.emplace("n", ui::Type::of(ui::TypeKind::Int));
    struct Typing {
        std::string_view expr;
        std::string_view expected;
    };
    for (Typing const& item : std::vector<Typing>{
             {.expr = R"({"sort":{"ref":"ints"},"as":"x","by":{"ref":"x"}})", .expected = "list<int>"},
             {.expr = R"({"sort":{"ref":"ints"},"as":"x","by":{"ref":"x"},"order":"asc"})", .expected = "list<int>"},
             {.expr = R"({"sort":{"ref":"ints"},"as":"x","by":{"ref":"x"},"order":{"ref":"n"}})",
              .expected = "refused: the `order` of `sort` is int; expected string"},
             {.expr = R"({"fn":"slice","args":[{"ref":"anything"},0,1]})", .expected = "list<any>?"},
             {.expr = R"({"fn":"slice","args":[{"ref":"ints"},0,1]})", .expected = "list<int>?"},
             {.expr = R"({"fn":"dateAdd","args":["2026-01-01",1,"days"]})", .expected = "string?"},
             {.expr = R"({"fn":"dateAdd","args":[{"ref":"text"},1,"days"]})", .expected = "string?"},
             {.expr = R"({"fn":"dateAdd","args":[{"ref":"at"},1,"hours"]})", .expected = "timestamp?"},
             {.expr = R"({"fn":"dateAdd","args":["2026-01-01",1,"fortnights"]})",
              .expected = "refused: the unit is `days`, `weeks`, `months`, `years`, `hours` or `minutes`"},
             {.expr = R"({"fn":"concatLists","args":[{"ref":"anything"},{"ref":"ints"}]})", .expected = "list<int>?"},
             {.expr = R"({"fn":"concatLists","args":[{"ref":"ints"},{"lit":["a"]}]})", .expected = "list<int>?"},
         }) {
        INFO(item.expr);
        CHECK(checked(item.expr, names) == item.expected);
    }
}

TEST_CASE("cov h expression: statistics whose exact sums leave 64 bits are refused", "[covh][ui][expr]") {
    constexpr std::string_view kOverflow = "null | the statistics overflow";
    // Two decimals whose denominators have no common factor, so that their least common multiple passes int64.
    std::string const coprime = R"({"lit":[{"num":1,"den":3037000500,"dp":18},{"num":1,"den":3037000501,"dp":18}]})";
    // A value of 9e18 beside one of a thousandth: the common scale makes its square wider than 128 bits.
    std::string const wide = R"({"lit":[9000000000000000000,{"num":1,"den":1000,"dp":3}]})";
    for (char const* const function : {"stdev", "rsd"}) {
        for (auto const& [what, list] : std::vector<std::pair<char const*, std::string>>{
                 {"denominators with no common multiple in int64", coprime}, {"squares wider than 128 bits", wide}}) {
            INFO(function << ": " << what);
            std::string const expr = std::string{R"({"fn":")"} + function + R"(","args":[)" + list + ",2]}";
            std::string const got = run(expr);
            CHECK(got.starts_with("null | "));
            CHECK(got.contains("overflow"));
        }
    }
    CHECK(run(R"({"fn":"stdev","args":[{"lit":[1,2,3]},2]})") != std::string{kOverflow});
}
