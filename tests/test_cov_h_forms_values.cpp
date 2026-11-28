// SPDX-License-Identifier: Apache-2.0

// The engine's encoding of a draft into a wire value, for specs a schema cannot spell but a host can build: a step
// that is no number, units with no scale, an option whose literal does not read, and kinds that carry no draft.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/values.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using morph::forms::ChoiceOption;
using morph::forms::Encoding;
using morph::forms::FieldKind;
using morph::forms::FieldSpec;
using morph::forms::UnitOption;
using morph::forms::ValueContext;
namespace engine = morph::forms::detail::engine;

FieldSpec decimalSpec(std::uint32_t places) {
    FieldSpec spec;
    spec.name = "f";
    spec.kind = FieldKind::Decimal;
    spec.decimalPlaces = places;
    return spec;
}

// The wire JSON of an encoding, or `error:` and its code.
std::string shown(Encoding const& encoding) {
    if (!encoding) {
        return "blank";
    }
    if (!*encoding) {
        return "error:" + encoding->error().code;
    }
    return (*encoding)->json;
}

}  // namespace

TEST_CASE("cov h values: a decimal draft is held to a step, a scale and units a host built",
          "[covh][forms-engine][values]") {
    ValueContext const context{};
    struct Case {
        char const* what;
        FieldSpec spec;
        char const* draft;
        std::size_t unit;
        std::optional<std::uint32_t> places;
        std::string expected;
    };
    auto const withStep = [](char const* step) {
        FieldSpec spec = decimalSpec(2);
        spec.multipleOf = step;
        return spec;
    };
    auto const withUnits = [](std::uint32_t firstDecimals, std::uint32_t secondDecimals) {
        FieldSpec spec = decimalSpec(2);
        spec.kind = FieldKind::Quantity;
        spec.units = {UnitOption{.id = "a", .display = "a", .decimals = firstDecimals, .num = 1, .den = 1},
                      UnitOption{.id = "b", .display = "b", .decimals = secondDecimals, .num = 1, .den = 1}};
        return spec;
    };
    std::vector<Case> const cases{
        {.what = "a step that is no number",
         .spec = withStep("bogus"),
         .draft = "1.5",
         .unit = 0,
         .places = std::nullopt,
         .expected = "error:not-a-multiple"},
        {.what = "a step of zero",
         .spec = withStep("0"),
         .draft = "1.5",
         .unit = 0,
         .places = std::nullopt,
         .expected = "error:not-a-multiple"},
        {.what = "a step the value meets",
         .spec = withStep("0.5"),
         .draft = "1.5",
         .unit = 0,
         .places = std::nullopt,
         .expected = R"({"num":150,"den":100,"dp":2})"},
        {.what = "an exponent is not a decimal",
         .spec = decimalSpec(2),
         .draft = "1e5",
         .unit = 0,
         .places = std::nullopt,
         .expected = "error:malformed"},
        {.what = "letters are not a decimal",
         .spec = decimalSpec(2),
         .draft = "abc",
         .unit = 0,
         .places = std::nullopt,
         .expected = "error:malformed"},
        {.what = "two separators",
         .spec = decimalSpec(2),
         .draft = "1.2.3",
         .unit = 0,
         .places = std::nullopt,
         .expected = "error:malformed"},
        {.what = "two signs",
         .spec = decimalSpec(2),
         .draft = "--5",
         .unit = 0,
         .places = std::nullopt,
         .expected = "error:malformed"},
        {.what = "a lone separator",
         .spec = decimalSpec(2),
         .draft = ".",
         .unit = 0,
         .places = std::nullopt,
         .expected = "error:malformed"},
        {.what = "entry places of a unit scale past what the wire holds",
         .spec = withUnits(19, 19),
         .draft = "1",
         .unit = 0,
         .places = std::nullopt,
         .expected = "error:out-of-range"},
        {.what = "own places on the first unit replace its decimals",
         .spec = withUnits(0, 0),
         .draft = "1.25",
         .unit = 0,
         .places = 2,
         .expected = R"({"num":125,"den":100,"dp":2})"},
        {.what = "own places on a later unit leave its decimals",
         .spec = withUnits(0, 3),
         .draft = "1.125",
         .unit = 1,
         .places = 2,
         .expected = R"({"num":1125,"den":1000,"dp":2})"},
        {.what = "own places beyond the units given",
         .spec = withUnits(0, 3),
         .draft = "1.25",
         .unit = 7,
         .places = 2,
         .expected = R"({"num":125,"den":100,"dp":2})"},
        {.what = "own places past the largest are held to it",
         .spec = decimalSpec(2),
         .draft = "1.5",
         .unit = 0,
         .places = 99,
         .expected = R"({"num":1500000000000000000,"den":1000000000000000000,"dp":18})"},
    };
    for (auto const& item : cases) {
        INFO(item.what);
        CHECK(shown(morph::forms::encodeScalar(item.spec, item.draft, item.unit, context, item.places)) ==
              item.expected);
    }
}

TEST_CASE("cov h values: a bound or step that is no number refuses what it cannot compare",
          "[covh][forms-engine][values]") {
    ValueContext const context{};
    FieldSpec spec = decimalSpec(2);
    spec.multipleOf = "bogus";
    auto const code = [&](std::string_view canonical) {
        auto const error = engine::decimalBounds(spec, canonical, context);
        return error ? error->code : std::string{};
    };
    CHECK(code("1.5") == "not-a-multiple");
    spec.multipleOf = "0.5";
    CHECK(code("1.5").empty());
    CHECK(code("not-a-decimal") == "not-a-multiple");
}

TEST_CASE("cov h values: an option whose literal does not read keeps its text and holds no value",
          "[covh][forms-engine][values]") {
    ValueContext const context{};
    FieldSpec spec;
    spec.name = "f";
    spec.kind = FieldKind::Enum;
    spec.enumOptions = {ChoiceOption{.valueJson = "{bad", .label = "bad"},
                        ChoiceOption{.valueJson = "\"ok\"", .label = "ok"}};
    auto const bad = morph::forms::encodeScalar(spec, "{bad", 0, context);
    REQUIRE(bad.has_value());
    REQUIRE(bad->has_value());
    CHECK((*bad)->json == "{bad");
    CHECK((*bad)->value.isNull());
    auto const ok = morph::forms::encodeScalar(spec, "\"ok\"", 0, context);
    REQUIRE(ok.has_value());
    REQUIRE(ok->has_value());
    REQUIRE((*ok)->value.asString() != nullptr);
    CHECK(*(*ok)->value.asString() == "ok");
}

TEST_CASE("cov h values: a nested object has no draft and no placeholder", "[covh][forms-engine][values]") {
    ValueContext const context{};
    for (FieldKind const kind : {FieldKind::Object, FieldKind::ObjectArray}) {
        FieldSpec spec;
        spec.name = "f";
        spec.kind = kind;
        INFO(morph::forms::fieldKindName(kind));
        CHECK(shown(morph::forms::encodeScalar(spec, "x", 0, context)) == "error:unrepresentable");
        CHECK(morph::forms::decodeScalar(spec, *engine::parseJson("{\"a\":1}"), context).empty());
        CHECK(morph::forms::defaultPlaceholder(spec).empty());
    }
    for (FieldKind const kind :
         {FieldKind::Text, FieldKind::Multiline, FieldKind::Boolean, FieldKind::Enum, FieldKind::Choice,
          FieldKind::DateTime, FieldKind::Date, FieldKind::EpochDateTime, FieldKind::EpochDate}) {
        FieldSpec spec;
        spec.name = "f";
        spec.kind = kind;
        INFO(morph::forms::fieldKindName(kind));
        CHECK(morph::forms::defaultPlaceholder(spec).empty());
        spec.placeholder = "hint";
        CHECK(morph::forms::defaultPlaceholder(spec) == "hint");
    }
}
