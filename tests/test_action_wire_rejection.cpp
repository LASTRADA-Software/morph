// SPDX-License-Identifier: Apache-2.0
//
// The decode boundary rejects an action payload carrying a Rational that
// cannot be represented.
//
// morph::wire carries an execute envelope's `body` as an opaque string and
// never parses it (wire.hpp says so: "payload smuggled *inside* `body` is
// invisible to any structural/depth check"). ActionTraits<A>::fromJson is
// therefore the first and only place a Rational inside that body is decoded --
// which makes it the layer that has to decide what a clamped value means,
// because it is the layer that knows the bytes came off a wire.

#include <catch2/catch_test_macros.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/util/rational.hpp>
#include <string>

// External linkage, deliberately: glaze's reflection needs it -- a type in an
// anonymous namespace fails with "used but not defined in this translation
// unit, and cannot be defined in any other because its type does not have
// linkage".
struct WireAmountAction {
    morph::math::Rational amount;
};

struct WireAmountResult {
    std::int64_t numerator = 0;
};

struct WireAmountModel {
    WireAmountResult execute(const WireAmountAction& action) {
        return WireAmountResult{.numerator = action.amount.numerator};
    }
};

BRIDGE_REGISTER_MODEL(WireAmountModel, "Test_WireRejection_Model")
BRIDGE_REGISTER_ACTION(WireAmountModel, WireAmountAction, "Test_WireRejection_Action")

TEST_CASE("A well-formed Rational in an action body decodes normally", "[registry][wire]") {
    const auto action =
        morph::model::ActionTraits<WireAmountAction>::fromJson(R"({"amount":{"num":5,"den":2,"dp":2}})");
    CHECK(action.amount.numerator == 5);
    CHECK(action.amount.denominator == 2);
}

TEST_CASE("A non-canonical but representable Rational is accepted, not rejected", "[registry][wire]") {
    // 4/8 reduces to 1/2. Reduction is canonicalisation, not clamping: the
    // value survives intact, so the payload is legitimate.
    const auto action =
        morph::model::ActionTraits<WireAmountAction>::fromJson(R"({"amount":{"num":4,"den":8,"dp":2}})");
    CHECK(action.amount.numerator == 1);
    CHECK(action.amount.denominator == 2);
}

TEST_CASE("A zero denominator is rejected at the decode boundary", "[registry][wire]") {
    // Previously this decoded to a perfectly plausible 5/1 and travelled on.
    // The model's own validate() could not have caught it -- by then there is
    // nothing to see.
    CHECK_THROWS_AS(morph::model::ActionTraits<WireAmountAction>::fromJson(R"({"amount":{"num":5,"den":0,"dp":2}})"),
                    morph::model::detail::ParseError);
}

TEST_CASE("An out-of-range precision is rejected at the decode boundary", "[registry][wire]") {
    CHECK_THROWS_AS(morph::model::ActionTraits<WireAmountAction>::fromJson(R"({"amount":{"num":5,"den":2,"dp":99}})"),
                    morph::model::detail::ParseError);
}

TEST_CASE("A component whose magnitude is not representable is rejected", "[registry][wire]") {
    CHECK_THROWS_AS(morph::model::ActionTraits<WireAmountAction>::fromJson(
                        R"({"amount":{"num":-9223372036854775808,"den":2,"dp":2}})"),
                    morph::model::detail::ParseError);
}

TEST_CASE("Rejection does not leak into the next decode", "[registry][wire]") {
    CHECK_THROWS_AS(morph::model::ActionTraits<WireAmountAction>::fromJson(R"({"amount":{"num":5,"den":0,"dp":2}})"),
                    morph::model::detail::ParseError);

    // The clamp count is scoped to one decode; a rejected payload must not
    // poison the payload after it.
    const auto action =
        morph::model::ActionTraits<WireAmountAction>::fromJson(R"({"amount":{"num":7,"den":2,"dp":2}})");
    CHECK(action.amount.numerator == 7);
}
