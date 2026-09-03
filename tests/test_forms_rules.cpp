// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "test_support.hpp"

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

// ---------------------------------------------------------------------------
// A miniature unit system for the cross-field-rules tests.
// ---------------------------------------------------------------------------

enum class CFRUnit : std::uint8_t { money };

template <>
struct morph::units::UnitTraits<CFRUnit> {
    static constexpr morph::units::UnitMeta meta(CFRUnit /*unit*/) noexcept {
        return {.id = "money", .display = "$", .defaultDecimals = 2};
    }
};

using CFRMoney = morph::units::Quantity<CFRUnit::money>;

// ---------------------------------------------------------------------------
// requiredWhen + engaged/notEngaged: "discount required only once promo is set".
// ---------------------------------------------------------------------------

struct CFRDiscountForm {
    CFRMoney promo;
    CFRMoney discount;

    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::requiredWhen(&CFRDiscountForm::discount, morph::forms::engaged(&CFRDiscountForm::promo)));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

struct CFRNoRulesAction {
    std::int64_t note = 0;
};

static_assert(morph::forms::HasFormRules<CFRDiscountForm>);
static_assert(!morph::forms::HasFormRules<CFRNoRulesAction>);

TEST_CASE("Forms::Rules::RequiredWhen::NotRequiredUntilConditionHolds", "[forms][rules]") {
    CFRDiscountForm form{};
    CHECK(morph::forms::allRulesSatisfied(form));  // promo unengaged -> discount not required

    form.promo = Rational{5, DecimalPlaces{2}};
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));  // promo engaged, discount still empty

    form.discount = Rational{2, DecimalPlaces{2}};
    CHECK(morph::forms::allRulesSatisfied(form));  // both engaged
}

TEST_CASE("Forms::Rules::RequiredWhen::ValidateIntegratesWithActionValidator", "[forms][rules]") {
    CFRDiscountForm form{};
    CHECK(morph::model::ActionValidator<CFRDiscountForm>::ready(form));
    form.promo = Rational{5, DecimalPlaces{2}};
    CHECK_FALSE(morph::model::ActionValidator<CFRDiscountForm>::ready(form));
}

TEST_CASE("Forms::Rules::NoFormRules::AllRulesSatisfiedIsTriviallyTrue", "[forms][rules]") {
    CFRNoRulesAction action{.note = 7};
    CHECK(morph::forms::allRulesSatisfied(action));
}

TEST_CASE("Forms::Rules::NotEngagedCondition", "[forms][rules]") {
    CFRDiscountForm form{};
    auto const cond = morph::forms::notEngaged(&CFRDiscountForm::promo);
    CHECK(cond.test(form));
    form.promo = Rational{1, DecimalPlaces{2}};
    CHECK_FALSE(cond.test(form));
}

TEST_CASE("Forms::Rules::SchemaJson::RequiredWhenEmitsXRules", "[forms][rules]") {
    auto const schema = morph::forms::schemaJson<CFRDiscountForm>();
    CHECK(schema.contains(
        R"("x-rules":[{"kind":"requiredWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}}])"));
}

TEST_CASE("Forms::Rules::SchemaJson::NoFormRulesEmitsNoXRules", "[forms][rules]") {
    auto const schema = morph::forms::schemaJson<CFRNoRulesAction>();
    CHECK_FALSE(schema.contains("x-rules"));
}

// ---------------------------------------------------------------------------
// Comparisons: greater / greaterOrEqual / less / lessOrEqual.
// ---------------------------------------------------------------------------

struct CFRDateRange {
    morph::time::Timestamp checkIn;
    morph::time::Timestamp checkOut;

    static constexpr auto formRules =
        morph::forms::ruleList(morph::forms::greater(&CFRDateRange::checkOut, &CFRDateRange::checkIn));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

// File-scope, not function-local: a local class cannot have a static data
// member ([class.local]), and `formRules` is exactly that, so this fixture
// (used by the "ComparisonAsCondition" test below) must live here rather
// than inside its TEST_CASE.
struct CFRSurcharge {
    CFRMoney promo;
    CFRMoney discount;

    static constexpr auto formRules = morph::forms::ruleList(morph::forms::requiredWhen(
        &CFRSurcharge::discount, morph::forms::greater(&CFRSurcharge::promo, &CFRSurcharge::discount)));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

TEST_CASE("Forms::Rules::Greater::VacuousWhileEitherOperandUnengaged", "[forms][rules]") {
    CFRDateRange range{};
    CHECK(morph::forms::allRulesSatisfied(range));  // both empty -> vacuously satisfied

    range.checkIn = morph::time::Timestamp::now();
    CHECK(morph::forms::allRulesSatisfied(range));  // checkOut still empty -> vacuously satisfied
}

TEST_CASE("Forms::Rules::Greater::FiresOnceBothEngaged", "[forms][rules]") {
    using morph::time::DateTime;
    using morph::time::Timestamp;

    CFRDateRange range{};
    range.checkIn = Timestamp{DateTime{std::chrono::year{2026}, std::chrono::month{7}, std::chrono::day{20},
                                       std::chrono::hours{0}, std::chrono::minutes{0}, std::chrono::seconds{0}}};
    range.checkOut = Timestamp{DateTime{std::chrono::year{2026}, std::chrono::month{7}, std::chrono::day{10},
                                        std::chrono::hours{0}, std::chrono::minutes{0}, std::chrono::seconds{0}}};
    CHECK_FALSE(morph::forms::allRulesSatisfied(range));  // checkOut before checkIn

    range.checkOut = Timestamp{DateTime{std::chrono::year{2026}, std::chrono::month{7}, std::chrono::day{25},
                                        std::chrono::hours{0}, std::chrono::minutes{0}, std::chrono::seconds{0}}};
    CHECK(morph::forms::allRulesSatisfied(range));  // checkOut after checkIn
}

TEST_CASE("Forms::Rules::SchemaJson::GreaterEmitsXRules", "[forms][rules]") {
    auto const schema = morph::forms::schemaJson<CFRDateRange>();
    CHECK(schema.contains(R"("x-rules":[{"kind":"greater","fields":["checkOut","checkIn"]}])"));
}

TEST_CASE("Forms::Rules::Comparisons::NumericUsesExactRational", "[forms][rules]") {
    // A greater() over Quantity fields compares the exact math::Rational
    // payload (via operator*()), never a lossy double -- values differing
    // only in declared precision/dp still compare exactly.
    CFRMoney const smaller{Rational{Numerator{1}, Denominator{3}, DecimalPlaces{2}}};
    CFRMoney const larger{Rational{Numerator{2}, Denominator{3}, DecimalPlaces{9}}};
    auto const rule = morph::forms::greater(&CFRDiscountForm::discount, &CFRDiscountForm::promo);
    CFRDiscountForm form{.promo = smaller, .discount = larger};
    CHECK(rule.test(form));
    CHECK_FALSE(morph::forms::greater(&CFRDiscountForm::promo, &CFRDiscountForm::discount).test(form));
}

TEST_CASE("Forms::Rules::GreaterOrEqual::LessAndLessOrEqual", "[forms][rules]") {
    CFRDiscountForm form{.promo = Rational{5, DecimalPlaces{2}}, .discount = Rational{5, DecimalPlaces{2}}};
    CHECK(morph::forms::greaterOrEqual(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));
    CHECK_FALSE(morph::forms::greater(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));
    CHECK(morph::forms::lessOrEqual(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));
    CHECK_FALSE(morph::forms::less(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));

    form.discount = Rational{6, DecimalPlaces{2}};
    CHECK(morph::forms::greater(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));
    CHECK_FALSE(morph::forms::less(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));
}

TEST_CASE("Forms::Rules::GreaterOrEqual::Less::LessOrEqual::VacuousWhileEitherOperandUnengaged", "[forms][rules]") {
    // Mirrors Forms::Rules::Greater::VacuousWhileEitherOperandUnengaged, but
    // for the three sibling comparisons: LessAndLessOrEqual above always
    // starts from both operands already engaged, so greaterOrEqual/less/
    // lessOrEqual's own "either operand unengaged -> vacuously true" arm
    // (each's `if (!lv.hasValue() || !rv.hasValue())`) was never exercised.
    CFRDiscountForm form{};
    CHECK(morph::forms::greaterOrEqual(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));
    CHECK(morph::forms::less(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));
    CHECK(morph::forms::lessOrEqual(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));

    // lhs engaged, rhs still unengaged -> still vacuous (the other half of the ||).
    form.discount = Rational{5, DecimalPlaces{2}};
    CHECK(morph::forms::greaterOrEqual(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));
    CHECK(morph::forms::less(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));
    CHECK(morph::forms::lessOrEqual(&CFRDiscountForm::discount, &CFRDiscountForm::promo).test(form));
}

TEST_CASE("Forms::Rules::RequiredWhen::ComparisonAsCondition", "[forms][rules]") {
    // A comparison reused as the `when` clause of requiredWhen (dual-use).
    // With both operands unengaged, greater() is vacuously true (the
    // condition "holds"), so discount becomes required -- and discount
    // itself is unengaged, so the rule fails.
    CFRSurcharge surcharge{};
    CHECK_FALSE(morph::forms::allRulesSatisfied(surcharge));

    surcharge.discount = Rational{1, DecimalPlaces{2}};
    // promo still unengaged, discount now engaged -> requiredWhen trivially satisfied regardless of when().
    CHECK(morph::forms::allRulesSatisfied(surcharge));
}

// ---------------------------------------------------------------------------
// equals condition + membership rules: exactlyOneOf / atLeastOneOf / mutuallyExclusive.
// ---------------------------------------------------------------------------

struct CFRContactForm {
    std::optional<std::string> email;
    std::optional<std::string> phone;

    static constexpr auto formRules =
        morph::forms::ruleList(morph::forms::exactlyOneOf(&CFRContactForm::email, &CFRContactForm::phone));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

// File-scope (not function-local, unlike CFRStatusAction/CFRCodeAction below,
// which never pass through schemaJson<>()/glaze reflection and so carry no
// such risk): schemaJson<A>() drives glaze's own reflection machinery, which
// every other schemaJson<>() call site in this codebase only ever exercises
// against a namespace-scope type.
struct CFREqualsForm {
    CFRMoney promo;
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::requiredWhen(
        &CFREqualsForm::promo, morph::forms::equals(&CFREqualsForm::promo, Rational{5, DecimalPlaces{2}})));
};

TEST_CASE("Forms::Rules::ExactlyOneOf::ZeroOneAndMultipleEngaged", "[forms][rules]") {
    CFRContactForm form{};
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));  // zero engaged

    form.email = "a@b.com";
    CHECK(morph::forms::allRulesSatisfied(form));  // exactly one engaged

    form.phone = "555";
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));  // both engaged
}

TEST_CASE("Forms::Rules::SchemaJson::ExactlyOneOfEmitsXRules", "[forms][rules]") {
    auto const schema = morph::forms::schemaJson<CFRContactForm>();
    CHECK(schema.contains(R"("x-rules":[{"kind":"exactlyOneOf","fields":["email","phone"]}])"));
}

TEST_CASE("Forms::Rules::AtLeastOneOfAndMutuallyExclusive", "[forms][rules]") {
    CFRContactForm form{};
    auto const atLeastOne = morph::forms::atLeastOneOf(&CFRContactForm::email, &CFRContactForm::phone);
    auto const mutex = morph::forms::mutuallyExclusive(&CFRContactForm::email, &CFRContactForm::phone);

    CHECK_FALSE(atLeastOne.test(form));  // zero engaged
    CHECK(mutex.test(form));             // zero engaged -> "at most one" holds

    form.email = "a@b.com";
    CHECK(atLeastOne.test(form));
    CHECK(mutex.test(form));

    form.phone = "555";
    CHECK(atLeastOne.test(form));   // still at least one (now two)
    CHECK_FALSE(mutex.test(form));  // two engaged -> not mutually exclusive
}

TEST_CASE("Forms::Rules::EmitNode::KindStringsNeverExercisedViaSchemaJson", "[forms][rules]") {
    // notEngaged/greaterOrEqual/less/lessOrEqual/atLeastOneOf/mutuallyExclusive
    // all have their test() logic covered above, but none of those fixtures
    // ever runs the rule through schemaJson<A>()/emitNode() -- the only path
    // that calls detail::ruleKindName() with these enumerators -- so each
    // one's "kind" wire string has never actually been produced. Calling
    // emitNode() directly (as the rule/condition objects already do
    // internally) exercises exactly that.
    CHECK(morph::forms::notEngaged(&CFRDiscountForm::promo).emitNode()["kind"].get<std::string>() == "notEngaged");
    CHECK(morph::forms::greaterOrEqual(&CFRDiscountForm::discount, &CFRDiscountForm::promo)
              .emitNode()["kind"]
              .get<std::string>() == "greaterOrEqual");
    CHECK(morph::forms::less(&CFRDiscountForm::discount, &CFRDiscountForm::promo)
              .emitNode()["kind"]
              .get<std::string>() == "less");
    CHECK(morph::forms::lessOrEqual(&CFRDiscountForm::discount, &CFRDiscountForm::promo)
              .emitNode()["kind"]
              .get<std::string>() == "lessOrEqual");
    CHECK(morph::forms::atLeastOneOf(&CFRContactForm::email, &CFRContactForm::phone)
              .emitNode()["kind"]
              .get<std::string>() == "atLeastOneOf");
    CHECK(morph::forms::mutuallyExclusive(&CFRContactForm::email, &CFRContactForm::phone)
              .emitNode()["kind"]
              .get<std::string>() == "mutuallyExclusive");
}

// ---------------------------------------------------------------------------
// Unsatisfiable declarations: a capping rule over fields `required` also
// demands (issue #165). Every fixture below uses CFRMoney -- an
// EmptyCapableField, hence *required by default* -- rather than
// std::optional: `isStdOptional` keeps a std::optional member out of
// `required` on sight, so a std::optional-typed fixture could not produce the
// contradiction at all and would pass for the wrong reason.
// ---------------------------------------------------------------------------

/// exactlyOneOf over two fields that are both required by default: `required`
/// demands both, the rule permits exactly one. Nothing can be submitted.
struct CFRUnsatisfiableExactlyOne {
    CFRMoney value;
    CFRMoney qualifier;

    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::exactlyOneOf(&CFRUnsatisfiableExactlyOne::value, &CFRUnsatisfiableExactlyOne::qualifier));
};

/// The same contradiction via the other capping kind: "at most one" vs. "both".
struct CFRUnsatisfiableMutex {
    CFRMoney value;
    CFRMoney qualifier;

    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::mutuallyExclusive(&CFRUnsatisfiableMutex::value, &CFRUnsatisfiableMutex::qualifier));
};

/// The sanctioned encoding, as rung 6's CaptureConcentration writes it: the
/// rule is the *only* gate on the pair, so both members opt out of `required`.
struct CFRSumTypeOptedOut {
    CFRMoney value;
    CFRMoney qualifier;

    static constexpr std::array optionalFields{std::string_view{"value"}, std::string_view{"qualifier"}};
    static constexpr auto formRules =
        morph::forms::ruleList(morph::forms::exactlyOneOf(&CFRSumTypeOptedOut::value, &CFRSumTypeOptedOut::qualifier));
};

/// Exactly ONE required field inside a capping rule is satisfiable -- engage
/// that one, leave the rest empty -- so it must not be rejected.
struct CFROneRequiredInRule {
    CFRMoney value;
    CFRMoney qualifier;

    static constexpr std::array optionalFields{std::string_view{"qualifier"}};
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::exactlyOneOf(&CFROneRequiredInRule::value, &CFROneRequiredInRule::qualifier));
};

/// atLeastOneOf is a floor, never a ceiling: engaging both required fields
/// satisfies it. Rejecting this would be a false positive.
struct CFRAtLeastOneAllRequired {
    CFRMoney value;
    CFRMoney qualifier;

    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::atLeastOneOf(&CFRAtLeastOneAllRequired::value, &CFRAtLeastOneAllRequired::qualifier));
};

/// A capping rule alongside an unrelated required field: only the fields the
/// rule ranges over count, so this generates.
struct CFRCappedPairPlusRequiredNeighbour {
    CFRMoney sampleId;
    CFRMoney value;
    CFRMoney qualifier;

    static constexpr std::array optionalFields{std::string_view{"value"}, std::string_view{"qualifier"}};
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::mutuallyExclusive(
        &CFRCappedPairPlusRequiredNeighbour::value, &CFRCappedPairPlusRequiredNeighbour::qualifier));
};

TEST_CASE("Forms::Rules::Unsatisfiable::ExactlyOneOfOverTwoRequiredFieldsThrows", "[forms][rules][unsatisfiable]") {
    CHECK_THROWS_AS(morph::forms::schemaJson<CFRUnsatisfiableExactlyOne>(), morph::forms::UnsatisfiableFormError);
}

TEST_CASE("Forms::Rules::Unsatisfiable::MutuallyExclusiveOverTwoRequiredFieldsThrows",
          "[forms][rules][unsatisfiable]") {
    CHECK_THROWS_AS(morph::forms::schemaJson<CFRUnsatisfiableMutex>(), morph::forms::UnsatisfiableFormError);
}

TEST_CASE("Forms::Rules::Unsatisfiable::MessageNamesTheActionTheKindAndBothFields", "[forms][rules][unsatisfiable]") {
    // The whole point of the check is that the diagnostic says what the
    // debugging session would otherwise have to discover, so assert on it.
    std::string message{};
    try {
        static_cast<void>(morph::forms::schemaJson<CFRUnsatisfiableExactlyOne>());
    } catch (const morph::forms::UnsatisfiableFormError& error) {
        message = error.what();
    }
    CHECK(message.find("CFRUnsatisfiableExactlyOne") != std::string::npos);
    CHECK(message.find("exactlyOneOf") != std::string::npos);
    CHECK(message.find("optionalFields") != std::string::npos);
    // The two offenders are not merely *mentioned*: they are rendered as one
    // comma-separated list, in the order the rule ranges over them, with no
    // stray separator at either end. Checking only that each name appears
    // somewhere in a sentence that also contains the words "required array"
    // passes against a list rendered ", value, qualifier" or "valuequalifier",
    // and the list is the part of this message a reader acts on.
    CHECK(message.contains("required array: value, qualifier. No submission"));
}

TEST_CASE("Forms::Rules::Unsatisfiable::ThrowIsNotCachedAwayBySchemaJson", "[forms][rules][unsatisfiable]") {
    // schemaJson memoises into a function-local static; a throw during that
    // static's initialisation must leave it uninitialised, so a second call
    // re-runs the check rather than serving a half-built (or empty) schema.
    CHECK_THROWS_AS(morph::forms::schemaJson<CFRUnsatisfiableExactlyOne>(), morph::forms::UnsatisfiableFormError);
    CHECK_THROWS_AS(morph::forms::schemaJson<CFRUnsatisfiableExactlyOne>(), morph::forms::UnsatisfiableFormError);
}

TEST_CASE("Forms::Rules::Unsatisfiable::OptingBothMembersOutOfRequiredGenerates", "[forms][rules][unsatisfiable]") {
    std::string schema{};
    CHECK_NOTHROW(schema = morph::forms::schemaJson<CFRSumTypeOptedOut>());
    CHECK(schema.find(R"("required":[])") != std::string::npos);
    CHECK(schema.find(R"({"kind":"exactlyOneOf","fields":["value","qualifier"]})") != std::string::npos);
}

TEST_CASE("Forms::Rules::Unsatisfiable::OneRequiredFieldInACappingRuleIsAccepted", "[forms][rules][unsatisfiable]") {
    // Satisfiable: engage `value`, leave `qualifier` empty. Rejecting this
    // would be a false positive.
    std::string schema{};
    CHECK_NOTHROW(schema = morph::forms::schemaJson<CFROneRequiredInRule>());
    CHECK(schema.find(R"("required":["value"])") != std::string::npos);
}

TEST_CASE("Forms::Rules::Unsatisfiable::AtLeastOneOfIsAFloorAndNeverConflicts", "[forms][rules][unsatisfiable]") {
    std::string schema{};
    CHECK_NOTHROW(schema = morph::forms::schemaJson<CFRAtLeastOneAllRequired>());
    CHECK(schema.find(R"({"kind":"atLeastOneOf","fields":["value","qualifier"]})") != std::string::npos);
    CHECK(schema.find(R"("required":["value","qualifier"])") != std::string::npos);
}

TEST_CASE("Forms::Rules::Unsatisfiable::RequiredFieldsOutsideTheRuleAreIrrelevant", "[forms][rules][unsatisfiable]") {
    std::string schema{};
    CHECK_NOTHROW(schema = morph::forms::schemaJson<CFRCappedPairPlusRequiredNeighbour>());
    CHECK(schema.find(R"("required":["sampleId"])") != std::string::npos);
}

TEST_CASE("Forms::Rules::Unsatisfiable::StdOptionalMembersCanNeverConflict", "[forms][rules][unsatisfiable]") {
    // CFRContactForm's exactlyOneOf ranges over two std::optional members,
    // which isStdOptional keeps out of `required` on sight -- so the
    // contradiction is unreachable for them, with or without the check. Pinned
    // because a fixture like this one is the easy wrong way to test #165.
    std::string schema{};
    CHECK_NOTHROW(schema = morph::forms::schemaJson<CFRContactForm>());
    CHECK(schema.find(R"("required":[])") != std::string::npos);
}

TEST_CASE("Forms::Rules::Equals::EngagedComparesLiteralUnengagedIsFalse", "[forms][rules]") {
    // A Quantity field compared against an exact Rational literal.
    auto const cond = morph::forms::equals(&CFRDiscountForm::promo, Rational{5, DecimalPlaces{2}});
    CFRDiscountForm form{};
    CHECK_FALSE(cond.test(form));  // unengaged -> false, NOT vacuously true (unlike comparisons)

    form.promo = Rational{5, DecimalPlaces{2}};
    CHECK(cond.test(form));

    form.promo = Rational{6, DecimalPlaces{2}};
    CHECK_FALSE(cond.test(form));
}

TEST_CASE("Forms::Rules::Equals::PlainScalarField", "[forms][rules]") {
    struct CFRStatusAction {
        std::int64_t status = 0;
    };
    auto const cond = morph::forms::equals(&CFRStatusAction::status, std::int64_t{2});
    CFRStatusAction action{};
    CHECK_FALSE(cond.test(action));
    action.status = 2;
    CHECK(cond.test(action));
}

TEST_CASE("Forms::Rules::Equals::StringLiteralOverload", "[forms][rules]") {
    struct CFRCodeAction {
        std::optional<std::string> code;
    };
    auto const cond = morph::forms::equals(&CFRCodeAction::code, "URGENT");
    CFRCodeAction action{};
    CHECK_FALSE(cond.test(action));
    action.code = "URGENT";
    CHECK(cond.test(action));
    action.code = "OTHER";
    CHECK_FALSE(cond.test(action));
}

// The documented way to declare rules is a `static constexpr formRules`
// member, so a rule node has to be a literal type. Storing a string literal as
// a std::string made that true only while the text fit the standard library's
// small-string buffer (15 chars on libstdc++): one character more and the
// declaration failed with "refers to a result of operator new". Building the
// node as a function-local `auto const` -- as the case above does -- never
// exercised that, which is why the limit went unnoticed. These declare at
// namespace scope, where the constant evaluation actually happens.

// glaze's reflection-based get_name() needs external linkage — the same
// convention every other fixture in this file follows.
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct CFRLongCodeAction {
    std::optional<std::string> code;
    std::optional<std::string> reason;
    // 17 characters: one past libstdc++'s SSO buffer, so this declaration is
    // itself the regression test -- it does not compile without the fix.
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::requiredWhen(
        &CFRLongCodeAction::reason, morph::forms::equals(&CFRLongCodeAction::code, "AWAITING_APPROVAL")));
};

// See CFRLongCodeAction.
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct CFRVeryLongCodeAction {
    std::optional<std::string> code;
    std::optional<std::string> reason;
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::requiredWhen(
        &CFRVeryLongCodeAction::reason,
        morph::forms::equals(&CFRVeryLongCodeAction::code, "A_VERY_LONG_STATUS_CODE_WELL_PAST_ANY_SSO_BUFFER")));
};

TEST_CASE("Forms::Rules::Equals::StringLiteralOfAnyLengthIsConstexpr", "[forms][rules]") {
    // Evaluating the rule in a constant expression pins that the node is
    // genuinely usable at compile time, not merely declarable.
    static_assert([] {
        CFRLongCodeAction probe{};
        probe.code = "AWAITING_APPROVAL";
        return probe.code.has_value();
    }());

    CFRLongCodeAction action{};
    CHECK(morph::forms::allRulesSatisfied(action));  // condition does not hold -> vacuous
    action.code = "AWAITING_APPROVAL";
    CHECK_FALSE(morph::forms::allRulesSatisfied(action));  // now required, still unset
    action.reason = "waiting on legal";
    CHECK(morph::forms::allRulesSatisfied(action));
    action.code = "SOMETHING_ELSE";
    action.reason.reset();
    CHECK(morph::forms::allRulesSatisfied(action));
}

TEST_CASE("Forms::Rules::Equals::VeryLongStringLiteralStillCompares", "[forms][rules]") {
    CFRVeryLongCodeAction action{};
    action.code = "A_VERY_LONG_STATUS_CODE_WELL_PAST_ANY_SSO_BUFFER";
    CHECK_FALSE(morph::forms::allRulesSatisfied(action));
    action.reason = "r";
    CHECK(morph::forms::allRulesSatisfied(action));
    // A prefix must not compare equal — the captured length is significant.
    action.code = "A_VERY_LONG_STATUS_CODE";
    action.reason.reset();
    CHECK(morph::forms::allRulesSatisfied(action));
}

TEST_CASE("Forms::Rules::SchemaJson::LongStringLiteralSerializesAsAPlainJsonString", "[forms][rules]") {
    // Inline capture is a compile-time representation detail; the wire form is
    // the same JSON string a std::string literal produced.
    auto const schema = morph::forms::schemaJson<CFRLongCodeAction>();
    CHECK(schema.contains(R"("kind":"equals","fields":["code"],"value":"AWAITING_APPROVAL")"));
}

TEST_CASE("Forms::Rules::SchemaJson::EqualsEmitsRationalValueExactly", "[forms][rules]") {
    auto const schema = morph::forms::schemaJson<CFREqualsForm>();
    CHECK(schema.contains(R"("when":{"kind":"equals","fields":["promo"],"value":{"num":5,"den":1}})"));
}

// ---------------------------------------------------------------------------
// Compound conditions: and / or / not, nesting to any depth.
// ---------------------------------------------------------------------------

struct CFRCompoundForm {
    CFRMoney promo;
    CFRMoney discount;
    std::optional<std::string> email;
    std::optional<std::string> phone;

    // discount required when (promo engaged AND email engaged)
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::requiredWhen(
        &CFRCompoundForm::discount, morph::forms::andOf(morph::forms::engaged(&CFRCompoundForm::promo),
                                                        morph::forms::engaged(&CFRCompoundForm::email))));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

struct CFROrForm {
    CFRMoney promo;
    CFRMoney discount;
    std::optional<std::string> email;
    std::optional<std::string> phone;

    // discount required when (promo engaged OR email engaged)
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::requiredWhen(
        &CFROrForm::discount,
        morph::forms::orOf(morph::forms::engaged(&CFROrForm::promo), morph::forms::engaged(&CFROrForm::email))));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

struct CFRNotForm {
    CFRMoney promo;
    CFRMoney discount;

    // discount required when promo is NOT engaged.
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::requiredWhen(
        &CFRNotForm::discount, morph::forms::notOf(morph::forms::engaged(&CFRNotForm::promo))));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

struct CFRNestedForm {
    CFRMoney promo;
    CFRMoney discount;
    std::optional<std::string> email;
    std::optional<std::string> phone;

    // discount required when NOT(promo engaged) OR (email engaged AND phone engaged)
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::requiredWhen(
        &CFRNestedForm::discount,
        morph::forms::orOf(morph::forms::notOf(morph::forms::engaged(&CFRNestedForm::promo)),
                           morph::forms::andOf(morph::forms::engaged(&CFRNestedForm::email),
                                               morph::forms::engaged(&CFRNestedForm::phone)))));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

// A compound condition usable directly as a top-level rule too (not only
// nested inside a requiredWhen/visibleWhen/readonlyWhen `when` clause) --
// "a single rule with a compound condition tree", per the issue.
struct CFRTopLevelCompoundForm {
    CFRMoney promo;
    std::optional<std::string> email;

    static constexpr auto formRules =
        morph::forms::ruleList(morph::forms::andOf(morph::forms::engaged(&CFRTopLevelCompoundForm::promo),
                                                   morph::forms::engaged(&CFRTopLevelCompoundForm::email)));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

TEST_CASE("Forms::Rules::And::BothMustHold", "[forms][rules][compound]") {
    CFRCompoundForm form{};
    CHECK(morph::forms::allRulesSatisfied(form));  // neither engaged -> and() false -> not required

    form.promo = Rational{5, DecimalPlaces{2}};
    CHECK(morph::forms::allRulesSatisfied(form));  // only promo engaged -> and() still false

    form.email = "a@b.com";
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));  // both engaged -> and() true -> discount now required

    form.discount = Rational{1, DecimalPlaces{2}};
    CHECK(morph::forms::allRulesSatisfied(form));
}

TEST_CASE("Forms::Rules::Or::EitherSuffices", "[forms][rules][compound]") {
    CFROrForm form{};
    CHECK(morph::forms::allRulesSatisfied(form));  // neither engaged -> or() false -> not required

    form.promo = Rational{5, DecimalPlaces{2}};
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));  // promo engaged -> or() true -> discount required

    form.discount = Rational{1, DecimalPlaces{2}};
    CHECK(morph::forms::allRulesSatisfied(form));
}

TEST_CASE("Forms::Rules::Not::NegatesInnerCondition", "[forms][rules][compound]") {
    CFRNotForm form{};
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));  // promo unengaged -> not(engaged) true -> discount required

    form.discount = Rational{1, DecimalPlaces{2}};
    CHECK(morph::forms::allRulesSatisfied(form));

    form.promo = Rational{5, DecimalPlaces{2}};
    form.discount = CFRMoney{};
    CHECK(morph::forms::allRulesSatisfied(form));  // promo engaged -> not(engaged) false -> not required
}

TEST_CASE("Forms::Rules::Compound::NestsToAnyDepth", "[forms][rules][compound]") {
    CFRNestedForm form{};
    // promo unengaged -> notOf(engaged(promo)) is true -> or() true -> required
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));

    form.promo = Rational{5, DecimalPlaces{2}};
    // promo engaged -> notOf branch false; email/phone both unengaged -> andOf branch false -> or() false
    CHECK(morph::forms::allRulesSatisfied(form));

    form.email = "a@b.com";
    form.phone = "555";
    // promo engaged (notOf branch false) but email AND phone both engaged (andOf branch true) -> or() true
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));

    form.discount = Rational{1, DecimalPlaces{2}};
    CHECK(morph::forms::allRulesSatisfied(form));
}

TEST_CASE("Forms::Rules::Compound::UsableDirectlyAsATopLevelRule", "[forms][rules][compound]") {
    CFRTopLevelCompoundForm form{};
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));  // and() false while both unengaged

    form.promo = Rational{5, DecimalPlaces{2}};
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));

    form.email = "a@b.com";
    CHECK(morph::forms::allRulesSatisfied(form));  // both engaged -> and() true -> the rule itself holds
}

TEST_CASE("Forms::Rules::SchemaJson::AndOrNotEmitXRulesAsNestedNodes", "[forms][rules][compound]") {
    auto const schema = morph::forms::schemaJson<CFRCompoundForm>();
    CHECK(schema.contains(R"("x-rules":[{"kind":"requiredWhen","fields":["discount"],)"
                          R"("when":{"kind":"and","conditions":[)"
                          R"({"kind":"engaged","fields":["promo"]},)"
                          R"({"kind":"engaged","fields":["email"]}]}}])"));

    auto const orSchema = morph::forms::schemaJson<CFROrForm>();
    CHECK(orSchema.contains(R"("kind":"or","conditions":[)"));

    auto const notSchema = morph::forms::schemaJson<CFRNotForm>();
    CHECK(notSchema.contains(R"("when":{"kind":"not","condition":{"kind":"engaged","fields":["promo"]}}}])"));
}

TEST_CASE("Forms::Rules::And::VariadicAcceptsMoreThanTwoConditions", "[forms][rules][compound]") {
    struct CFRTriple {
        CFRMoney a;
        CFRMoney b;
        CFRMoney c;
    };
    CFRTriple triple{};
    auto const cond = morph::forms::andOf(morph::forms::engaged(&CFRTriple::a), morph::forms::engaged(&CFRTriple::b),
                                          morph::forms::engaged(&CFRTriple::c));
    CHECK_FALSE(cond.test(triple));
    triple.a = Rational{1, DecimalPlaces{2}};
    triple.b = Rational{1, DecimalPlaces{2}};
    triple.c = Rational{1, DecimalPlaces{2}};
    CHECK(cond.test(triple));
}

// ---------------------------------------------------------------------------
// Presentation rules: visibleWhen / readonlyWhen (never gate the submit check).
// ---------------------------------------------------------------------------

struct CFRPromoForm {
    CFRMoney promo;
    CFRMoney discount;

    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::requiredWhen(&CFRPromoForm::discount, morph::forms::engaged(&CFRPromoForm::promo)),
        morph::forms::visibleWhen(&CFRPromoForm::discount, morph::forms::engaged(&CFRPromoForm::promo)),
        morph::forms::readonlyWhen(&CFRPromoForm::promo, morph::forms::engaged(&CFRPromoForm::discount)));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

TEST_CASE("Forms::Rules::VisibleWhen::NeverGatesTheCheck", "[forms][rules]") {
    CFRPromoForm form{};
    // visibleWhen never affects allRulesSatisfied, in either visibility state.
    CHECK(morph::forms::allRulesSatisfied(form));  // promo unengaged: discount not required, hidden

    auto const visibility =
        morph::forms::visibleWhen(&CFRPromoForm::discount, morph::forms::engaged(&CFRPromoForm::promo));
    CHECK(visibility.test(form));             // VisibleWhen::test() is always true -- it never gates, by design
    CHECK_FALSE(visibility.when.test(form));  // the CONDITION it carries is false right now (promo unengaged)
    CHECK(visibility.isPresentation);
}

TEST_CASE("Forms::Rules::VisibleWhen::TestAlwaysTrueEvenThoughItsOwnConditionCanBeFalse", "[forms][rules]") {
    // VisibleWhen::test() always returns true (it never gates); the
    // condition it carries (`when`) is what a renderer inspects separately
    // to decide the control's visibility.
    CFRPromoForm form{};
    auto const rule = morph::forms::visibleWhen(&CFRPromoForm::discount, morph::forms::engaged(&CFRPromoForm::promo));
    CHECK(rule.test(form));             // the RULE never fails
    CHECK_FALSE(rule.when.test(form));  // the CONDITION it carries is false right now (promo unengaged)

    form.promo = Rational{5, DecimalPlaces{2}};
    CHECK(rule.test(form));       // still never fails
    CHECK(rule.when.test(form));  // condition now true
}

TEST_CASE("Forms::Rules::SchemaJson::VisibleWhenAndReadonlyWhenEmitXRules", "[forms][rules]") {
    auto const schema = morph::forms::schemaJson<CFRPromoForm>();
    CHECK(schema.contains(
        R"({"kind":"visibleWhen","fields":["discount"],"when":{"kind":"engaged","fields":["promo"]}})"));
    CHECK(schema.contains(
        R"({"kind":"readonlyWhen","fields":["promo"],"when":{"kind":"engaged","fields":["discount"]}})"));
}

TEST_CASE("Forms::Rules::PresentationRules::NeverBreakAllRulesSatisfiedAcrossStates", "[forms][rules]") {
    CFRPromoForm form{};
    CHECK(morph::forms::allRulesSatisfied(form));

    form.promo = Rational{5, DecimalPlaces{2}};
    CHECK_FALSE(morph::forms::allRulesSatisfied(form));  // discount now required by requiredWhen (a validation rule)

    form.discount = Rational{1, DecimalPlaces{2}};
    CHECK(morph::forms::allRulesSatisfied(form));  // requiredWhen satisfied; visibleWhen/readonlyWhen never gate
}

// ---------------------------------------------------------------------------
// No-drift: one declaration, evaluated identically on every dispatch path.
// ---------------------------------------------------------------------------

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>

namespace {
std::atomic<int> gCfrBookRoomExecuteCount{0};
}  // namespace

struct CFRBookRoom {
    morph::time::Timestamp checkIn;
    morph::time::Timestamp checkOut;
    std::optional<std::string> email;
    std::optional<std::string> phone;
    CFRMoney promo;
    CFRMoney discount;

    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::greater(&CFRBookRoom::checkOut, &CFRBookRoom::checkIn),
        morph::forms::exactlyOneOf(&CFRBookRoom::email, &CFRBookRoom::phone),
        morph::forms::requiredWhen(&CFRBookRoom::discount, morph::forms::engaged(&CFRBookRoom::promo)),
        morph::forms::visibleWhen(&CFRBookRoom::discount, morph::forms::engaged(&CFRBookRoom::promo)));

    [[nodiscard]] bool validate() const { return morph::forms::allRulesSatisfied(*this); }
};

struct CFRBookRoomResult {
    bool booked = false;
};

struct CFRBookingModel {
    CFRBookRoomResult execute(const CFRBookRoom&) {
        gCfrBookRoomExecuteCount.fetch_add(1);
        return CFRBookRoomResult{.booked = true};
    }
};

BRIDGE_REGISTER_MODEL(CFRBookingModel, "CFR_BookingModel")
BRIDGE_REGISTER_ACTION(CFRBookingModel, CFRBookRoom, "CFR_BookRoom")

namespace {

[[nodiscard]] CFRBookRoom validRoomBooking() {
    using morph::time::DateTime;
    using morph::time::Timestamp;
    CFRBookRoom room{};
    room.checkIn = Timestamp{DateTime{std::chrono::year{2026}, std::chrono::month{8}, std::chrono::day{1},
                                      std::chrono::hours{0}, std::chrono::minutes{0}, std::chrono::seconds{0}}};
    room.checkOut = Timestamp{DateTime{std::chrono::year{2026}, std::chrono::month{8}, std::chrono::day{5},
                                       std::chrono::hours{0}, std::chrono::minutes{0}, std::chrono::seconds{0}}};
    room.email = "guest@example.com";
    return room;
}

[[nodiscard]] CFRBookRoom invalidRoomBooking() {
    // checkOut before checkIn -> violates greater(checkOut, checkIn).
    using morph::time::DateTime;
    using morph::time::Timestamp;
    CFRBookRoom room{};
    room.checkIn = Timestamp{DateTime{std::chrono::year{2026}, std::chrono::month{8}, std::chrono::day{5},
                                      std::chrono::hours{0}, std::chrono::minutes{0}, std::chrono::seconds{0}}};
    room.checkOut = Timestamp{DateTime{std::chrono::year{2026}, std::chrono::month{8}, std::chrono::day{1},
                                       std::chrono::hours{0}, std::chrono::minutes{0}, std::chrono::seconds{0}}};
    room.email = "guest@example.com";
    return room;
}

}  // namespace

TEST_CASE("Forms::Rules::NoDrift::ActionDispatcherRejectsViaValidationError", "[forms][rules][validation]") {
    gCfrBookRoomExecuteCount.store(0);
    auto holder = morph::model::detail::ModelFactory::create<CFRBookingModel>();
    auto const payload = morph::model::ActionTraits<CFRBookRoom>::toJson(invalidRoomBooking());

    REQUIRE_THROWS_AS(morph::model::detail::ActionDispatcher::instance().dispatch("CFR_BookingModel", "CFR_BookRoom",
                                                                                  *holder, payload),
                      morph::model::ValidationError);
    REQUIRE(gCfrBookRoomExecuteCount.load() == 0);
}

TEST_CASE("Forms::Rules::NoDrift::ActionDispatcherDispatchesAValidBookingNormally", "[forms][rules][validation]") {
    gCfrBookRoomExecuteCount.store(0);
    auto holder = morph::model::detail::ModelFactory::create<CFRBookingModel>();
    auto const payload = morph::model::ActionTraits<CFRBookRoom>::toJson(validRoomBooking());

    auto const resultJson = morph::model::detail::ActionDispatcher::instance().dispatch(
        "CFR_BookingModel", "CFR_BookRoom", *holder, payload);
    auto const result = morph::model::ActionTraits<CFRBookRoom>::resultFromJson(resultJson);
    CHECK(result.booked);
    CHECK(gCfrBookRoomExecuteCount.load() == 1);
}

TEST_CASE("Forms::Rules::NoDrift::LocalBackendRejectsViaOnErrorWithValidationError", "[forms][rules][validation]") {
    gCfrBookRoomExecuteCount.store(0);
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CFRBookingModel> handler{bridge, &cbExec};

    std::atomic<bool> sawValidationError{false};
    std::atomic<bool> done{false};
    handler.execute(invalidRoomBooking())
        .then([&](CFRBookRoomResult) { done.store(true); })
        .onError([&](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const morph::model::ValidationError&) {
                sawValidationError.store(true);
            } catch (...) {
            }
            done.store(true);
        });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(sawValidationError.load());
    REQUIRE(gCfrBookRoomExecuteCount.load() == 0);
}

TEST_CASE("Forms::Rules::NoDrift::LocalBackendDispatchesAValidBookingNormally", "[forms][rules][validation]") {
    gCfrBookRoomExecuteCount.store(0);
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CFRBookingModel> handler{bridge, &cbExec};

    std::atomic<bool> observedBooked{false};
    std::atomic<bool> done{false};
    handler.execute(validRoomBooking())
        .then([&](CFRBookRoomResult result) {
            observedBooked.store(result.booked);
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(observedBooked.load());
    REQUIRE(gCfrBookRoomExecuteCount.load() == 1);
}

TEST_CASE("Forms::Rules::NoDrift::SimulatedRemoteBackendRejectsTheSameViolatingAction", "[forms][rules][validation]") {
    gCfrBookRoomExecuteCount.store(0);
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::bridge::BridgeHandler<CFRBookingModel> handler{bridge, &cbExec};

    std::atomic<bool> sawError{false};
    std::string errorMessage;
    std::atomic<bool> done{false};
    handler.execute(invalidRoomBooking())
        .then([&](CFRBookRoomResult) { done.store(true); })
        .onError([&](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& exc) {
                errorMessage = exc.what();
                sawError.store(true);
            }
            done.store(true);
        });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(sawError.load());
    REQUIRE(errorMessage == "action failed validation: CFR_BookingModel/CFR_BookRoom");
    REQUIRE(gCfrBookRoomExecuteCount.load() == 0);
}

TEST_CASE("Forms::Rules::NoDrift::SimulatedRemoteBackendDispatchesAValidBookingNormally",
          "[forms][rules][validation]") {
    gCfrBookRoomExecuteCount.store(0);
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::bridge::BridgeHandler<CFRBookingModel> handler{bridge, &cbExec};

    std::atomic<bool> observedBooked{false};
    std::atomic<bool> done{false};
    handler.execute(validRoomBooking())
        .then([&](CFRBookRoomResult result) {
            observedBooked.store(result.booked);
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(observedBooked.load());
    REQUIRE(gCfrBookRoomExecuteCount.load() == 1);
}
