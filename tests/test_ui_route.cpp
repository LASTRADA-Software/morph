// SPDX-License-Identifier: Apache-2.0

// Routes (spec 6 §20): every representable instance round-trips through its route, and every refusal names the
// route and the reason.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/document.hpp>
#include <morph/ui/route.hpp>
#include <morph/ui/value.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>

using morph::ui::Decimal;
using morph::ui::Document;
using morph::ui::Value;
using morph::ui::ValueRecord;

namespace {

std::shared_ptr<Document const> load(std::string_view text) {
    auto loaded = morph::ui::loadDocumentText(text);
    REQUIRE(loaded.has_value());
    return std::make_shared<Document const>(std::move(*loaded));
}

constexpr std::string_view kSample = R"({ "vocab": "ui/1", "id": "lims.sample",
  "params": { "sampleId": "int", "note": "string?", "factor": "decimal?", "row": "key?", "draft": "bool?" },
  "view": { "kind": "text", "text": "x" } })";

constexpr std::string_view kTimed = R"({ "vocab": "ui/1", "id": "lims.log",
  "params": { "since": "timestamp" }, "view": { "kind": "text", "text": "x" } })";

struct Screens {
    std::shared_ptr<Document const> sample = load(kSample);
    std::shared_ptr<Document const> timed = load(kTimed);

    [[nodiscard]] std::shared_ptr<Document const> operator()(std::string_view id) const {
        if (id == sample->id) {
            return sample;
        }
        if (id == timed->id) {
            return timed;
        }
        return nullptr;
    }
};

Value decimal(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Value{Decimal{morph::math::Rational{morph::math::Numerator{num}, morph::math::Denominator{den},
                                               morph::math::DecimalPlaces{places}}}};
}

}  // namespace

// Mutation: write a decimal's places as a plain number (the route reads back with other places).
TEST_CASE("a route writes each param's canonical text and reads back to the same instance", "[ui][route]") {
    Screens const screens;
    ValueRecord const params{{"sampleId", Value{std::int64_t{1042}}},
                             {"note", Value{std::string{"a b&c=d/é"}}},
                             {"factor", decimal(-5, 4, 3)},
                             {"row", Value{std::string{"x:1"}}},
                             {"draft", Value{true}}};
    auto const route = morph::ui::routeOf(*screens.sample, params);
    REQUIRE(route.has_value());
    CHECK(*route == "lims.sample?sampleId=1042&note=a%20b%26c%3Dd%2F%C3%A9&factor=-5/4@3&row=s:x%3A1&draft=true");

    auto const parsed = morph::ui::parseRoute(*route, screens);
    REQUIRE(parsed.has_value());
    CHECK(parsed->screen == "lims.sample");
    CHECK(parsed->params == params);

    ValueRecord const keyed{{"sampleId", Value{std::int64_t{-7}}}, {"row", Value{std::int64_t{9007199254740993}}}};
    auto const keyedRoute = morph::ui::routeOf(*screens.sample, keyed);
    REQUIRE(keyedRoute.has_value());
    CHECK(*keyedRoute == "lims.sample?sampleId=-7&row=i:9007199254740993");
    CHECK(morph::ui::parseRoute(*keyedRoute, screens)->params == keyed);
}

TEST_CASE("a null optional is left out of the route, and the bare id is a route", "[ui][route]") {
    Screens const screens;
    ValueRecord const params{{"sampleId", Value{std::int64_t{1}}}, {"note", Value{}}};
    CHECK(morph::ui::routeOf(*screens.sample, params) == "lims.sample?sampleId=1");
    auto const parsed = morph::ui::parseRoute("lims.sample?sampleId=1", screens);
    REQUIRE(parsed.has_value());
    CHECK(parsed->params == ValueRecord{{"sampleId", Value{std::int64_t{1}}}});
}

TEST_CASE("an instance that has no route says why", "[ui][route]") {
    Screens const screens;
    CHECK_FALSE(morph::ui::routeOf(*screens.sample, ValueRecord{}).has_value());  // required sampleId missing
    CHECK_FALSE(morph::ui::routeOf(*screens.sample, ValueRecord{{"sampleId", Value{std::string{"1"}}}}).has_value());
    auto const timed = morph::ui::routeOf(*screens.timed, ValueRecord{{"since", Value{std::int64_t{0}}}});
    REQUIRE_FALSE(timed.has_value());
    CHECK_THAT(timed.error(), Catch::Matchers::ContainsSubstring("no route form"));
}

// Mutation: accept a value that does not parse as its type (the int case reads "12x" as 12).
TEST_CASE("a route is refused with the route and the reason", "[ui][route]") {
    Screens const screens;
    auto const reason = [&](std::string_view route) {
        auto const parsed = morph::ui::parseRoute(route, screens);
        REQUIRE_FALSE(parsed.has_value());
        CHECK_THAT(parsed.error(), Catch::Matchers::StartsWith("route " + std::string{route} + ": "));
        return parsed.error();
    };
    CHECK_THAT(reason("lims.nothing?sampleId=1"), Catch::Matchers::ContainsSubstring("unknown screen"));
    CHECK_THAT(reason("lims.sample"), Catch::Matchers::ContainsSubstring("required param sampleId"));
    CHECK_THAT(reason("lims.sample?sampleId=12x"), Catch::Matchers::ContainsSubstring("does not parse"));
    CHECK_THAT(reason("lims.sample?sampleId=1&draft=yes"), Catch::Matchers::ContainsSubstring("does not parse"));
    CHECK_THAT(reason("lims.sample?sampleId=1&factor=1/0@2"), Catch::Matchers::ContainsSubstring("does not parse"));
    CHECK_THAT(reason("lims.sample?sampleId=1&factor=1/2"), Catch::Matchers::ContainsSubstring("does not parse"));
    CHECK_THAT(reason("lims.sample?sampleId=1&row=z:1"), Catch::Matchers::ContainsSubstring("does not parse"));
    CHECK_THAT(reason("lims.sample?sampleId=1&note=%G1"), Catch::Matchers::ContainsSubstring("does not parse"));
    CHECK_THAT(reason("lims.sample?sampleId=1&note=%4"), Catch::Matchers::ContainsSubstring("does not parse"));
    CHECK_THAT(reason("lims.sample?sampleId=1&other=2"), Catch::Matchers::ContainsSubstring("declares no param"));
    CHECK_THAT(reason("lims.sample?sampleId=1&sampleId=2"), Catch::Matchers::ContainsSubstring("given twice"));
    CHECK_THAT(reason("lims.sample?sampleId"), Catch::Matchers::ContainsSubstring("without a value"));
}

// Mutation: read lowercase hex escapes as invalid, or drop a type's guard on a value of another kind.
TEST_CASE("route: params of every routable type refuse a value of another kind, and escapes read either case",
          "[ui][route]") {
    namespace route = morph::ui::detail::route;
    using morph::ui::Type;
    using morph::ui::TypeKind;
    using morph::ui::Value;
    CHECK(route::decode("%2f%2F") == "//");
    CHECK_FALSE(route::decode("%zz").has_value());
    CHECK_FALSE(route::decode("%2").has_value());
    for (TypeKind const kind : {TypeKind::Int, TypeKind::Bool, TypeKind::String, TypeKind::Decimal, TypeKind::Key}) {
        CAPTURE(static_cast<int>(kind));
        CHECK_FALSE(route::textOf(Type::of(kind), Value{morph::ui::ValueList{}}).has_value());
    }
    for (TypeKind const kind : {TypeKind::Any, TypeKind::Quantity, TypeKind::Timestamp, TypeKind::Record}) {
        CHECK_FALSE(route::textOf(Type::of(kind), Value{std::int64_t{1}}).has_value());
        CHECK_FALSE(route::valueOf(Type::of(kind), "1").has_value());
    }
    CHECK_FALSE(route::valueOf(Type::of(TypeKind::Key), "i:x").has_value());
    CHECK_FALSE(route::valueOf(Type::of(TypeKind::Key), "s:%zz").has_value());
    CHECK_FALSE(route::valueOf(Type::of(TypeKind::Key), "q:1").has_value());
    CHECK_FALSE(route::valueOf(Type::of(TypeKind::String), "%zz").has_value());
    CHECK_FALSE(route::valueOf(Type::of(TypeKind::Bool), "yes").has_value());
    CHECK(route::valueOf(Type::of(TypeKind::Key), "s:a%20b") == Value{"a b"});
}

// Mutation: accept a param name whose escape does not decode (the param is looked up under garbage).
TEST_CASE("a route whose param name does not decode is refused", "[ui][route]") {
    Screens const screens;
    auto const refused = morph::ui::parseRoute("lims.sample?%zzId=1", screens);
    REQUIRE_FALSE(refused.has_value());
    CHECK_THAT(refused.error(), Catch::Matchers::ContainsSubstring("a param name that does not decode"));
}
