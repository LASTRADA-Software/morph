// SPDX-License-Identifier: Apache-2.0
//
// The unit registry builder: what it refuses to add, what removing a unit leaves behind, and what a successor
// registry keeps.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/table/data_source.hpp>
#include <string>
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
        {unit("", "mass"), "has an id of 0 bytes, not 1 to 32"},
        {unit(std::string(33, 'x'), "mass"), "has an id of 33 bytes"},
        {unit("a b", "mass"), "has white space in its id"},
        {unit("1kg", "mass"), "begins like a number"},
        {unit("-kg", "mass"), "begins like a number"},
        {unit(".kg", "mass"), "begins like a number"},
        {negative, "has a factor that is not positive"},
        {zeroDenominator, "has a factor that is not positive"},
        {unit("nodim", ""), "names no dimension"},
    };
    for (auto const& refusal : refusals) {
        CAPTURE(refusal.unit.id);
        UnitRegistryBuilder builder;
        auto const added = builder.add(refusal.unit);
        REQUIRE_FALSE(added);
        CHECK(added.error().code == TableErrorCode::InvalidSpec);
        CHECK(added.error().message.find(refusal.message) != std::string::npos);
    }
    UnitRegistryBuilder builder;
    REQUIRE(builder.add(unit("kg", "mass")));
    auto const twice = builder.add(unit("kg", "mass"));
    REQUIRE_FALSE(twice);
    CHECK(twice.error().message.find("is registered already") != std::string::npos);
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
    CHECK(std::move(fromNothing).build()->size() == 1);
    CHECK(after->dimensions().size() >= 2);
}
