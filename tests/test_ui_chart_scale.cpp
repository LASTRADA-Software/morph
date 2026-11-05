// SPDX-License-Identifier: Apache-2.0

// A chart's axis arithmetic, exact: the padded range of its values, the ticks that label it, and the overflows that
// leave a chart without either.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <format>
#include <limits>
#include <morph/ui/chart_scale.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ui = morph::ui;
namespace math = morph::math;

namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

math::Rational whole(std::int64_t value) { return math::Rational{value, math::DecimalPlaces{0}}; }

math::Rational fraction(std::int64_t numerator, std::int64_t denominator, std::uint32_t places) {
    return math::Rational{math::Numerator{numerator}, math::Denominator{denominator}, math::DecimalPlaces{places}};
}

// A range's ends in the exact `n/d` form.
std::string text(std::optional<ui::ChartRange> const& range) {
    return range ? std::format("{} .. {}", range->low, range->high) : std::string{"none"};
}

// The ticks' positions in the exact `n/d` form, then their places.
std::string text(std::optional<ui::ChartTicks> const& ticks) {
    if (!ticks) {
        return "none";
    }
    std::string out;
    for (math::Rational const& value : ticks->values) {
        out += (out.empty() ? "" : " ") + std::format("{}", value);
    }
    return std::format("{} @{}", out, ticks->places);
}

}  // namespace

TEST_CASE("A chart's power of ten is exact either side of one", "[ui][chart]") {
    CHECK(ui::chartPowerOfTen(3) == whole(1000));
    CHECK(ui::chartPowerOfTen(3).decimalPlaces.value == 0);
    CHECK(ui::chartPowerOfTen(0) == whole(1));
    CHECK(ui::chartPowerOfTen(-2) == fraction(1, 100, 2));
    CHECK(ui::chartPowerOfTen(-2).decimalPlaces.value == 2);
}

TEST_CASE("A chart's floor and ceiling round toward the right infinity on each side of zero", "[ui][chart]") {
    CHECK(ui::chartFloor(fraction(7, 2, 1)) == 3);
    CHECK(ui::chartFloor(fraction(-7, 2, 1)) == -4);
    CHECK(ui::chartFloor(whole(-3)) == -3);
    CHECK(ui::chartCeil(fraction(7, 2, 1)) == 4);
    CHECK(ui::chartCeil(fraction(-7, 2, 1)) == -3);
    CHECK(ui::chartCeil(whole(3)) == 3);
}

TEST_CASE("A chart's range pads its values by a twentieth of their span at each end", "[ui][chart]") {
    std::vector<math::Rational> const values{whole(40), whole(0), whole(100)};
    CHECK(text(ui::chartRange(values, 0)) == "-5 .. 105");
}

TEST_CASE("A chart with no values spans zero to one", "[ui][chart]") {
    CHECK(text(ui::chartRange({}, 2)) == "0 .. 1");
}

TEST_CASE("A chart whose values are all equal widens by one unit in the last place shown", "[ui][chart]") {
    std::vector<math::Rational> const values{whole(5), whole(5)};
    CHECK(text(ui::chartRange(values, 2)) == "499/100 .. 501/100");
    // Places beyond what a rational holds widen by its finest unit rather than by nothing.
    CHECK(text(ui::chartRange(values, 30)) ==
          "4999999999999999999/1000000000000000000 .. "
          "5000000000000000001/1000000000000000000");
}

TEST_CASE("A chart has no range when its span, padding or ends overflow", "[ui][chart]") {
    SECTION("the span") {
        std::vector<math::Rational> const values{whole(-kMax), whole(kMax)};
        CHECK(text(ui::chartRange(values, 0)) == "none");
    }
    SECTION("the padding") {
        // A twentieth of 10^-18 needs a denominator past the int64 range.
        std::vector<math::Rational> const values{whole(0), ui::chartPowerOfTen(-18)};
        CHECK(text(ui::chartRange(values, 18)) == "none");
    }
    SECTION("the upper end") {
        std::vector<math::Rational> const values{whole(kMax - 1), whole(kMax)};
        CHECK(text(ui::chartRange(values, 0)) == "none");
    }
    SECTION("the lower end") {
        std::vector<math::Rational> const values{whole(-kMax), whole(-kMax + 1)};
        CHECK(text(ui::chartRange(values, 0)) == "none");
    }
}

TEST_CASE("A chart's ticks take the largest 1-2-5 step that puts three on the axis", "[ui][chart]") {
    SECTION("whole steps") { CHECK(text(ui::chartTicks({.low = whole(-5), .high = whole(105)})) == "0 50 100 @0"); }
    SECTION("a step below one carries its places") {
        CHECK(text(ui::chartTicks({.low = fraction(1, 2, 1), .high = fraction(9, 10, 1)})) ==
              "1/2 3/5 7/10 4/5 9/10 @1");
    }
    SECTION("a negative axis") {
        CHECK(text(ui::chartTicks({.low = fraction(-9, 10, 1), .high = fraction(-1, 2, 1)})) ==
              "-9/10 -4/5 -7/10 -3/5 -1/2 @1");
    }
    SECTION("a step of two where five would put only two") {
        CHECK(text(ui::chartTicks({.low = whole(1), .high = whole(14)})) == "2 4 6 8 10 12 14 @0");
        CHECK(text(ui::chartTicks({.low = whole(1), .high = whole(9)})) == "2 4 6 8 @0");
    }
    SECTION("the widest axis stops at a step of 10^18") {
        auto const ticks = ui::chartTicks({.low = whole(0), .high = whole(kMax)});
        REQUIRE(ticks);
        CHECK(ticks->values.size() == 10);
        CHECK(ticks->values.back() == whole(9'000'000'000'000'000'000));
        CHECK(ticks->places == 0);
    }
}

TEST_CASE("A chart has no ticks on an empty, reversed, too narrow or overflowing axis", "[ui][chart]") {
    CHECK(text(ui::chartTicks({.low = whole(1), .high = whole(1)})) == "none");
    CHECK(text(ui::chartTicks({.low = whole(2), .high = whole(1)})) == "none");
    CHECK(text(ui::chartTicks({.low = whole(-kMax), .high = whole(kMax)})) == "none");
    // Two multiples of 10^-18 and no finer step to try.
    CHECK(text(ui::chartTicks({.low = whole(0), .high = ui::chartPowerOfTen(-18)})) == "none");
    // 50 over a step of 5 * 10^-18 overflows before a step putting three is found.
    CHECK(text(ui::chartTicks({.low = whole(50),
                               .high = fraction((50 * 100'000'000'000'000'000) + 1, 100'000'000'000'000'000, 17)})) ==
          "none");
}
