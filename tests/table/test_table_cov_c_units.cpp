// SPDX-License-Identifier: Apache-2.0
//
// The unit registry builder: what it refuses to add, what removing a unit leaves behind, and what a successor
// registry keeps.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/table/data_source.hpp>
#include <string>
#include <string_view>
#include <vector>

using namespace morph::table;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

UnitInfo unit(std::string id, std::string dimension, Rational toBase = Rational{1, DecimalPlaces{0}}) {
    return UnitInfo{
        .id = std::move(id), .symbol = {}, .dimension = std::move(dimension), .toBase = toBase, .decimals = 0};
}

}  // namespace

TEST_CASE("a unit is refused for each way its id, factor or dimension is unusable", "[covc][table][units]") {
    struct Refusal {
        UnitInfo unit;
        std::string message;
    };
    auto negative = unit("neg", "mass");
    negative.toBase.numerator = -1;
    auto zeroDenominator = unit("zero", "mass");
    zeroDenominator.toBase.denominator = 0;
    std::vector<Refusal> const refusals{
        {.unit = unit("", "mass"), .message = "has an id of 0 bytes, not 1 to 32"},
        {.unit = unit(std::string(33, 'x'), "mass"), .message = "has an id of 33 bytes"},
        {.unit = unit("a b", "mass"), .message = "has white space in its id"},
        {.unit = unit("1kg", "mass"), .message = "begins like a number"},
        {.unit = unit("-kg", "mass"), .message = "begins like a number"},
        {.unit = unit(".kg", "mass"), .message = "begins like a number"},
        {.unit = negative, .message = "has a factor that is not positive"},
        {.unit = zeroDenominator, .message = "has a factor that is not positive"},
        {.unit = unit("nodim", ""), .message = "names no dimension"},
    };
    for (auto const& refusal : refusals) {
        CAPTURE(refusal.unit.id);
        UnitRegistryBuilder builder;
        auto const added = builder.add(refusal.unit);
        REQUIRE_FALSE(added);
        CHECK(added.error().code == TableErrorCode::InvalidSpec);
        CHECK(added.error().message.contains(refusal.message));
    }
    UnitRegistryBuilder builder;
    REQUIRE(builder.add(unit("kg", "mass")));
    auto const twice = builder.add(unit("kg", "mass"));
    REQUIRE_FALSE(twice);
    CHECK(twice.error().message.contains("is registered already"));
}

TEST_CASE("removing a unit frees its id and drops it from its dimension", "[covc][table][units]") {
    UnitRegistryBuilder builder;
    REQUIRE(builder.add(unit("kg", "mass")));
    REQUIRE(builder.add(unit("g", "mass", Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}})));
    CHECK(builder.holds("mass"));
    CHECK_FALSE(builder.holds("length"));
    CHECK(builder.baseOf("mass") == "kg");
    CHECK_FALSE(builder.baseOf("length"));
    CHECK_FALSE(builder.remove(""));
    CHECK_FALSE(builder.remove("nope"));
    CHECK(builder.remove("kg"));
    CHECK_FALSE(builder.remove("kg"));
    CHECK_FALSE(builder.baseOf("mass"));
    CHECK(builder.holds("mass"));
    REQUIRE(builder.add(unit("kg", "mass")));
    auto const registry = std::move(builder).build();
    CHECK(registry->unitsOf("mass").size() == 2);
    CHECK(registry->unitsOf("length").empty());
    CHECK(registry->find("g"));
    CHECK_FALSE(registry->find("nope"));
}

TEST_CASE("a successor registry keeps its predecessor's units and lineage", "[covc][table][units]") {
    UnitRegistryBuilder first;
    REQUIRE(first.add(unit("kg", "mass")));
    auto const before = std::move(first).build();
    UnitRegistryBuilder next{before};
    CHECK(next.remove("kg"));
    REQUIRE(next.add(unit("m", "length")));
    auto const after = std::move(next).build();
    CHECK(after->lineage() == before->lineage());
    CHECK_FALSE(after->contains(*before->find("kg")));
    CHECK(before->contains(*before->find("kg")));
    CHECK(after->find("m"));
    UnitRegistryBuilder fromNothing{std::shared_ptr<UnitRegistry const>{}};
    auto const fromNothingBuilt = std::move(fromNothing).build();
    CHECK(fromNothingBuilt->size() == 1);
    CHECK(after->dimensions().size() >= 2);
}

namespace covunits {

enum class Unit : std::uint8_t { kg, g, mg, t, lb, l, ml, cl };

namespace {

std::string_view dimensionOf(Unit unit) {
    switch (unit) {
        case Unit::l:
        case Unit::ml:
        case Unit::cl:
            return "volume";
        case Unit::kg:
        case Unit::g:
        case Unit::mg:
        case Unit::t:
        case Unit::lb:
        default:
            return "mass";
    }
}

}  // namespace

}  // namespace covunits

template <>
struct morph::units::UnitTraits<covunits::Unit> {
    static constexpr morph::units::UnitMeta meta(covunits::Unit unit) noexcept {
        switch (unit) {
            case covunits::Unit::kg:
                return {.id = "kg", .display = "kg", .defaultDecimals = 3};
            case covunits::Unit::g:
                return {.id = "g", .display = "g", .defaultDecimals = 1};
            case covunits::Unit::mg:
                return {.id = "mg", .display = "mg", .defaultDecimals = 0};
            case covunits::Unit::t:
                return {.id = "t", .display = "t", .defaultDecimals = 6};
            case covunits::Unit::lb:
                return {.id = "lb", .display = "lb", .defaultDecimals = 2};
            case covunits::Unit::l:
                return {.id = "l", .display = "l", .defaultDecimals = 3};
            case covunits::Unit::ml:
                return {.id = "ml", .display = "ml", .defaultDecimals = 0};
            case covunits::Unit::cl:
            default:
                return {.id = "cl", .display = "cl", .defaultDecimals = 1};
        }
    }
    static constexpr std::array<morph::units::UnitRelation<covunits::Unit>, 6> relations{{
        {.from = covunits::Unit::cl, .to = covunits::Unit::ml, .fromTo = Rational{0, DecimalPlaces{0}}},
        {.from = covunits::Unit::ml,
         .to = covunits::Unit::l,
         .fromTo = Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}},
        {.from = covunits::Unit::g,
         .to = covunits::Unit::kg,
         .fromTo = Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}},
        {.from = covunits::Unit::mg,
         .to = covunits::Unit::g,
         .fromTo = Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}},
        {.from = covunits::Unit::g,
         .to = covunits::Unit::mg,
         .fromTo = Rational{Numerator{1000}, Denominator{1}, DecimalPlaces{0}}},
        {.from = covunits::Unit::kg,
         .to = covunits::Unit::t,
         .fromTo = Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}},
    }};
};

TEST_CASE("a unit enum's relations are walked in both directions to the dimension's base", "[covc][table][units]") {
    using covunits::Unit;
    UnitRegistryBuilder builder;
    std::vector<Unit> const all{Unit::kg, Unit::g, Unit::mg, Unit::t, Unit::lb, Unit::l, Unit::ml, Unit::cl};
    auto const added = addUnitsOf<Unit>(builder, all, &covunits::dimensionOf);
    REQUIRE_FALSE(added);
    CHECK(added.error().code == TableErrorCode::InvalidSpec);
    CHECK(added.error().column == "lb");
    CHECK(added.error().message.contains("has no relation to the base of mass"));
    auto const registry = std::move(builder).build();
    auto const factorOf = [&](std::string_view id) { return registry->info(*registry->find(id)).toBase; };
    CHECK(factorOf("kg") == Rational{1, DecimalPlaces{0}});
    CHECK(factorOf("g") == Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}});
    CHECK(factorOf("mg") == Rational{Numerator{1}, Denominator{1'000'000}, DecimalPlaces{6}});
    CHECK(factorOf("t") == Rational{1000, DecimalPlaces{0}});
    CHECK(factorOf("ml") == Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}});
    CHECK_FALSE(registry->find("lb"));
    CHECK_FALSE(registry->find("cl"));
}

TEST_CASE("a dimension the builder holds with no unit of factor one refuses every unit of it",
          "[covc][table][units]") {
    using covunits::Unit;
    UnitRegistryBuilder builder;
    REQUIRE(builder.add(UnitInfo{
        .id = "stone", .symbol = {}, .dimension = "mass", .toBase = Rational{14, DecimalPlaces{0}}, .decimals = 0}));
    std::vector<Unit> const mass{Unit::kg, Unit::g};
    auto const added = addUnitsOf<Unit>(builder, mass, &covunits::dimensionOf);
    REQUIRE_FALSE(added);
    CHECK(added.error().message.contains("has no relation to the base of mass"));

    UnitRegistryBuilder held;
    REQUIRE(held.add(UnitInfo{
        .id = "kg", .symbol = {}, .dimension = "mass", .toBase = Rational{1, DecimalPlaces{0}}, .decimals = 0}));
    std::vector<Unit> const rest{Unit::g, Unit::kg, Unit::t};
    REQUIRE(addUnitsOf<Unit>(held, rest, &covunits::dimensionOf));
    auto const heldBuilt = std::move(held).build();
    CHECK(heldBuilt->find("t"));
}
