// SPDX-License-Identifier: Apache-2.0
//
// The form engine's value helpers: what a literal denotes for rules, exact multiples, decimal objects and the bounds
// of a typed decimal.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/values.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace {

using morph::forms::FieldSpec;
using morph::forms::FormModel;
using morph::forms::ValueContext;
namespace engine = morph::forms::detail::engine;

FieldSpec fieldOf(std::string_view propertyJson) {
    std::string const schema = R"({"properties":{"f":)" + std::string{propertyJson} + R"(},"required":["f"]})";
    auto model = FormModel::fromSchema("T", schema);
    REQUIRE(model.has_value());
    return model->fields().front();
}

morph::math::Rational rat(std::int64_t num, std::int64_t den) {
    return morph::math::Rational{morph::math::Numerator{num}, morph::math::Denominator{den},
                                 morph::math::DecimalPlaces{2}};
}

}  // namespace

TEST_CASE("an integer's digits denote an int64 only when they are one", "[covc][forms][values]") {
    auto const whole = engine::integerValue("42");
    REQUIRE(whole.asInt() != nullptr);
    CHECK(*whole.asInt() == 42);
    CHECK(engine::integerValue("1.5").asInt() == nullptr);
    CHECK(engine::integerValue("99999999999999999999").asInt() == nullptr);
    CHECK(engine::integerValue("").asInt() == nullptr);
}

TEST_CASE("a decimal's text denotes a decimal only when it spells one", "[covc][forms][values]") {
    CHECK(engine::decimalValue("1.25").asDecimal() != nullptr);
    CHECK(engine::decimalValue("not a number").asDecimal() == nullptr);
}

TEST_CASE("a literal denotes the exact value its digits spell", "[covc][forms][values]") {
    auto const fraction = engine::valueOfLiteral(*engine::parseJson("1.25"));
    REQUIRE(fraction.asDecimal() != nullptr);
    auto const whole = engine::valueOfLiteral(*engine::parseJson("7"));
    CHECK(whole.asInt() != nullptr);
    CHECK(engine::valueOfLiteral(engine::Json{}).asInt() == nullptr);
}

TEST_CASE("a value is a multiple of a step only when the quotient is whole and fits", "[covc][forms][values]") {
    CHECK(engine::isMultiple(rat(3, 2), rat(1, 2)));
    CHECK_FALSE(engine::isMultiple(rat(3, 2), rat(1, 1)));
    CHECK_FALSE(engine::isMultiple(rat(1, 1), rat(0, 1)));
    CHECK_FALSE(engine::isMultiple(rat(std::numeric_limits<std::int64_t>::max(), 1), rat(1, 3)));
}

TEST_CASE("a decimal object names its text by num, den and dp", "[covc][forms][values]") {
    auto const object = [](std::string_view text) { return engine::decimalObjectText(*engine::parseJson(text)); };
    CHECK(object(R"({"num":125,"den":100,"dp":2})") == std::optional<std::string>{"1.25"});
    CHECK(object(R"({"num":5,"den":1})") == std::optional<std::string>{"5"});
    CHECK_FALSE(object(R"({"den":100})"));
    CHECK_FALSE(object(R"({"num":1})"));
    CHECK_FALSE(object(R"({"num":1,"den":0})"));
    CHECK_FALSE(object(R"({"num":"x","den":1})"));
}

TEST_CASE("a typed decimal is held to its bounds and its step", "[covc][forms][values]") {
    ValueContext const context{};
    auto const spec = fieldOf(R"({"type":"number","minimum":1,"maximum":10,"multipleOf":0.5})");
    auto const code = [&](std::string_view canonical) {
        auto const error = engine::decimalBounds(spec, canonical, context);
        return error ? error->code : std::string{};
    };
    CHECK(code("2.5").empty());
    CHECK(code("0.5") == "below-minimum");
    CHECK(code("11") == "above-maximum");
    CHECK(code("2.3") == "not-a-multiple");
}
