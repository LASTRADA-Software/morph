// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <morph/render/i18n.hpp>
#include <morph/render/locale_format.hpp>
#include <morph/ui/expression.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui = morph::ui;
namespace math = morph::math;

namespace {

// A context answering from maps; each reader the test does not fill answers null.
class FakeContext : public ui::ExpressionContext {
public:
    std::map<std::string, ui::Value, std::less<>> names;
    std::map<std::string, ui::Value, std::less<>> queries;
    std::map<std::string, ui::Value, std::less<>> results;
    std::map<std::string, ui::Value, std::less<>> errors;
    std::map<std::string, ui::Value, std::less<>> attributes;
    ui::Value currentRow;
    ui::Value payload;
    ui::UnitTable table;
    std::string activeLocale = "en";
    morph::render::NumericLocale numeric;
    std::chrono::minutes offset{0};
    morph::render::TranslationProvider catalogue;

    [[nodiscard]] ui::Value name(std::string_view name) const override { return from(names, name); }
    [[nodiscard]] ui::Value row() const override { return currentRow; }
    [[nodiscard]] ui::Value event() const override { return payload; }
    [[nodiscard]] ui::Value query(std::string_view query) const override { return from(queries, query); }
    [[nodiscard]] ui::Value result(std::string_view mutation) const override { return from(results, mutation); }
    [[nodiscard]] ui::Value pending(std::string_view name) const override {
        return ui::Value{std::string_view{name} == "saving"};
    }
    [[nodiscard]] ui::Value error(std::string_view name) const override { return from(errors, name); }
    [[nodiscard]] ui::Value errorKind(std::string_view name) const override {
        return from(errors, name).isNull() ? ui::Value{} : ui::Value{"validation"};
    }
    [[nodiscard]] ui::Value can(std::string_view name) const override {
        return ui::Value{std::string_view{name} == "approve"};
    }
    [[nodiscard]] ui::Value pref(std::string_view point) const override {
        return from(attributes, "pref." + std::string{point});
    }
    [[nodiscard]] ui::Value env(std::string_view attribute) const override {
        return from(attributes, "env." + std::string{attribute});
    }
    [[nodiscard]] ui::Value session(std::string_view attribute) const override {
        return from(attributes, "session." + std::string{attribute});
    }
    [[nodiscard]] ui::Value host(std::string_view name) const override {
        return from(attributes, "host." + std::string{name});
    }
    [[nodiscard]] ui::UnitTable const& units() const override { return table; }
    [[nodiscard]] std::string locale() const override { return activeLocale; }
    [[nodiscard]] morph::render::NumericLocale numericLocale() const override { return numeric; }
    [[nodiscard]] std::chrono::minutes displayOffset(morph::time::DateTime instant) const override {
        static_cast<void>(instant);
        return offset;
    }
    [[nodiscard]] morph::render::TranslationProvider const& translations() const override { return catalogue; }

private:
    [[nodiscard]] static ui::Value from(std::map<std::string, ui::Value, std::less<>> const& map,
                                        std::string_view key) {
        auto const found = map.find(key);
        return found == map.end() ? ui::Value{} : found->second;
    }
};

[[nodiscard]] ui::Expr expr(std::string_view text) {
    ui::Json json{};
    REQUIRE_FALSE(glz::read_json(json, text));
    std::expected<ui::Expr, ui::Problem> tree = ui::parseExpression(json, "/e");
    INFO(text);
    if (!tree) {
        INFO(tree.error().path << ": " << tree.error().reason);
    }
    REQUIRE(tree.has_value());
    return *tree;
}

// Evaluates @p text in @p context; the reports land in @p diagnostics.
[[nodiscard]] ui::Value run(std::string_view text, ui::ExpressionContext const& context,
                            ui::Diagnostics& diagnostics) {
    return ui::evaluate(expr(text), context, diagnostics);
}

[[nodiscard]] ui::Value run(std::string_view text, ui::ExpressionContext const& context = FakeContext{}) {
    ui::Diagnostics diagnostics;
    return run(text, context, diagnostics);
}

// The first report @p text makes, or empty when it makes none.
[[nodiscard]] std::string reportOf(std::string_view text, ui::ExpressionContext const& context = FakeContext{}) {
    ui::Diagnostics diagnostics;
    ui::Value const value = run(text, context, diagnostics);
    CHECK(value.isNull());
    return diagnostics.reported().empty() ? std::string{} : diagnostics.reported().front().reason;
}

[[nodiscard]] ui::Value decimal(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return ui::Value{math::Rational{math::Numerator{num}, math::Denominator{den}, math::DecimalPlaces{places}}};
}

[[nodiscard]] std::string dec(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return R"({"lit": {"num": )" + std::to_string(num) + R"(, "den": )" + std::to_string(den) + R"(, "dp": )" +
           std::to_string(places) + "}}";
}

[[nodiscard]] std::string qty(std::int64_t num, std::uint32_t places, std::string_view unit) {
    return R"({"lit": {"num": )" + std::to_string(num) + R"(, "den": 1, "dp": )" + std::to_string(places) +
           R"(, "unit": ")" + std::string{unit} + R"("}})";
}

// mg/L and g/L, one ratio between them.
[[nodiscard]] ui::UnitTable concentrations() {
    ui::UnitTable table;
    table.add({.id = "mg_per_l", .display = "mg/L", .places = 1});
    table.add({.id = "g_per_l", .display = "g/L", .places = 3});
    table.add({.id = "kg_per_m3", .display = "kg/m³", .places = 3});
    table.relate({.from = "g_per_l", .to = "mg_per_l", .fromTo = math::Rational{1000, math::DecimalPlaces{0}}});
    table.relate({.from = "kg_per_m3", .to = "g_per_l", .fromTo = math::Rational{1, math::DecimalPlaces{0}}});
    return table;
}

}  // namespace

// Mutation: two-valued logic (read null as false in `and`/`or`/`not`).
TEST_CASE("ui::evaluate: and, or and not follow Kleene's three-valued logic", "[ui][expr]") {
    std::vector<std::pair<std::string, ui::Value>> const truth{
        {"true", ui::Value{true}}, {"false", ui::Value{false}}, {"null", ui::Value{}}};
    auto const kleeneAnd = [](ui::Value const& lhs, ui::Value const& rhs) {
        if (lhs == ui::Value{false} || rhs == ui::Value{false}) {
            return ui::Value{false};
        }
        return lhs.isNull() || rhs.isNull() ? ui::Value{} : ui::Value{true};
    };
    auto const kleeneOr = [](ui::Value const& lhs, ui::Value const& rhs) {
        if (lhs == ui::Value{true} || rhs == ui::Value{true}) {
            return ui::Value{true};
        }
        return lhs.isNull() || rhs.isNull() ? ui::Value{} : ui::Value{false};
    };
    for (auto const& [lhsText, lhs] : truth) {
        CHECK(run(R"({"fn": "not", "args": [)" + lhsText + "]}") ==
              (lhs.isNull() ? ui::Value{} : ui::Value{!*lhs.asBool()}));
        for (auto const& [rhsText, rhs] : truth) {
            INFO(lhsText << " " << rhsText);
            std::string operands = lhsText;
            operands += ",";
            operands += rhsText;
            operands += "]}";
            CHECK(run(R"({"fn": "and", "args": [)" + operands) == kleeneAnd(lhs, rhs));
            CHECK(run(R"({"fn": "or", "args": [)" + operands) == kleeneOr(lhs, rhs));
        }
    }
    CHECK(run(R"({"fn": "and", "args": [true, null, false]})") == ui::Value{false});
    CHECK(run(R"({"fn": "any", "args": [{"list": [false, null]}]})").isNull());
    CHECK(run(R"({"fn": "all", "args": [{"list": [true, null, false]}]})") == ui::Value{false});
}

// Mutation: report on every evaluation (drop the seen-path check in Diagnostics::report).
TEST_CASE("ui::evaluate: a runtime error is null, reported once per expression path", "[ui][expr]") {
    std::vector<ui::Problem> sunk;
    ui::Diagnostics diagnostics{[&sunk](ui::Problem const& problem) { sunk.push_back(problem); }};
    FakeContext const context;
    ui::Expr const sum = expr(R"({"list": [{"fn": "add", "args": ["one", 1]}, {"fn": "div", "args": [1, 0]}]})");
    for (int round = 0; round < 3; ++round) {
        CHECK(ui::evaluate(sum, context, diagnostics) == ui::Value{ui::ValueList{ui::Value{}, ui::Value{}}});
    }
    CHECK(sunk == std::vector<ui::Problem>{{.path = "/e/list/0", .reason = "`add` takes numbers or quantities"},
                                           {.path = "/e/list/1", .reason = "division by zero"}});
    CHECK(diagnostics.reported() == sunk);
}

// Mutations: let `if` read a null condition as false; read a missing name's path from a stale value.
TEST_CASE("ui::evaluate: absence propagates through every form", "[ui][expr]") {
    CHECK(run(R"({"fn": "add", "args": [null, 1]})").isNull());
    CHECK(run(R"({"if": [null, 1, 2]})").isNull());
    CHECK(run(R"({"map": null, "as": "x", "to": 1})").isNull());
    CHECK(run(R"({"filter": {"ref": "missing"}, "as": "x", "where": true})").isNull());
    CHECK(run(R"({"ref": "missing.deeper.still"})").isNull());
    CHECK(run(R"({"row": "id"})").isNull());
    CHECK(run(R"({"fn": "concat", "args": ["a", null]})").isNull());
    CHECK(run(R"({"fn": "number", "args": [null]})").isNull());
    CHECK(run(R"({"obj": {"a": null}})") == ui::Value{ui::ValueRecord{{"a", ui::Value{}}}});
    CHECK(run(R"({"fn": "engaged", "args": [null]})") == ui::Value{false});
    CHECK(run(R"({"fn": "coalesce", "args": [null, null, 3, 4]})") == ui::Value{std::int64_t{3}});
    CHECK(run(R"({"fn": "coalesce", "args": [null]})").isNull());
}

// Mutations: route `row` to the names; read `pref`'s whole point instead of its path; drop the event's path.
TEST_CASE("ui::evaluate: each reading form asks the context its own question", "[ui][expr]") {
    FakeContext context;
    context.names["draft"] = ui::Value{ui::ValueRecord{{"title", "Report"}, {"meta", ui::ValueRecord{{"rev", true}}}}};
    context.currentRow = ui::Value{ui::ValueRecord{{"id", std::int64_t{7}}}};
    context.payload = ui::Value{ui::ValueRecord{{"text", "typed"}}};
    context.queries["samples"] = ui::Value{ui::ValueList{std::int64_t{1}}};
    context.results["save"] = ui::Value{"saved"};
    context.errors["load"] = ui::Value{"not found"};
    context.attributes["pref.columns"] = ui::Value{ui::ValueRecord{{"order", "name"}}};
    context.attributes["env.widthClass"] = ui::Value{"compact"};
    context.attributes["session.displayName"] = ui::Value{"Ada"};
    context.attributes["host.sync.count"] = ui::Value{std::int64_t{2}};
    CHECK(run(R"({"ref": "draft.title"})", context) == ui::Value{"Report"});
    CHECK(run(R"({"ref": "draft.meta.rev"})", context) == ui::Value{true});
    CHECK(run(R"({"row": "id"})", context) == ui::Value{std::int64_t{7}});
    CHECK(run(R"({"ref": "event.text"})", context) == ui::Value{"typed"});
    CHECK(run(R"({"ref": "event"})", context) == context.payload);
    CHECK(run(R"({"query": "samples"})", context) == context.queries["samples"]);
    CHECK(run(R"({"result": "save"})", context) == ui::Value{"saved"});
    CHECK(run(R"({"pending": "saving"})", context) == ui::Value{true});
    CHECK(run(R"({"error": "load"})", context) == ui::Value{"not found"});
    CHECK(run(R"({"errorKind": "load"})", context) == ui::Value{"validation"});
    CHECK(run(R"({"can": "approve"})", context) == ui::Value{true});
    CHECK(run(R"({"pref": "columns.order"})", context) == ui::Value{"name"});
    CHECK(run(R"({"env": "widthClass"})", context) == ui::Value{"compact"});
    CHECK(run(R"({"session": "displayName"})", context) == ui::Value{"Ada"});
    CHECK(run(R"({"host": "sync.count"})", context) == ui::Value{std::int64_t{2}});
}

// Mutations: keep a filtered element whose test is null; let the context's name win over a comprehension's.
TEST_CASE("ui::evaluate: constructors and comprehensions", "[ui][expr]") {
    FakeContext context;
    context.names["x"] = ui::Value{"outer"};
    context.queries["rows"] = ui::Value{ui::ValueList{ui::ValueRecord{{"id", std::int64_t{1}}, {"open", true}},
                                                      ui::ValueRecord{{"id", std::int64_t{2}}, {"open", ui::Value{}}},
                                                      ui::ValueRecord{{"id", std::int64_t{3}}, {"open", false}}}};
    CHECK(run(R"({"map": {"query": "rows"}, "as": "x", "to": {"ref": "x.id"}})", context) ==
          ui::Value{ui::ValueList{std::int64_t{1}, std::int64_t{2}, std::int64_t{3}}});
    CHECK(run(R"({"map": {"filter": {"query": "rows"}, "as": "r", "where": {"ref": "r.open"}}, "as": "r",
                  "to": {"ref": "r.id"}})",
              context) == ui::Value{ui::ValueList{std::int64_t{1}}});
    CHECK(run(R"({"map": {"lit": [1, 2]}, "as": "y", "to": {"ref": "x"}})", context) ==
          ui::Value{ui::ValueList{"outer", "outer"}});
    CHECK(run(R"({"obj": {"id": 1, "tags": {"list": ["a", {"ref": "x"}]}}})", context) ==
          ui::Value{ui::ValueRecord{{"id", std::int64_t{1}}, {"tags", ui::ValueList{"a", "outer"}}}});
    CHECK(reportOf(R"({"map": 3, "as": "x", "to": 1})") == "`map` takes a list");
    CHECK(reportOf(R"({"filter": {"lit": [1]}, "as": "x", "where": 1})") == "`where` takes a bool");
    CHECK(reportOf(R"({"if": [1, 2, 3]})") == "`if` takes a bool condition");
    CHECK(reportOf(R"({"fn": "and", "args": [true, 1]})") == "`and` takes bools");
}

// Mutations: compare decimals as doubles; ignore units (compare the bare amounts).
TEST_CASE("ui::evaluate: comparison is exact across int, decimal and quantity units", "[ui][expr]") {
    FakeContext context;
    context.table = concentrations();
    CHECK(run(R"({"fn": "eq", "args": [1, )" + dec(10, 10, 1) + "]}", context) == ui::Value{true});
    CHECK(run(R"({"fn": "lt", "args": [)" + dec(1, 3, 2) + ", " +
              dec(333'333'333'333'333'333, 1'000'000'000'000'000'000, 18) + "]}") == ui::Value{false});
    CHECK(run(R"({"fn": "gt", "args": [)" + dec(1, 3, 2) + ", " +
              dec(333'333'333'333'333'333, 1'000'000'000'000'000'000, 18) + "]}") == ui::Value{true});
    CHECK(run(R"({"fn": "eq", "args": [)" + qty(1000, 0, "mg_per_l") + ", " + qty(1, 0, "g_per_l") + "]}", context) ==
          ui::Value{true});
    CHECK(run(R"({"fn": "lt", "args": [)" + qty(999, 0, "mg_per_l") + ", " + qty(1, 0, "kg_per_m3") + "]}", context) ==
          ui::Value{true});
    CHECK(run(R"({"fn": "ge", "args": ["b", "a"]})") == ui::Value{true});
    context.names["early"] = ui::Value{*morph::time::DateTime::fromIso8601("2026-01-01T00:00:00Z")};
    context.names["late"] = ui::Value{*morph::time::DateTime::fromIso8601("2026-01-02T00:00:00Z")};
    CHECK(run(R"({"fn": "le", "args": [{"ref": "early"}, {"ref": "late"}]})", context) == ui::Value{true});
    CHECK(run(R"({"fn": "gt", "args": [{"ref": "early"}, {"ref": "late"}]})", context) == ui::Value{false});
    CHECK(run(R"({"fn": "ne", "args": [{"list": [1]}, {"list": [1]}]})") == ui::Value{false});
    CHECK(reportOf(R"({"fn": "eq", "args": [)" + qty(1, 0, "mg_per_l") + ", " + qty(1, 0, "furlong") + "]}",
                   context) == "no conversion from `furlong` to `mg_per_l`");
    CHECK(reportOf(R"({"fn": "lt", "args": [1, "1"]})") == "cannot compare an int with a string");
    CHECK(reportOf(R"({"fn": "lt", "args": [1, )" + qty(1, 0, "g_per_l") + "]}", context) ==
          "a quantity meets a plain number");
}

// Mutation: check overflow on `add` only (sub and mul then wrap).
TEST_CASE("ui::evaluate: arithmetic is exact, and overflow or division by zero is null", "[ui][expr]") {
    FakeContext context;
    context.table = concentrations();
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    CHECK(run(R"({"fn": "add", "args": [2, 3]})") == ui::Value{std::int64_t{5}});
    CHECK(run(R"({"fn": "sub", "args": [2, )" + dec(1, 2, 1) + "]}") == decimal(3, 2, 1));
    CHECK(run(R"({"fn": "div", "args": [1, 3]})") == decimal(1, 3, 0));
    CHECK(run(R"({"fn": "mul", "args": [)" + qty(2, 0, "g_per_l") + ", 3]}", context) ==
          ui::Value{ui::QuantityValue{.amount = math::Rational{6, math::DecimalPlaces{0}}, .unit = "g_per_l"}});
    CHECK(run(R"({"fn": "add", "args": [)" + qty(1, 0, "g_per_l") + ", " + qty(500, 0, "mg_per_l") + "]}", context) ==
          ui::Value{ui::QuantityValue{
              .amount = math::Rational{math::Numerator{3}, math::Denominator{2}, math::DecimalPlaces{0}},
              .unit = "g_per_l"}});
    CHECK(run(R"({"fn": "neg", "args": [5]})") == ui::Value{std::int64_t{-5}});
    std::string const max = std::to_string(kMax);
    CHECK(reportOf(R"({"fn": "add", "args": [)" + max + ", 1]}") == "overflow");
    CHECK(reportOf(R"({"fn": "sub", "args": [-)" + max + ", 2]}") == "overflow");
    CHECK(reportOf(R"({"fn": "mul", "args": [)" + max + ", 2]}") == "overflow");
    CHECK(reportOf(R"({"fn": "neg", "args": [{"fn": "sub", "args": [-)" + max + ", 1]}]})") == "overflow");
    CHECK(reportOf(R"({"fn": "div", "args": [)" + dec(1, 2, 1) + ", 0]}") == "division by zero");
    CHECK(reportOf(R"({"fn": "mul", "args": [)" + qty(1, 0, "g_per_l") + ", " + qty(1, 0, "g_per_l") + "]}",
                   context) == "`mul` takes at most one quantity");
    CHECK(reportOf(R"({"fn": "div", "args": [1, )" + qty(1, 0, "g_per_l") + "]}", context) ==
          "`div` divides a quantity by a number, not a number by a quantity");
}

// Mutations: round half away from zero; take `mean` through a double.
TEST_CASE("ui::evaluate: round and mean are exact and round half to even", "[ui][expr]") {
    CHECK(run(R"({"fn": "round", "args": [)" + dec(5, 2, 1) + ", 0]}") == decimal(2, 1, 0));
    CHECK(run(R"({"fn": "round", "args": [)" + dec(7, 2, 1) + ", 0]}") == decimal(4, 1, 0));
    CHECK(run(R"({"fn": "round", "args": [)" + dec(-5, 2, 1) + ", 0]}") == decimal(-2, 1, 0));
    CHECK(run(R"({"fn": "round", "args": [)" + dec(2675, 1000, 3) + ", 2]}") == decimal(268, 100, 2));
    // (2.67 + 2.68) / 2 is the tie 2.675, which half-to-even rounds up; as a double it is 2.67499..., which does not.
    CHECK(run(R"({"fn": "mean", "args": [{"list": [)" + dec(267, 100, 2) + ", " + dec(268, 100, 2) + "]}]}") ==
          decimal(268, 100, 2));
    CHECK(run(R"({"fn": "mean", "args": [{"list": [1, 2]}]})") == decimal(2, 1, 0));
    CHECK(run(R"({"fn": "mean", "args": [{"list": []}]})").isNull());
    CHECK(run(R"({"fn": "sum", "args": [{"list": [1, 2, )" + dec(1, 2, 1) + "]}]}") == decimal(7, 2, 1));
    CHECK(run(R"({"fn": "sum", "args": [{"list": []}]})") == ui::Value{std::int64_t{0}});
    CHECK(reportOf(R"({"fn": "round", "args": [1, 19]})") ==
          "`round` takes a number or a quantity, and places from 0 to 18");
    CHECK(reportOf(R"({"fn": "round", "args": [)" + std::to_string(std::numeric_limits<std::int64_t>::max()) +
                   ", 2]}") == "overflow");
}

// Mutation: count bytes instead of code points in `length`.
TEST_CASE("ui::evaluate: string and list functions", "[ui][expr]") {
    CHECK(run(R"({"fn": "concat", "args": ["a", "b", "c"]})") == ui::Value{"abc"});
    CHECK(run(R"({"fn": "format", "args": ["{1} of {0} {{done}}", 3, "two"]})") == ui::Value{"two of 3 {done}"});
    CHECK(run(R"({"fn": "format", "args": ["{0} at {1}", )" + dec(5, 4, 2) + R"(, true]})") ==
          ui::Value{"1.25 at true"});
    CHECK(reportOf(R"({"fn": "format", "args": ["{2}", 1]})") == "`format` has no argument {2}");
    CHECK(reportOf(R"({"fn": "format", "args": ["{x}", 1]})") == "`format`'s placeholders are written {0}, {1}, ...");
    CHECK(reportOf(R"({"fn": "format", "args": ["a } b"]})") == "`format`'s template has an unmatched `}`");
    CHECK(run(R"({"fn": "trim", "args": ["  padded\t "]})") == ui::Value{"padded"});
    CHECK(run(R"({"fn": "length", "args": ["Zoë"]})") == ui::Value{std::int64_t{3}});
    CHECK(run(R"({"fn": "isEmpty", "args": [""]})") == ui::Value{true});
    CHECK(run(R"({"fn": "isEmpty", "args": [{"list": [1]}]})") == ui::Value{false});
    CHECK(run(R"({"fn": "contains", "args": ["sample-7", "le-"]})") == ui::Value{true});
    CHECK(reportOf(R"({"fn": "trim", "args": [1]})") == "`trim` takes a string");
    ui::Value const numbers{ui::ValueList{std::int64_t{4}, std::int64_t{1}, std::int64_t{3}}};
    FakeContext context;
    context.names["xs"] = numbers;
    context.table = concentrations();
    CHECK(run(R"({"fn": "count", "args": [{"ref": "xs"}]})", context) == ui::Value{std::int64_t{3}});
    CHECK(run(R"({"fn": "min", "args": [{"ref": "xs"}]})", context) == ui::Value{std::int64_t{1}});
    CHECK(run(R"({"fn": "max", "args": [{"ref": "xs"}]})", context) == ui::Value{std::int64_t{4}});
    CHECK(run(R"({"fn": "max", "args": [{"list": [)" + qty(900, 0, "mg_per_l") + ", " + qty(1, 0, "g_per_l") + "]}]}",
              context) ==
          ui::Value{ui::QuantityValue{.amount = math::Rational{1, math::DecimalPlaces{0}}, .unit = "g_per_l"}});
    CHECK(run(R"({"fn": "first", "args": [{"ref": "xs"}]})", context) == ui::Value{std::int64_t{4}});
    CHECK(run(R"({"fn": "first", "args": [{"list": []}]})").isNull());
    CHECK(run(R"({"fn": "includes", "args": [{"ref": "xs"}, )" + dec(30, 10, 1) + "]}", context) == ui::Value{true});
    CHECK(run(R"({"fn": "indexOf", "args": [{"ref": "xs"}, 3]})", context) == ui::Value{std::int64_t{2}});
    CHECK(run(R"({"fn": "indexOf", "args": [{"ref": "xs"}, 9]})", context).isNull());
    CHECK(reportOf(R"({"fn": "count", "args": ["abc"]})") == "`count` takes a list");
}

// Mutations: ignore the display offset; ignore the unit conversion in `quantity`.
TEST_CASE("ui::evaluate: display functions use the locale, the unit table and the display zone", "[ui][expr]") {
    FakeContext context;
    context.table = concentrations();
    context.numeric = morph::render::NumericLocale{.decimalSeparator = ",", .groupSeparator = "."};
    context.offset = std::chrono::minutes{120};
    CHECK(run(R"({"fn": "number", "args": [)" + dec(123'456, 100, 2) + "]}", context) == ui::Value{"1.234,56"});
    CHECK(run(R"({"fn": "decimal", "args": [)" + dec(5, 2, 1) + ", 3]}", context) == ui::Value{"2,500"});
    CHECK(run(R"({"fn": "money", "args": [)" + dec(123'450, 100, 2) + R"(, "EUR"]})", context) ==
          ui::Value{"1.234,50 EUR"});
    CHECK(run(R"({"fn": "quantity", "args": [)" + qty(2, 1, "g_per_l") + "]}", context) == ui::Value{"2,0 g/L"});
    CHECK(run(R"({"fn": "quantity", "args": [)" + qty(2, 1, "g_per_l") + R"(, "mg_per_l"]})", context) ==
          ui::Value{"2.000,0 mg/L"});
    std::string const instant = R"({"lit": "2026-10-11T23:30:15Z"})";
    ui::Expr const asTimestamp = expr(R"({"fn": "date", "args": [{"ref": "at"}]})");
    context.names["at"] = ui::Value{*morph::time::DateTime::fromIso8601("2026-10-11T23:30:15Z")};
    ui::Diagnostics diagnostics;
    CHECK(ui::evaluate(asTimestamp, context, diagnostics) == ui::Value{"2026-10-12"});
    CHECK(run(R"({"fn": "dateTime", "args": [{"ref": "at"}, "short"]})", context) == ui::Value{"2026-10-12"});
    CHECK(run(R"({"fn": "dateTime", "args": [{"ref": "at"}]})", context) == ui::Value{"2026-10-12 01:30"});
    CHECK(run(R"({"fn": "dateTime", "args": [{"ref": "at"}, "long"]})", context) == ui::Value{"2026-10-12 01:30:15"});
    CHECK(reportOf(R"({"fn": "dateTime", "args": [{"ref": "at"}, "full"]})", context) ==
          "a style is `short`, `medium` or `long`");
    CHECK(reportOf(R"({"fn": "date", "args": [)" + instant + "]}", context) == "`date` takes a timestamp and a style");
    CHECK(reportOf(R"({"fn": "money", "args": [)" + qty(1, 0, "g_per_l") + R"(, "EUR"]})", context) ==
          "`money` takes a number and a currency code");
}

// Mutations: show the fallback when the catalogue has the key; skip the report of a refused catalogue entry.
TEST_CASE("ui::evaluate: t reads the catalogue for the locale and falls back to its own text", "[ui][expr]") {
    FakeContext context;
    context.activeLocale = "pl-PL";
    context.catalogue = [](std::string_view key, std::string_view locale) -> std::optional<std::string> {
        if (locale == "pl-PL" && key == "samples") {
            return ".input {$n :number} .match $n one {{{$n} próbka}} few {{{$n} próbki}} * {{{$n} próbek}}";
        }
        if (key == "broken") {
            return "{$n :datetime}";
        }
        return std::nullopt;
    };
    std::string_view const english = R"(".input {$n :number} .match $n one {{{$n} sample}} * {{{$n} samples}}")";
    auto const samples = [&](int n) {
        return R"({"t": "samples", "fallback": )" + std::string{english} + R"(, "args": {"n": )" + std::to_string(n) +
               "}}";
    };
    CHECK(run(samples(1), context) == ui::Value{"1 próbka"});
    CHECK(run(samples(3), context) == ui::Value{"3 próbki"});
    CHECK(run(samples(5), context) == ui::Value{"5 próbek"});
    context.activeLocale = "en";
    CHECK(run(samples(1), context) == ui::Value{"1 sample"});
    CHECK(run(R"({"fn": "t", "args": ["missing", "Hello {$who}", {"obj": {"who": "Ada"}}]})", context) ==
          ui::Value{"Hello Ada"});
    ui::Diagnostics diagnostics;
    CHECK(run(R"({"t": "broken", "fallback": "{$n} items", "args": {"n": 2}})", context, diagnostics) ==
          ui::Value{"2 items"});
    REQUIRE(diagnostics.reported().size() == 1);
    CHECK(diagnostics.reported().front().reason ==
          "the catalogue's `broken` is refused: only `:number` and `:string` are supported at offset 4");
    CHECK(reportOf(R"({"t": "missing", "fallback": "{$n} items"})", context) == "no argument `n`");
}
