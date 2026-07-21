// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>

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
