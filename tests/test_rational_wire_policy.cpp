// SPDX-License-Identifier: Apache-2.0
//
// Rational's wire round-trip, and the clamp fact it reports to whoever is
// decoding. Deciding what a clamp *means* is the decoding layer's job, not
// this type's -- see test_action_wire_rejection.cpp for that half.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <limits>
#include <morph/util/rational.hpp>
#include <string>

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
using morph::math::WireClampScope;

namespace {

constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

[[nodiscard]] std::string encode(const Rational& value) {
    std::string out;
    REQUIRE_FALSE(glz::write_json(value, out));
    return out;
}

[[nodiscard]] Rational decode(const std::string& json) {
    Rational value;
    REQUIRE_FALSE(glz::read_json(value, json));
    return value;
}

}  // namespace

TEST_CASE("A valid Rational round-trips through its own formatter unchanged", "[rational][wire]") {
    // The property the type owes on its own: whatever it can represent, it can
    // write and read back identically.
    const Rational values[] = {
        Rational{Numerator{0}, Denominator{1}, DecimalPlaces{0}},
        Rational{Numerator{5}, Denominator{2}, DecimalPlaces{2}},
        Rational{Numerator{-5}, Denominator{2}, DecimalPlaces{2}},
        Rational{Numerator{1}, Denominator{3}, DecimalPlaces{18}},
        Rational{Numerator{kMax}, Denominator{1}, DecimalPlaces{2}},
        Rational{Numerator{-kMax}, Denominator{kMax}, DecimalPlaces{9}},
    };

    for (const auto& original : values) {
        const auto reread = decode(encode(original));
        INFO("round-tripping " << original.numerator << "/" << original.denominator);
        CHECK(reread == original);
        CHECK(reread.numerator == original.numerator);
        CHECK(reread.denominator == original.denominator);
        CHECK(reread.decimalPlaces.value == original.decimalPlaces.value);
        // And re-encoding is byte-identical, so the round trip is a fixed point.
        CHECK(encode(reread) == encode(original));
    }
}

TEST_CASE("Round-tripping a valid value reports no clamp", "[rational][wire]") {
    const WireClampScope clamps;
    const auto reread = decode(encode(Rational{Numerator{4}, Denominator{8}, DecimalPlaces{2}}));

    // 4/8 canonicalises to 1/2 -- that is reduction, not clamping, and it
    // round-trips the same value.
    CHECK(reread.numerator == 1);
    CHECK(reread.denominator == 2);
    CHECK(clamps.clamped() == 0);
}

TEST_CASE("Wire::validate names exactly the values that cannot be represented", "[rational][wire]") {
    constexpr auto int64Min = std::numeric_limits<std::int64_t>::min();

    CHECK(Rational::Wire{.num = 5, .den = 2, .dp = 2}.validate());
    CHECK(Rational::Wire{.num = 4, .den = 8, .dp = 2}.validate());   // non-canonical, representable
    CHECK(Rational::Wire{.num = 1, .den = -2, .dp = 2}.validate());  // negative den, representable

    CHECK_FALSE(Rational::Wire{.num = 5, .den = 0, .dp = 2}.validate());
    CHECK_FALSE(Rational::Wire{.num = 5, .den = 2, .dp = morph::math::kMaxDecimalPlaces + 1}.validate());
    CHECK_FALSE(Rational::Wire{.num = int64Min, .den = 2, .dp = 2}.validate());
    CHECK_FALSE(Rational::Wire{.num = 5, .den = int64Min, .dp = 2}.validate());
}

TEST_CASE("Decoding an unrepresentable value still succeeds, and says so", "[rational][wire]") {
    // Decoding cannot fail -- that is the point. The value looks entirely
    // plausible afterwards, which is why the clamp has to be reported rather
    // than left for a downstream validate() that has nothing to notice.
    const WireClampScope clamps;
    const auto value = decode(R"({"num":5,"den":0,"dp":2})");

    CHECK(value.numerator == 5);
    CHECK(value.denominator == 1);
    CHECK(clamps.clamped() == 1);
}

TEST_CASE("Clamp counts nest without stealing the enclosing scope's", "[rational][wire]") {
    const WireClampScope outer;
    CHECK(outer.clamped() == 0);
    {
        const WireClampScope inner;
        (void)decode(R"({"num":5,"den":0,"dp":2})");
        CHECK(inner.clamped() == 1);
    }
    // The inner scope folds its count back into the outer one rather than
    // discarding it: a nested decode's clamp is still this decode's clamp.
    CHECK(outer.clamped() == 1);
}
