// SPDX-License-Identifier: Apache-2.0

// Rules read for what they hold: the `when` clause and the conditions a kind has, the verdict of a rule whose
// operands cannot be compared, and the literals that no exact value spells.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <morph/forms/engine/rules.hpp>
#include <morph/forms/forms.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using morph::forms::RuleExpr;
using morph::forms::RuleLookup;
using morph::forms::RuleOperand;
using morph::forms::Tri;
namespace engine = morph::forms::detail::engine;
namespace ui = morph::ui;

RuleExpr ruleOf(std::string_view json) {
    auto const dom = engine::parseJson(json);
    REQUIRE(dom.has_value());
    auto parsed = RuleExpr::parse(*dom, "x-rules[0]");
    INFO((parsed ? std::string{} : parsed.error().message));
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

RuleOperand validOf(ui::Value value) {
    return RuleOperand{.state = RuleOperand::State::Valid, .value = std::move(value)};
}

}  // namespace

TEST_CASE("cov h rules: a rule's when clause and conditions are those of its kind", "[covh][forms-engine][rules]") {
    struct Case {
        char const* json;
        bool hasWhen;
        std::size_t conditions;
        bool gates;
    };
    constexpr char const* kEngaged = R"({"kind":"engaged","fields":["a"]})";
    std::string const when = std::string{R"("when":)"} + kEngaged;
    std::vector<std::pair<std::string, Case>> const cases{
        {R"({"kind":"requiredWhen","fields":["a"],)" + when + "}",
         {.json = "", .hasWhen = true, .conditions = 0, .gates = true}},
        {R"({"kind":"visibleWhen","fields":["a"],)" + when + "}",
         {.json = "", .hasWhen = true, .conditions = 0, .gates = false}},
        {R"({"kind":"readonlyWhen","fields":["a"],)" + when + "}",
         {.json = "", .hasWhen = true, .conditions = 0, .gates = false}},
        {R"({"kind":"engaged","fields":["a"]})", {.json = "", .hasWhen = false, .conditions = 0, .gates = true}},
        {R"({"kind":"exactlyOneOf","fields":["a","b"]})",
         {.json = "", .hasWhen = false, .conditions = 0, .gates = true}},
        {R"({"kind":"not","condition":)" + std::string{kEngaged} + "}",
         {.json = "", .hasWhen = false, .conditions = 1, .gates = true}},
        {R"({"kind":"or","conditions":[)" + std::string{kEngaged} + "," + kEngaged + "]}",
         {.json = "", .hasWhen = false, .conditions = 2, .gates = true}},
        {R"({"kind":"and","conditions":[]})", {.json = "", .hasWhen = false, .conditions = 0, .gates = true}},
        {R"({"kind":"quantumEntangled","fields":["a"]})",
         {.json = "", .hasWhen = false, .conditions = 0, .gates = true}},
    };
    for (auto const& [json, item] : cases) {
        INFO(json);
        RuleExpr const rule = ruleOf(json);
        CHECK((rule.when() != nullptr) == item.hasWhen);
        CHECK(rule.conditions().size() == item.conditions);
        CHECK(rule.gates() == item.gates);
        CHECK(rule.names("a") == (rule.fields().size() > 0));
        CHECK_FALSE(rule.names("z"));
    }
    RuleExpr const visible = ruleOf(R"({"kind":"visibleWhen","fields":["a"],)" + when + "}");
    CHECK(visible.presents(RuleExpr::Kind::VisibleWhen, "a"));
    CHECK_FALSE(visible.presents(RuleExpr::Kind::ReadonlyWhen, "a"));
    CHECK_FALSE(visible.presents(RuleExpr::Kind::VisibleWhen, "z"));
}

TEST_CASE("cov h rules: the negation of an undecided verdict is undecided", "[covh][forms-engine][rules]") {
    CHECK(morph::forms::triNot(Tri::Unknown) == Tri::Unknown);
    CHECK(morph::forms::triNot(Tri::True) == Tri::False);
    CHECK(morph::forms::triNot(Tri::False) == Tri::True);
}

TEST_CASE(
    "cov h rules: a condition no rule translation holds is undecided, and a literal no value spells is undecided",
    "[covh][forms-engine][rules]") {
    struct Case {
        char const* what;
        char const* json;
        ui::Value operand;
        Tri expected;
    };
    std::map<std::string, RuleOperand, std::less<>> values;
    RuleLookup const lookup = [&values](std::string_view name) {
        auto const found = values.find(name);
        return found == values.end() ? RuleOperand{} : found->second;
    };
    std::vector<Case> const cases{
        {"a presentation rule as a condition",
         R"({"kind":"or","conditions":[{"kind":"visibleWhen","fields":["a"],"when":{"kind":"engaged","fields":["a"]}}]})",
         ui::Value{std::int64_t{1}}, Tri::Unknown},
        {"a required-when rule as a condition",
         R"({"kind":"not","condition":{"kind":"requiredWhen","fields":["a"],"when":{"kind":"engaged","fields":["a"]}}})",
         ui::Value{std::int64_t{1}}, Tri::Unknown},
        {"digits past a decimal's range", R"({"kind":"equals","fields":["a"],"valueText":"99999999999999999999"})",
         ui::Value{std::int64_t{1}}, Tri::Unknown},
        {"an unsigned beyond int64", R"({"kind":"equals","fields":["a"],"value":18446744073709551615})",
         ui::Value{std::int64_t{1}}, Tri::Unknown},
        {"an unsigned within int64", R"({"kind":"equals","fields":["a"],"value":7})", ui::Value{std::int64_t{7}},
         Tri::True},
        {"an object that is no decimal", R"({"kind":"equals","fields":["a"],"value":{"x":1}})",
         ui::Value{std::int64_t{1}}, Tri::Unknown},
        {"an array", R"({"kind":"equals","fields":["a"],"value":[1]})", ui::Value{std::int64_t{1}}, Tri::Unknown},
        {"no value", R"({"kind":"equals","fields":["a"]})", ui::Value{std::int64_t{1}}, Tri::Unknown},
        {"a string", R"({"kind":"equals","fields":["a"],"value":"x"})", ui::Value{std::string{"x"}}, Tri::True},
    };
    for (auto const& item : cases) {
        INFO(item.what);
        values["a"] = validOf(item.operand);
        CHECK(ruleOf(item.json).evaluate(lookup) == item.expected);
    }
}
