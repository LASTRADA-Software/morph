// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <morph/forms/engine/rules.hpp>
#include <morph/forms/forms.hpp>
#include <morph/ui/value.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using morph::forms::RuleExpr;
using morph::forms::RuleLookup;
using morph::forms::RuleOperand;
using morph::forms::Tri;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
namespace engine = morph::forms::detail::engine;
namespace ui = morph::ui;

[[nodiscard]] RuleExpr rule(std::string_view json) {
    auto const dom = engine::parseJson(json);
    REQUIRE(dom.has_value());
    auto parsed = RuleExpr::parse(*dom, "x-rules[0]");
    INFO((parsed ? std::string{} : parsed.error().message));
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

[[nodiscard]] RuleOperand valid(ui::Value value) {
    return RuleOperand{.state = RuleOperand::State::Valid, .value = std::move(value)};
}
[[nodiscard]] RuleOperand invalid() { return RuleOperand{.state = RuleOperand::State::Invalid, .value = {}}; }
[[nodiscard]] ui::Value decimal(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return ui::Decimal{Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}}};
}

/// The fields a rule sees; an absent name is blank.
struct Fields {
    std::map<std::string, RuleOperand, std::less<>> values;

    [[nodiscard]] RuleLookup lookup() const {
        return [this](std::string_view name) {
            auto const found = values.find(name);
            return found == values.end() ? RuleOperand{} : found->second;
        };
    }
};

}  // namespace

TEST_CASE("RuleExpr: every kind the emitter writes parses to a known kind", "[forms-engine][rules]") {
    for (std::uint8_t ordinal = 0; ordinal < 16; ++ordinal) {
        auto const name = morph::forms::detail::ruleKindName(static_cast<morph::forms::detail::RuleKind>(ordinal));
        REQUIRE_FALSE(name.empty());
        std::string json = R"({"kind":")" + std::string{name} + R"(","fields":["a","b"],)";
        json += R"("when":{"kind":"engaged","fields":["a"]},"condition":{"kind":"engaged","fields":["a"]},)";
        json += R"("conditions":[{"kind":"engaged","fields":["a"]}],"value":1})";
        RuleExpr const parsed = rule(json);
        INFO(name);
        CHECK(parsed.kind() != RuleExpr::Kind::Unknown);
        CHECK(parsed.kindName() == name);
    }
}

TEST_CASE("RuleExpr: a malformed rule is a SchemaError naming where it is", "[forms-engine][rules]") {
    auto const reject = [](std::string_view json) {
        auto const dom = engine::parseJson(json);
        REQUIRE(dom.has_value());
        return RuleExpr::parse(*dom, "x-rules[3]");
    };
    auto const noKind = reject(R"({"fields":["a"]})");
    REQUIRE_FALSE(noKind.has_value());
    CHECK(noKind.error().path == "x-rules[3]");
    CHECK_FALSE(reject(R"({"kind":"engaged","fields":"a"})").has_value());
    CHECK_FALSE(reject(R"({"kind":"requiredWhen","fields":["a"]})").has_value());
    CHECK_FALSE(reject(R"({"kind":"greater","fields":["a"]})").has_value());
    auto const nested = reject(R"({"kind":"and","conditions":[{"kind":"engaged"}]})");
    REQUIRE_FALSE(nested.has_value());
    CHECK(nested.error().path == "x-rules[3].conditions[0]");
    CHECK(reject(R"({"kind":"quantumEntangled"})").has_value());  // unknown is not malformed
}

TEST_CASE("RuleExpr: a rule translates into expr/1 over numbered operands", "[forms-engine][rules]") {
    RuleExpr const greater = rule(R"({"kind":"greater","fields":["high","low"]})");
    CHECK(greater.operands().size() == 2);
    CHECK(greater.operands().front() == "high");
    CHECK(engine::toJson(greater.expression()) ==
          R"({"if":[{"fn":"and","args":[{"ref":"f0.engaged"},{"ref":"f1.engaged"}]},)"
          R"({"fn":"gt","args":[{"ref":"f0.value"},{"ref":"f1.value"}]},true]})");
    // The translation is an expression the UI document's parser accepts: it is what the evaluator runs.
    CHECK(ui::parseExpression(greater.expression()).has_value());
    RuleExpr const member = rule(R"({"kind":"atLeastOneOf","fields":["a","b","a"]})");
    CHECK(member.operands().size() == 2);  // a field named twice is one operand
}

TEST_CASE("RuleExpr: and, or and not are Kleene over Unknown", "[forms-engine][rules]") {
    Fields const fields{{{"yes", valid(ui::Value{"x"})}, {"bad", invalid()}}};
    auto const lookup = fields.lookup();
    auto const verdict = [&](std::string_view json) { return rule(json).evaluate(lookup); };
    CHECK(
        verdict(
            R"({"kind":"and","conditions":[{"kind":"engaged","fields":["no"]},{"kind":"engaged","fields":["bad"]}]})") ==
        Tri::False);
    CHECK(
        verdict(
            R"({"kind":"and","conditions":[{"kind":"engaged","fields":["yes"]},{"kind":"engaged","fields":["bad"]}]})") ==
        Tri::Unknown);
    CHECK(
        verdict(
            R"({"kind":"or","conditions":[{"kind":"engaged","fields":["yes"]},{"kind":"engaged","fields":["bad"]}]})") ==
        Tri::True);
    CHECK(
        verdict(
            R"({"kind":"or","conditions":[{"kind":"engaged","fields":["no"]},{"kind":"engaged","fields":["bad"]}]})") ==
        Tri::Unknown);
    CHECK(verdict(R"({"kind":"not","condition":{"kind":"engaged","fields":["bad"]}})") == Tri::Unknown);
    CHECK(verdict(R"({"kind":"not","condition":{"kind":"engaged","fields":["no"]}})") == Tri::True);
}

TEST_CASE("RuleExpr: an unknown kind never blocks, and a false conjunct still does", "[forms-engine][rules]") {
    Fields fields{};
    auto const lookup = fields.lookup();
    std::vector<RuleExpr> const unknownTop{rule(R"({"kind":"quantumEntangled","fields":["email","note"]})")};
    CHECK(morph::forms::gatingVerdict(unknownTop, lookup) == Tri::Unknown);

    std::vector<RuleExpr> const insideNot{rule(
        R"({"kind":"requiredWhen","fields":["note"],"when":{"kind":"not","condition":{"kind":"quantumEntangled","fields":["email"]}}})")};
    CHECK(morph::forms::gatingVerdict(insideNot, lookup) == Tri::True);
    CHECK(morph::forms::requiredVerdict(insideNot, "note", lookup) == Tri::Unknown);

    std::vector<RuleExpr> const falseConjunct{rule(
        R"({"kind":"and","conditions":[{"kind":"engaged","fields":["email"]},{"kind":"quantumEntangled","fields":["note"]}]})")};
    CHECK(morph::forms::gatingVerdict(falseConjunct, lookup) == Tri::False);
    fields.values.emplace("email", valid(ui::Value{"a@b"}));
    CHECK(morph::forms::gatingVerdict(falseConjunct, lookup) == Tri::Unknown);

    std::vector<RuleExpr> const presentation{
        rule(R"({"kind":"visibleWhen","fields":["note"],"when":{"kind":"quantumEntangled","fields":["email"]}})"),
        rule(R"({"kind":"readonlyWhen","fields":["note"],"when":{"kind":"quantumEntangled","fields":["email"]}})")};
    CHECK(morph::forms::visibleVerdict(presentation, "note", lookup) == Tri::Unknown);   // presents as visible
    CHECK(morph::forms::readonlyVerdict(presentation, "note", lookup) == Tri::Unknown);  // presents as editable
    CHECK(morph::forms::gatingVerdict(presentation, lookup) == Tri::True);
}

TEST_CASE("RuleExpr: comparisons are vacuous on a blank operand and exact on valid ones", "[forms-engine][rules]") {
    Fields fields{};
    RuleExpr const greater = rule(R"({"kind":"greater","fields":["high","low"]})");
    CHECK(greater.evaluate(fields.lookup()) == Tri::True);
    fields.values["high"] = valid(decimal(2, 1, 2));
    CHECK(greater.evaluate(fields.lookup()) == Tri::True);
    fields.values["low"] = valid(decimal(200, 100, 2));
    CHECK(greater.evaluate(fields.lookup()) == Tri::False);
    fields.values["low"] = valid(decimal(199, 100, 2));
    CHECK(greater.evaluate(fields.lookup()) == Tri::True);
    fields.values["low"] = invalid();
    CHECK(greater.evaluate(fields.lookup()) == Tri::Unknown);
    fields.values["low"] = valid(ui::Value{"text"});
    CHECK(greater.evaluate(fields.lookup()) == Tri::Unknown);  // a number and a string do not order
    // Integers past 2^53 differ in their last digit: a double would call them equal.
    fields.values["high"] = valid(ui::Value{std::int64_t{9007199254740993}});
    fields.values["low"] = valid(ui::Value{std::int64_t{9007199254740992}});
    CHECK(greater.evaluate(fields.lookup()) == Tri::True);
    // An int and a decimal compare by value.
    fields.values["low"] = valid(decimal(18014398509481985, 2, 1));
    CHECK(greater.evaluate(fields.lookup()) == Tri::True);
}

TEST_CASE("RuleExpr: equals compares booleans as booleans and big integers on digits", "[forms-engine][rules]") {
    Fields fields{};
    RuleExpr const flag = rule(R"({"kind":"equals","fields":["flag"],"value":true})");
    CHECK(flag.evaluate(fields.lookup()) == Tri::False);
    fields.values["flag"] = valid(ui::Value{true});
    CHECK(flag.evaluate(fields.lookup()) == Tri::True);
    fields.values["flag"] = valid(ui::Value{"true"});
    CHECK(flag.evaluate(fields.lookup()) == Tri::False);  // a string is not the boolean
    fields.values["flag"] = invalid();
    CHECK(flag.evaluate(fields.lookup()) == Tri::Unknown);

    RuleExpr const big =
        rule(R"({"kind":"equals","fields":["id"],"value":9007199254740992,"valueText":"9007199254740993"})");
    fields.values["id"] = valid(ui::Value{std::int64_t{9007199254740992}});
    CHECK(big.evaluate(fields.lookup()) == Tri::False);  // valueText wins over the rounded value
    fields.values["id"] = valid(ui::Value{std::int64_t{9007199254740993}});
    CHECK(big.evaluate(fields.lookup()) == Tri::True);

    RuleExpr const exactValue = rule(R"({"kind":"equals","fields":["amount"],"value":{"num":5,"den":2,"dp":1}})");
    fields.values["amount"] = valid(decimal(250, 100, 2));
    CHECK(exactValue.evaluate(fields.lookup()) == Tri::True);
    RuleExpr const fraction = rule(R"({"kind":"equals","fields":["amount"],"value":2.5})");
    CHECK(fraction.evaluate(fields.lookup()) == Tri::True);
}

TEST_CASE("RuleExpr: membership counts definite operands and is Unknown only when the count is open",
          "[forms-engine][rules]") {
    Fields fields{};
    RuleExpr const one = rule(R"({"kind":"exactlyOneOf","fields":["a","b","c"]})");
    RuleExpr const least = rule(R"({"kind":"atLeastOneOf","fields":["a","b","c"]})");
    RuleExpr const most = rule(R"({"kind":"mutuallyExclusive","fields":["a","b","c"]})");
    CHECK(one.evaluate(fields.lookup()) == Tri::False);
    CHECK(least.evaluate(fields.lookup()) == Tri::False);
    CHECK(most.evaluate(fields.lookup()) == Tri::True);
    fields.values["a"] = invalid();
    CHECK(one.evaluate(fields.lookup()) == Tri::Unknown);
    CHECK(least.evaluate(fields.lookup()) == Tri::Unknown);
    CHECK(most.evaluate(fields.lookup()) == Tri::True);
    fields.values["b"] = valid(ui::Value{"x"});
    CHECK(one.evaluate(fields.lookup()) == Tri::Unknown);
    CHECK(least.evaluate(fields.lookup()) == Tri::True);
    CHECK(most.evaluate(fields.lookup()) == Tri::Unknown);
    fields.values["c"] = valid(ui::Value{"y"});
    CHECK(one.evaluate(fields.lookup()) == Tri::False);
    CHECK(least.evaluate(fields.lookup()) == Tri::True);
    CHECK(most.evaluate(fields.lookup()) == Tri::False);
    fields.values.erase("a");
    fields.values.erase("c");
    CHECK(one.evaluate(fields.lookup()) == Tri::True);
    CHECK(most.evaluate(fields.lookup()) == Tri::True);
}

TEST_CASE("RuleExpr: presentation verdicts combine every rule naming the field", "[forms-engine][rules]") {
    Fields fields{};
    std::vector<RuleExpr> const rules{
        rule(R"({"kind":"visibleWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}})"),
        rule(R"({"kind":"requiredWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}})"),
        rule(R"({"kind":"readonlyWhen","fields":["total"],"when":{"kind":"engaged","fields":["promo"]}})")};
    CHECK(morph::forms::visibleVerdict(rules, "discount", fields.lookup()) == Tri::False);
    CHECK(morph::forms::visibleVerdict(rules, "total", fields.lookup()) == Tri::True);
    CHECK(morph::forms::requiredVerdict(rules, "discount", fields.lookup()) == Tri::False);
    CHECK(morph::forms::readonlyVerdict(rules, "total", fields.lookup()) == Tri::False);
    fields.values["promo"] = valid(ui::Value{std::int64_t{5}});
    CHECK(morph::forms::visibleVerdict(rules, "discount", fields.lookup()) == Tri::True);
    CHECK(morph::forms::requiredVerdict(rules, "discount", fields.lookup()) == Tri::True);
    CHECK(morph::forms::readonlyVerdict(rules, "total", fields.lookup()) == Tri::True);
    CHECK(morph::forms::gatingVerdict(rules, fields.lookup()) == Tri::False);  // discount now required, blank
    // A field no rule of the kind names is shown, editable and not required by rules.
    CHECK(morph::forms::readonlyVerdict(rules, "discount", fields.lookup()) == Tri::False);
    CHECK(morph::forms::requiredVerdict(rules, "total", fields.lookup()) == Tri::False);
    CHECK(morph::forms::visibleVerdict(rules, "elsewhere", fields.lookup()) == Tri::True);
}

TEST_CASE("RuleExpr: a presentation rule applies to every field it names, not only the first",
          "[forms-engine][rules]") {
    Fields fields{};
    std::vector<RuleExpr> const rules{
        rule(R"({"kind":"visibleWhen","fields":["a","b"],"when":{"kind":"engaged","fields":["flag"]}})"),
        rule(R"({"kind":"requiredWhen","fields":["c","d"],"when":{"kind":"engaged","fields":["flag"]}})")};
    CHECK(morph::forms::visibleVerdict(rules, "b", fields.lookup()) == Tri::False);
    fields.values["flag"] = valid(ui::Value{true});
    CHECK(morph::forms::requiredVerdict(rules, "d", fields.lookup()) == Tri::True);
    fields.values["c"] = valid(ui::Value{"x"});
    CHECK(morph::forms::gatingVerdict(rules, fields.lookup()) == Tri::False);  // d is required too, and blank
    fields.values["d"] = valid(ui::Value{"y"});
    CHECK(morph::forms::gatingVerdict(rules, fields.lookup()) == Tri::True);
}
