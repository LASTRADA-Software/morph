// SPDX-License-Identifier: Apache-2.0
//
// The compiled half of the x-rules agreement check (morph#176).
//
// `x-rules` is evaluated twice — here by `morph::forms::allRulesSatisfied`, and
// again in JavaScript by `src/qt/forms/qml/DynamicForm.qml` — and nothing pinned
// the two to each other. Two divergences were reachable through rule/literal
// combinations the spec explicitly sanctions:
//
//   (a) `equals` against a `bool` literal — the renderer compared the field as
//       text, so `"true" === true` was false and the rule never fired on the
//       client while firing here. The client submitted a body the server
//       rejects.
//   (b) `equals` against an `int64` literal beyond 2^53 — the renderer's
//       JSON.parse rounded the literal, collapsing values this evaluator keeps
//       distinct. The client blocked a submission the server would accept.
//
// This file pins the compiled verdicts for one action and four field states;
// src/qt/forms/tests/tst_DynamicFormRuleAgreement.qml drives the *verbatim*
// schemaJson output of this same action through the renderer and asserts the
// matching verdict. Neither file is meaningful alone — the point is that the
// two agree, so they are written against the same action and the same states
// deliberately.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <optional>
#include <string>

// File-scope, not anonymous-namespaced: glaze's reflection needs linkage.
// NOLINTBEGIN(misc-use-internal-linkage)
// NOLINTBEGIN(cert-err58-cpp,bugprone-throwing-static-initialization,misc-const-correctness,bugprone-exception-escape)
struct RuleAgreementAction {
    std::optional<bool> flag;
    std::optional<std::int64_t> id;
    std::optional<std::string> reason;

    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::requiredWhen(&RuleAgreementAction::reason,
                                   morph::forms::equals(&RuleAgreementAction::flag, true)),
        morph::forms::requiredWhen(&RuleAgreementAction::reason,
                                   morph::forms::equals(&RuleAgreementAction::id, std::int64_t{9007199254740993})));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

struct RuleAgreementModel {
    int seen = 0;

    bool execute(const RuleAgreementAction& action) {
        seen += action.flag.has_value() ? 1 : 0;
        return true;
    }
};

BRIDGE_REGISTER_MODEL(RuleAgreementModel, "Test_RuleAgreement_Model")
BRIDGE_REGISTER_ACTION(RuleAgreementModel, RuleAgreementAction, "Test_RuleAgreement")
// NOLINTEND(cert-err58-cpp,bugprone-throwing-static-initialization,misc-const-correctness,bugprone-exception-escape)
// NOLINTEND(misc-use-internal-linkage)

TEST_CASE("equals against a bool literal fires requiredWhen", "[forms][rules][agreement]") {
    RuleAgreementAction action{};
    action.flag = true;
    // Condition holds and `reason` is unset, so the rule is unsatisfied. The
    // QML case of the same name asserts the renderer reports ready == false.
    CHECK_FALSE(morph::forms::allRulesSatisfied(action));

    action.reason = "because";
    CHECK(morph::forms::allRulesSatisfied(action));
}

TEST_CASE("equals against a bool literal does not fire when false", "[forms][rules][agreement]") {
    RuleAgreementAction action{};
    action.flag = false;
    CHECK(morph::forms::allRulesSatisfied(action));
}

TEST_CASE("equals against an int64 literal does not fire one below it", "[forms][rules][agreement]") {
    RuleAgreementAction action{};
    // One below the literal. These differ by 1 and both round to the same
    // double, which is exactly what made the renderer disagree here.
    action.id = 9007199254740992;
    CHECK(morph::forms::allRulesSatisfied(action));
}

TEST_CASE("equals against an int64 literal fires on the exact literal", "[forms][rules][agreement]") {
    RuleAgreementAction action{};
    action.id = 9007199254740993;
    CHECK_FALSE(morph::forms::allRulesSatisfied(action));

    action.reason = "because";
    CHECK(morph::forms::allRulesSatisfied(action));
}

TEST_CASE("the emitted schema carries what the renderer needs to agree", "[forms][rules][agreement]") {
    auto const schema = morph::forms::schemaJson<RuleAgreementAction>();
    // The bool literal is emitted as a JSON boolean, not a string — the
    // renderer must compare it as one.
    CHECK(schema.contains(R"("value":true)"));
    // The int64 literal beyond 2^53 carries exact digits alongside the number,
    // because the number itself does not survive JSON.parse.
    CHECK(schema.contains(R"("valueText":"9007199254740993")"));
}
