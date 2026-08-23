// SPDX-License-Identifier: Apache-2.0

#include <morph/util/quantity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

// ---------------------------------------------------------------------------
// A rich unit system: energy billing + a mass chain (g/kg/t) + a non-ratio
// pair (celsius/fahrenheit) exercised by a user-provided `convert`.
// ---------------------------------------------------------------------------
namespace qt {

enum class U : std::uint16_t {
    scalar,
    watt,
    kilowatt,
    hour,
    kilowatt_hour,
    euro,
    euro_per_kwh,
    gram,
    kilogram,
    tonne,
    celsius,
    fahrenheit,
    yen,  // zero-decimal currency: no fractional subunit.
};

}  // namespace qt

template <>
struct morph::units::UnitTraits<qt::U> {
    static constexpr morph::units::UnitMeta meta(qt::U unit) noexcept {
        switch (unit) {
            case qt::U::scalar: return {"scalar", "", 3};
            case qt::U::watt: return {"watt", "W", 1};
            case qt::U::kilowatt: return {"kilowatt", "kW", 3};
            case qt::U::hour: return {"hour", "h", 2};
            case qt::U::kilowatt_hour: return {"kwh", "kWh", 3};
            case qt::U::euro: return {"euro", "EUR", 2};
            case qt::U::euro_per_kwh: return {"eur_per_kwh", "EUR/kWh", 4};
            case qt::U::gram: return {"g", "g", 3};
            case qt::U::kilogram: return {"kg", "kg", 3};
            case qt::U::tonne: return {"t", "t", 3};
            case qt::U::celsius: return {"celsius", "C", 1};
            case qt::U::fahrenheit: return {"fahrenheit", "F", 1};
            case qt::U::yen: return {"yen", "JPY", 0};
            default:               return {"?", "?", 3};
        }
    }

    static constexpr std::array<morph::units::UnitRelation<qt::U>, 3> relations{{
        {qt::U::watt, qt::U::kilowatt, Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}},
        {qt::U::gram, qt::U::kilogram, Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}},
        {qt::U::kilogram, qt::U::tonne, Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}},
    }};

    // User-provided non-ratio conversion (a pair with no UnitRelation entry).
    // Templated on declared decimals so it needs no mid-class `meta()` call.
    template <std::uint32_t DIn, std::uint32_t DOut>
    static void convert(const morph::units::Quantity<qt::U::celsius, DIn>& in,
                        morph::units::Quantity<qt::U::fahrenheit, DOut>& out) {
        if (in.value()) {
            out = morph::units::Quantity<qt::U::fahrenheit, DOut>::fromDouble(in.value()->toDouble() * 9.0 / 5.0 +
                                                                             32.0);
        } else {
            out = morph::units::Quantity<qt::U::fahrenheit, DOut>{};
        }
    }
};

namespace qt {

consteval U operator*(U a, U b) {
    if (a == U::scalar) {
        return b;
    }
    if (b == U::scalar) {
        return a;
    }
    if (a == U::kilowatt && b == U::hour) {
        return U::kilowatt_hour;
    }
    if (a == U::hour && b == U::kilowatt) {
        return U::kilowatt_hour;
    }
    if (a == U::kilowatt_hour && b == U::euro_per_kwh) {
        return U::euro;
    }
    if (a == U::euro_per_kwh && b == U::kilowatt_hour) {
        return U::euro;
    }
    throw "unsupported unit product";
}

consteval U operator/(U a, U b) {
    if (b == U::scalar) {
        return a;
    }
    if (a == b) {
        return U::scalar;
    }
    if (a == U::euro && b == U::kilowatt_hour) {
        return U::euro_per_kwh;
    }
    if (a == U::kilowatt_hour && b == U::hour) {
        return U::kilowatt;
    }
    throw "unsupported unit quotient";
}

}  // namespace qt

using morph::units::NamedQuantity;
using morph::units::Quantity;

using Watt = Quantity<qt::U::watt>;
using Kilowatt = Quantity<qt::U::kilowatt>;
using Hours = Quantity<qt::U::hour>;
using KilowattHour = Quantity<qt::U::kilowatt_hour>;
using Euro = Quantity<qt::U::euro>;
using Gram = Quantity<qt::U::gram>;
using Kilogram = Quantity<qt::U::kilogram>;
using Tonne = Quantity<qt::U::tonne>;
using Celsius = Quantity<qt::U::celsius>;
using Fahrenheit = Quantity<qt::U::fahrenheit>;
using Yen = Quantity<qt::U::yen>;  // declaredDecimals defaults to 0.
using Tariff = NamedQuantity<"tariff", qt::U::euro_per_kwh>;
using StandingCharge = NamedQuantity<"standing", qt::U::euro>;

// Compile-time facts.
static_assert(morph::units::isQuantity<Kilowatt>);
static_assert(!morph::units::isQuantity<Rational>);
static_assert(morph::units::HasUnitRelations<qt::U>);
static_assert(Kilowatt::unit == qt::U::kilowatt);
static_assert(Kilowatt::declaredDecimals == 3);
// Zero declared decimals is legal (a zero-decimal currency like JPY/KRW):
// the declared-decimals floor is 0, not 1. Yen's UnitMeta::defaultDecimals
// is 0 and Quantity<U> defaults DeclaredDecimals from it.
static_assert(Yen::declaredDecimals == 0);
static_assert(std::same_as<Yen, Quantity<qt::U::yen, 0>>);
static_assert(std::same_as<decltype(std::declval<Kilowatt>() * std::declval<Hours>()), KilowattHour>);
static_assert(std::same_as<decltype(std::declval<KilowattHour>() / std::declval<Hours>()), Kilowatt>);
static_assert(std::same_as<decltype(std::declval<KilowattHour>() / std::declval<KilowattHour>()), Quantity<qt::U::scalar>>);

TEST_CASE("Quantity::construction and access", "[quantity]") {
    Kilowatt empty;
    CHECK_FALSE(empty.hasValue());
    CHECK_FALSE(empty.value().has_value());

    auto engaged = Kilowatt::fromDouble(2.5);
    CHECK(engaged.hasValue());
    CHECK(engaged.value().has_value());
    CHECK((*engaged).toDouble() == 2.5);
    CHECK(engaged.value_or(Rational{Numerator{9}, Denominator{1}, DecimalPlaces{3}}).toDouble() == 2.5);
    CHECK(empty.value_or(Rational{Numerator{9}, Denominator{1}, DecimalPlaces{3}}).toDouble() == 9.0);

    // fromOptional round-trips both states.
    CHECK(Kilowatt::fromOptional(std::nullopt).hasValue() == false);
    CHECK(Kilowatt::fromOptional(Rational{Numerator{1}, Denominator{1}, DecimalPlaces{3}}).hasValue());

    // fromDouble is empty only for non-finite / out-of-range input.
    CHECK_FALSE(Kilowatt::fromDouble(std::numeric_limits<double>::quiet_NaN()).hasValue());
    CHECK_FALSE(Kilowatt::fromDouble(1e300).hasValue());
}

TEST_CASE("Quantity::declared vs actual precision", "[quantity]") {
    using PreciseMass = Quantity<qt::U::kilogram, 5>;
    static_assert(PreciseMass::declaredDecimals == 5);
    CHECK(PreciseMass::declaredPrecision().value == 5);
    CHECK(Kilogram::unitMeta().id == "kg");

    auto value = Kilogram::fromDouble(1.5);
    auto retagged = value.withDecimalPlaces(DecimalPlaces{7});
    CHECK((*retagged).decimalPlaces.value == 7);
    CHECK((*value.atDeclaredPrecision()).decimalPlaces.value == 3);

    // Cross-precision converting constructor keeps value + tag.
    PreciseMass precise{value};
    CHECK((*precise).toDouble() == 1.5);

    // No-op on empty.
    Kilogram empty;
    CHECK_FALSE(empty.withDecimalPlaces(DecimalPlaces{4}).hasValue());
    CHECK_FALSE(empty.atDeclaredPrecision().hasValue());
}

TEST_CASE("Quantity::arithmetic and empty propagation", "[quantity]") {
    auto a = Kilowatt::fromDouble(2.0);
    auto b = Kilowatt::fromDouble(3.0);
    CHECK((*(a + b)).toDouble() == 5.0);
    CHECK((*(b - a)).toDouble() == 1.0);
    CHECK((*(-a)).toDouble() == -2.0);
    CHECK((*(a * Rational{Numerator{3}, Denominator{1}, DecimalPlaces{3}})).toDouble() == 6.0);
    CHECK((*(Rational{Numerator{3}, Denominator{1}, DecimalPlaces{3}} * a)).toDouble() == 6.0);
    CHECK((*(b / Rational{Numerator{2}, Denominator{1}, DecimalPlaces{3}})).toDouble() == 1.5);

    // Cross-unit algebra.
    auto power = Kilowatt::fromDouble(2.0);
    auto time = Hours::fromDouble(3.0);
    auto energy = power * time;
    CHECK((*energy).toDouble() == 6.0);
    CHECK((*(energy / time)).toDouble() == 2.0);

    // Empty propagation.
    Kilowatt empty;
    CHECK_FALSE((empty + b).hasValue());
    CHECK_FALSE((b + empty).hasValue());
    CHECK_FALSE((empty - b).hasValue());
    CHECK_FALSE((-empty).hasValue());
    CHECK_FALSE((empty * time).hasValue());
    CHECK_FALSE((empty * Rational{Numerator{2}, Denominator{1}, DecimalPlaces{3}}).hasValue());
    CHECK_FALSE((empty / Rational{Numerator{2}, Denominator{1}, DecimalPlaces{3}}).hasValue());

    // Division by zero yields empty.
    auto zero = Hours::fromDouble(0.0);
    CHECK_FALSE((energy / zero).hasValue());
    CHECK_FALSE((b / Rational{Numerator{0}, Denominator{1}, DecimalPlaces{3}}).hasValue());
}

TEST_CASE("Quantity::empty in every operand position", "[quantity]") {
    auto engaged = KilowattHour::fromDouble(6.0);
    auto time = Hours::fromDouble(2.0);
    KilowattHour emptyEnergy;
    Hours emptyTime;
    auto const two = Rational{Numerator{2}, Denominator{1}, DecimalPlaces{3}};

    // Same-unit +/- with the empty operand on the right (left engaged).
    CHECK_FALSE((engaged + emptyEnergy).hasValue());
    CHECK_FALSE((engaged - emptyEnergy).hasValue());

    // Cross-unit * and / with the empty operand in each position.
    CHECK_FALSE((emptyEnergy / time).hasValue());   // lhs empty
    CHECK_FALSE((engaged / emptyTime).hasValue());  // rhs empty
    auto power = Kilowatt::fromDouble(2.0);
    Kilowatt emptyPower;
    CHECK_FALSE((emptyPower * time).hasValue());  // lhs empty
    CHECK_FALSE((power * emptyTime).hasValue());  // rhs empty

    // Scalar * / with an empty quantity.
    CHECK_FALSE((emptyEnergy * two).hasValue());
    CHECK_FALSE((emptyEnergy / two).hasValue());
    CHECK_FALSE((-emptyEnergy).hasValue());
}

TEST_CASE("Quantity::comparison", "[quantity]") {
    auto one = Kilowatt::fromDouble(1.0);
    auto two = Kilowatt::fromDouble(2.0);
    Kilowatt empty;

    CHECK(one == Kilowatt::fromDouble(1.0));
    CHECK(empty == Kilowatt{});
    CHECK_FALSE(empty == one);
    CHECK_FALSE(one == empty);
    CHECK(one != two);

    // Ignores the precision tag.
    CHECK(one == Quantity<qt::U::kilowatt, 6>::fromDouble(1.0));

    // Ordering (both engaged).
    CHECK(two > one);
    CHECK(one < two);
    CHECK(one <= Kilowatt::fromDouble(1.0));
    CHECK(two >= one);

    // Ordering with an empty operand (in either position) throws.
    CHECK_THROWS_AS((void)(empty < one), std::logic_error);
    CHECK_THROWS_AS((void)(one < empty), std::logic_error);
    CHECK_THROWS_AS((void)(empty <=> empty), std::logic_error);
}

TEST_CASE("Quantity::formatting", "[quantity]") {
    CHECK(std::format("{}", Euro::fromDouble(1.36)) == "1.36EUR");
    CHECK(std::format("{}", KilowattHour::fromDouble(6.0)) == "6kWh");
    CHECK(std::format("{}", Quantity<qt::U::scalar>::fromDouble(0.3)) == "0.3");
    CHECK(std::format("{}", Kilowatt{}) == "N/AkW");
    CHECK(std::format("{}", Tariff::fromDouble(0.3)) == "0.3EUR/kWh");
}

TEST_CASE("Quantity::zero-decimal currency (declaredDecimals == 0)", "[quantity]") {
    // A zero-decimal currency (JPY/KRW): fromDouble rounds to the nearest
    // whole unit at DeclaredDecimals == 0, and formatting shows no fractional
    // digit or decimal point at all.
    auto const price = Yen::fromDouble(1200.0);
    REQUIRE(price.hasValue());
    CHECK(price.value()->getDecimalPlaces() == DecimalPlaces{0});
    CHECK(std::format("{}", price) == "1200JPY");

    // fromDouble rounds a fractional input half-away-from-zero to the nearest
    // whole yen (no fractional subunit exists to carry the remainder).
    CHECK(std::format("{}", Yen::fromDouble(1200.6)) == "1201JPY");
    CHECK(std::format("{}", Yen::fromDouble(1200.4)) == "1200JPY");

    // Empty still formats as N/A + unit, same as any other Quantity.
    CHECK(std::format("{}", Yen{}) == "N/AJPY");

    // Arithmetic between two dp-0 values stays at dp 0.
    auto const sum = Yen::fromDouble(500.0) + Yen::fromDouble(700.0);
    REQUIRE(sum.hasValue());
    CHECK(sum.value()->getDecimalPlaces() == DecimalPlaces{0});
    CHECK(std::format("{}", sum) == "1200JPY");

    // Wire round-trip: the payload is the nullable Rational, same shape as
    // any other Quantity, with "dp":0 preserved rather than bumped to 1.
    auto const written = glz::write_json(price);
    REQUIRE(written.has_value());
    CHECK(*written == R"({"num":1200,"den":1,"dp":0})");

    Yen restored{};
    REQUIRE_FALSE(glz::read_json(restored, *written));
    CHECK(restored == price);
    CHECK(restored.value()->getDecimalPlaces() == DecimalPlaces{0});
}

TEST_CASE("formatRationalDecimal - exact decimal rendering (no double path)", "[quantity][format]") {
    using morph::units::detail::formatRationalDecimal;

    // Terminating decimals render exactly regardless of precision.
    CHECK(formatRationalDecimal(Rational{Numerator{1}, Denominator{8}, DecimalPlaces{3}}) == "0.125");
    CHECK(formatRationalDecimal(Rational{Numerator{1}, Denominator{4}, DecimalPlaces{4}}) == "0.25");
    CHECK(formatRationalDecimal(Rational{Numerator{2}, Denominator{1}, DecimalPlaces{3}}) == "2");
    CHECK(formatRationalDecimal(Rational{Numerator{136}, Denominator{100}, DecimalPlaces{2}}) == "1.36");

    // Non-terminating decimals: round-half-away-from-zero at DecimalPlaces.
    // 1/3 at 18 places is eighteen 3s (no trailing 5 leaking in from a double).
    CHECK(formatRationalDecimal(Rational{Numerator{1}, Denominator{3}, DecimalPlaces{18}}) ==
          "0.333333333333333333");
    // 2/3 at 4 places rounds up: 0.66666.. -> 0.6667.
    CHECK(formatRationalDecimal(Rational{Numerator{2}, Denominator{3}, DecimalPlaces{4}}) == "0.6667");
    // 1/6 = 0.16666.. at 3 places rounds to 0.167.
    CHECK(formatRationalDecimal(Rational{Numerator{1}, Denominator{6}, DecimalPlaces{3}}) == "0.167");

    // Exact half rounds away from zero.
    CHECK(formatRationalDecimal(Rational{Numerator{5}, Denominator{1000}, DecimalPlaces{2}}) == "0.01");

    // Large exact integers survive without the 2^53 double-mantissa error.
    // 9007199254740993 == 2^53 + 1 (the classic non-representable double).
    CHECK(formatRationalDecimal(
              Rational{Numerator{9007199254740993}, Denominator{1}, DecimalPlaces{3}}) == "9007199254740993");

    // Negatives: rounding is symmetric (away from zero) and the sign is kept.
    CHECK(formatRationalDecimal(Rational{Numerator{-1}, Denominator{3}, DecimalPlaces{18}}) ==
          "-0.333333333333333333");
    CHECK(formatRationalDecimal(Rational{Numerator{-2}, Denominator{3}, DecimalPlaces{4}}) == "-0.6667");
    CHECK(formatRationalDecimal(Rational{Numerator{-42}, Denominator{10}, DecimalPlaces{3}}) == "-4.2");

    // Rounding can carry into a larger integer part.
    CHECK(formatRationalDecimal(Rational{Numerator{9999}, Denominator{10000}, DecimalPlaces{3}}) == "1");
}

TEST_CASE("toDecimalString - the exact decimal without the unit", "[quantity][format]") {
    using morph::units::toDecimalString;
    using morph::units::toString;

    using Scalar = Quantity<qt::U::scalar>;  // empty display text

    SECTION("engaged: the decimal alone, no unit suffix") {
        // The same trimmed exact decimal the formatter prints, minus the unit.
        CHECK(toDecimalString(Kilowatt{Rational{Numerator{52}, Denominator{10}, DecimalPlaces{1}}}) == "5.2");
        CHECK(toDecimalString(KilowattHour{Rational{Numerator{6}, Denominator{1}, DecimalPlaces{3}}}) == "6");
        CHECK(toDecimalString(Euro{Rational{Numerator{136}, Denominator{100}, DecimalPlaces{2}}}) == "1.36");
        CHECK(toDecimalString(Celsius{Rational{Numerator{-42}, Denominator{10}, DecimalPlaces{3}}}) == "-4.2");

        // The unit is genuinely absent, not merely invisible at the end.
        CHECK(toDecimalString(Kilowatt{Rational{Numerator{52}, Denominator{10}, DecimalPlaces{1}}})
                  .find("kW") == std::string::npos);
    }

    SECTION("empty renders \"N/A\", not an empty string") {
        // The deliberate choice: "N/A" is what keeps the toString identity
        // below total for the empty case too.
        CHECK(toDecimalString(Kilowatt{}) == "N/A");
        CHECK(toDecimalString(Yen{}) == "N/A");
        CHECK(toDecimalString(Scalar{}) == "N/A");
        CHECK_FALSE(toDecimalString(Kilowatt{}).empty());

        // A view wanting a blank cell supplies that policy itself -- this is
        // the whole of what `examples/lims`' `valueText<Q>` does.
        Kilowatt const absent{};
        CHECK((absent.hasValue() ? toDecimalString(absent) : std::string{}).empty());
    }

    SECTION("identity: toString == toDecimalString + unitMeta().display") {
        auto const holds = [](const auto& quantity) {
            using Q = std::remove_cvref_t<decltype(quantity)>;
            CHECK(toString(quantity) == toDecimalString(quantity) + std::string{Q::unitMeta().display});
            // ... and the formatter is that same text again (one print path).
            CHECK(std::format("{}", quantity) ==
                  toDecimalString(quantity) + std::string{Q::unitMeta().display});
        };

        holds(Kilowatt{Rational{Numerator{52}, Denominator{10}, DecimalPlaces{1}}});
        holds(Kilowatt{});                                                        // empty, non-empty display
        holds(Yen{Rational{Numerator{1200}, Denominator{1}, DecimalPlaces{0}}});  // multi-char display
        holds(Yen{});
        holds(Scalar{Rational{Numerator{3}, Denominator{10}, DecimalPlaces{1}}});  // empty display
        holds(Scalar{});                                                           // empty + empty display

        // The identity above is satisfied vacuously by any pair of renderers
        // that agree, so pin the literal texts on both sides of it as well.
        Kilowatt const power{Rational{Numerator{52}, Denominator{10}, DecimalPlaces{1}}};
        CHECK(toDecimalString(power) == "5.2");
        CHECK(toString(power) == "5.2kW");
        CHECK(toDecimalString(Kilowatt{}) == "N/A");
        CHECK(toString(Kilowatt{}) == "N/AkW");
    }

    SECTION("stays exact -- not either std::format-on-payload path") {
        // 2^53 + 1: the classic value a double cannot represent.
        Euro const big{Rational{Numerator{9007199254740993}, Denominator{1}, DecimalPlaces{2}}};
        CHECK(toDecimalString(big) == "9007199254740993");
        // Rational's own non-empty spec goes through toDouble() and loses it.
        CHECK(std::format("{:.0f}", *big.value()) == "9007199254740992");

        // ... and its empty spec prints the fraction, not a decimal.
        Euro const fraction{Rational{Numerator{12}, Denominator{5}, DecimalPlaces{3}}};
        CHECK(std::format("{}", *fraction.value()) == "12/5");
        CHECK(toDecimalString(fraction) == "2.4");
    }
}

TEST_CASE("Quantity::conversion - ratio, chaining, reverse", "[quantity]") {
    // Direct ratio (W -> kW) and reverse (kW -> W).
    CHECK((*static_cast<Kilowatt>(Watt::fromDouble(2000.0))).toDouble() == 2.0);
    CHECK((*static_cast<Watt>(Kilowatt::fromDouble(2.0))).toDouble() == 2000.0);

    // Chaining (g -> t through kg) and reverse (t -> g).
    CHECK((*static_cast<Tonne>(Gram::fromDouble(2000000.0))).toDouble() == 2.0);
    CHECK((*static_cast<Gram>(Tonne::fromDouble(2.0))).toDouble() == 2000000.0);

    // Empty converts to empty (no convert call).
    CHECK_FALSE(static_cast<Kilowatt>(Watt{}).hasValue());

    // Mixed-unit addition converts the right operand first.
    auto total = Kilowatt::fromDouble(2.0) + Watt::fromDouble(500.0);
    CHECK((*total).toDouble() == 2.5);
    auto diff = Kilowatt::fromDouble(2.0) - Watt::fromDouble(500.0);
    CHECK((*diff).toDouble() == 1.5);
}

TEST_CASE("Quantity::conversion - user override (non-ratio)", "[quantity]") {
    auto boiling = static_cast<Fahrenheit>(Celsius::fromDouble(100.0));
    CHECK((*boiling).toDouble() == 212.0);
    CHECK_FALSE(static_cast<Fahrenheit>(Celsius{}).hasValue());
}

TEST_CASE("Quantity::unitAlternatives derived from relations", "[quantity]") {
    auto kgAlts = Kilogram::unitAlternatives();
    // kg touches two relations: g<->kg and kg<->t.
    CHECK(kgAlts.size() == 2);
    CHECK(kgAlts[0].unit == qt::U::gram);
    CHECK(kgAlts[0].num == 1);
    CHECK(kgAlts[0].den == 1000);
    CHECK(kgAlts[1].unit == qt::U::tonne);
    CHECK(kgAlts[1].num == 1000);
    CHECK(kgAlts[1].den == 1);
    CHECK(Euro::unitAlternatives().empty());
}

TEST_CASE("equation() - degenerate roots", "[quantity][equation]") {
    // Empty -> single N/A.
    CHECK(Kilowatt{}.equation() == std::vector<std::string>{"N/A"});
    // Computed-but-empty -> still N/A.
    CHECK((Kilowatt{} + Kilowatt::fromDouble(1.0)).equation() == std::vector<std::string>{"N/A"});
    // Bare leaf -> its value.
    CHECK(KilowattHour::fromDouble(6.0).equation() == std::vector<std::string>{"6"});
    // Named root -> just the name.
    CHECK(KilowattHour::fromDouble(6.0).named("load").equation() == std::vector<std::string>{"\"load\""});
    CHECK(Tariff::fromDouble(0.3).equation() == std::vector<std::string>{"\"tariff\""});
    // Unnamed conversion root -> the converted value.
    CHECK(static_cast<Kilowatt>(Watt::fromDouble(2000.0)).equation() == std::vector<std::string>{"2"});
}

TEST_CASE("equation() - engaged payload without a recorded node", "[quantity][equation]") {
    // A payload populated directly (bypassing the value constructors) has no
    // derivation node; equation() falls back to the formatted value.
    Kilowatt manual;
    manual.payload = Rational{Numerator{5}, Denominator{1}, DecimalPlaces{3}};
    CHECK(manual.equation() == std::vector<std::string>{"5"});
}

TEST_CASE("equation() - operand without a node contributes its raw value", "[quantity][equation]") {
    // `manual` has an engaged payload but no derivation node; as an operand its
    // value is read straight from the recorded step (the node-less fallback).
    KilowattHour manual;
    manual.payload = Rational{Numerator{6}, Denominator{1}, DecimalPlaces{3}};
    auto other = KilowattHour::fromDouble(2.0).named("b");
    auto sum = manual + other;
    auto const lines = sum.equation();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == R"(6 + "b")");
    CHECK(lines[1] == "    = 6 + 2");
    CHECK(lines[2] == "    = 8");
}

TEST_CASE("equation() - worked formula with inlined values", "[quantity][equation]") {
    auto heater = static_cast<Kilowatt>(Watt::fromDouble(2000.0)).named("heater");
    auto runtime = Hours::fromDouble(3.0);
    auto solar = KilowattHour::fromDouble(1.8).named("solar");
    Tariff tariff{Tariff::fromDouble(0.30)};
    StandingCharge standing{StandingCharge::fromDouble(0.10)};

    auto consumption = heater * runtime;
    auto grid_cost = consumption * tariff;
    auto savings = solar * tariff;
    auto net_cost = grid_cost - savings + standing;

    CHECK(std::format("{}", net_cost) == "1.36EUR");
    auto const lines = net_cost.equation();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == R"("heater" * 3 * "tariff" - "solar" * "tariff" + "standing")");
    CHECK(lines[1] == "    = 2 * 3 * 0.3 - 1.8 * 0.3 + 0.1");
    CHECK(lines[2] == "    = 1.36");
}

TEST_CASE("equation() - reused value earns one placeholder", "[quantity][equation]") {
    auto a = KilowattHour::fromDouble(6.0).named("a");
    auto b = KilowattHour::fromDouble(1.8).named("b");
    auto diff = a - b;                  // unnamed, computed, used twice
    auto share = diff / (diff + b);

    auto const lines = share.equation();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == R"(c1 / (c1 + "b"))");
    CHECK(lines[1] == "    = 4.2 / (4.2 + 1.8)");
    CHECK(lines[2] == "    = 0.7");
    CHECK(lines[3] == R"(where c1 = "a" - "b" = 6 - 1.8 = 4.2)");
}

TEST_CASE("equation() - reused raw leaf earns one placeholder", "[quantity][equation]") {
    auto x = KilowattHour::fromDouble(4.0);  // unnamed leaf, used twice
    auto result = x + x;
    auto const lines = result.equation();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "c1 + c1");
    CHECK(lines[1] == "    = 4 + 4");
    CHECK(lines[2] == "    = 8");
    CHECK(lines[3] == "where c1 = 4");
}

TEST_CASE("equation() - associativity-driven parentheses", "[quantity][equation]") {
    auto a = KilowattHour::fromDouble(6.0).named("a");
    auto b = KilowattHour::fromDouble(2.0).named("b");
    auto c = KilowattHour::fromDouble(1.0).named("c");

    // a - (b + c): right operand of '-' at equal precedence keeps parens.
    CHECK((a - (b + c)).equation()[0] == R"("a" - ("b" + "c"))");
    // (a - b) - c: left-associative chain needs no parens.
    CHECK(((a - b) - c).equation()[0] == R"("a" - "b" - "c")");
    // a - b + c: mixed left-associative, no parens.
    CHECK(((a - b) + c).equation()[0] == R"("a" - "b" + "c")");
    // a + (b - c): equal precedence under '+' (associative) needs no parens.
    CHECK((a + (b - c)).equation()[0] == R"("a" + "b" - "c")");

    // Parentheses around a lower-precedence operand of '*' (using a valid
    // product, kW * h -> kWh): left operand and right operand cases.
    auto p = Kilowatt::fromDouble(2.0).named("p");
    auto q = Kilowatt::fromDouble(3.0).named("q");
    auto t = Hours::fromDouble(4.0).named("t");
    CHECK(((p + q) * t).equation()[0] == R"(("p" + "q") * "t")");
    CHECK((t * (p + q)).equation()[0] == R"("t" * ("p" + "q"))");

    // e / (p * t): right operand of '/' is itself a product (equal precedence),
    // so it is parenthesised (kW*h -> kWh, then kWh / kWh -> scalar).
    auto e = KilowattHour::fromDouble(6.0).named("e");
    CHECK((e / (p * t)).equation()[0] == R"("e" / ("p" * "t"))");
}

TEST_CASE("equation() - unary negation of a sub-expression parenthesises", "[quantity][equation]") {
    auto a = KilowattHour::fromDouble(6.0).named("a");
    auto b = KilowattHour::fromDouble(2.0).named("b");
    // Negating a sum parenthesises it; negating an atom does not.
    CHECK((-(a + b)).equation()[0] == R"(-("a" + "b"))");
    CHECK((-a).equation()[0] == R"(-"a")");
}

TEST_CASE("equation() - two reused values produce a two-line legend", "[quantity][equation]") {
    auto a = KilowattHour::fromDouble(6.0).named("a");
    auto b = KilowattHour::fromDouble(2.0).named("b");
    auto p = a - b;  // reused twice
    auto q = a + b;  // reused twice
    auto result = p - q + p - q;
    auto const lines = result.equation();
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "c1 - c2 + c1 - c2");
    CHECK(lines[3] == R"(where c1 = "a" - "b" = 6 - 2 = 4)");
    CHECK(lines[4] == R"(      c2 = "a" + "b" = 6 + 2 = 8)");
}

TEST_CASE("equation() - unary negation and scalar scaling", "[quantity][equation]") {
    auto energy = KilowattHour::fromDouble(3.0).named("e");
    CHECK((-energy).equation()[0] == R"(-"e")");

    auto scaled = KilowattHour::fromDouble(3.0) * Rational{Numerator{2}, Denominator{1}, DecimalPlaces{3}};
    auto const lines = scaled.equation();
    CHECK(lines[0] == "3 * 2");
    CHECK(lines[2] == "    = 6");
}

TEST_CASE("equation() - unnamed conversion inlined in a formula", "[quantity][equation]") {
    // Leaving the converted heater unnamed writes its converted value into the
    // formula (the conversion node renders as its result, both symbolically and
    // when substituted).
    auto heater = static_cast<Kilowatt>(Watt::fromDouble(2000.0));  // unnamed conversion
    auto runtime = Hours::fromDouble(3.0).named("t");
    auto consumption = heater * runtime;
    auto const lines = consumption.equation();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == R"(2 * "t")");
    CHECK(lines[1] == R"(    = 2 * 3)");
    CHECK(lines[2] == "    = 6");
}

TEST_CASE("NamedQuantity slices to a plain Quantity", "[quantity]") {
    Tariff tariff{Tariff::fromDouble(0.30)};
    Quantity<qt::U::euro_per_kwh> plain = tariff;  // slice
    CHECK((*plain).toDouble() == 0.3);
    CHECK(std::format("{}", tariff) == "0.3EUR/kWh");

    // NamedQuantity default-constructs empty.
    Tariff blank;
    CHECK_FALSE(blank.hasValue());
}
