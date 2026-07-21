// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>

using morph::math::DecimalPlaces;
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
