// SPDX-License-Identifier: Apache-2.0

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

TEST_CASE("Forms::Rules::SchemaJson::EqualsEmitsRationalValueExactly", "[forms][rules]") {
    auto const schema = morph::forms::schemaJson<CFREqualsForm>();
    CHECK(schema.contains(R"("when":{"kind":"equals","fields":["promo"],"value":{"num":5,"den":1}})"));
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
