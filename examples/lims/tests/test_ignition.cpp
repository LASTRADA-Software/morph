// SPDX-License-Identifier: Apache-2.0
//
// Ignition tests: each stored as it is added or edited, with no separate save.

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <testkit/db_fixture.hpp>

#include "lims/core/errors.hpp"
#include "lims/dto/ignition_dto.hpp"
#include "lims/models/ignition_model.hpp"
#include "lims_test_support.hpp"

using morph::ladder::testkit::DbFixture;

namespace {

// An exact decimal, as a view holds it.
morph::math::Rational decimal(std::int64_t numerator, std::int64_t denominator, std::uint32_t places) {
    return morph::math::Rational{morph::math::Numerator{numerator}, morph::math::Denominator{denominator},
                                 morph::math::DecimalPlaces{places}};
}

morph::math::Rational mass(std::int64_t grams) { return decimal(grams * 1000, 1000, 3); }

}  // namespace

TEST_CASE("adding_and_recording_an_ignition_test_stores_it", "[lims][ignition]") {
    DbFixture const db;
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::IgnitionModel model;
    auto const added = model.execute(lims::AddIgnitionTest{});
    CHECK_FALSE(added.container.has_value());
    auto const recorded = model.execute(
        lims::RecordIgnitionValues{.id = added.id, .container = mass(100), .before = mass(299), .after = mass(223)});
    CHECK(recorded.id == added.id);
    // A fresh instance reads it, so it is the store that holds it and not this one.
    lims::IgnitionModel fresh;
    auto const listed = fresh.execute(lims::ListIgnitionTests{});
    REQUIRE(listed.tests.size() == 1);
    CHECK(listed.tests.at(0).container == mass(100));
    CHECK(listed.tests.at(0).before == mass(299));
    CHECK(listed.tests.at(0).after == mass(223));
}

TEST_CASE("recording_only_some_values_keeps_the_others", "[lims][ignition]") {
    DbFixture const db;
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::IgnitionModel model;
    auto const added = model.execute(lims::AddIgnitionTest{});
    static_cast<void>(model.execute(lims::RecordIgnitionValues{.id = added.id, .container = mass(100)}));
    auto const second = model.execute(lims::RecordIgnitionValues{.id = added.id, .before = mass(299)});
    CHECK(second.container == mass(100));
    CHECK(second.before == mass(299));
    CHECK_FALSE(second.after.has_value());
}

TEST_CASE("recording_on_an_unknown_test_is_refused", "[lims][ignition]") {
    DbFixture const db;
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::IgnitionModel model;
    CHECK_THROWS_AS(model.execute(lims::RecordIgnitionValues{.id = 999, .container = mass(1)}), lims::NotFound);
}

TEST_CASE("deleting_removes_it", "[lims][ignition]") {
    DbFixture const db;
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::IgnitionModel model;
    auto const added = model.execute(lims::AddIgnitionTest{});
    static_cast<void>(model.execute(lims::DeleteIgnitionTest{.id = added.id}));
    CHECK(model.execute(lims::ListIgnitionTests{}).tests.empty());
    CHECK_THROWS_AS(model.execute(lims::DeleteIgnitionTest{.id = added.id}), lims::NotFound);
}

// Mutation: leave the container out of the numerator.
TEST_CASE("the_result_is_worked_out_from_the_three_masses", "[lims][ignition]") {
    DbFixture const db;
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::IgnitionModel model;
    auto const added = model.execute(lims::AddIgnitionTest{});
    CHECK_FALSE(added.result.has_value());
    auto const recorded = model.execute(
        lims::RecordIgnitionValues{.id = added.id, .container = mass(100), .before = mass(299), .after = mass(223)});
    // ((299 - 223) - 100) / (299 - 100)
    REQUIRE(recorded.result.has_value());
    CHECK(*recorded.result == decimal(-1206, 10000, 4));
    lims::IgnitionModel fresh;
    CHECK(fresh.execute(lims::ListIgnitionTests{}).tests.at(0).result == decimal(-1206, 10000, 4));
}

// Mutation: treat an absent mass as 0.
TEST_CASE("there_is_no_result_until_every_mass_is_known", "[lims][ignition]") {
    DbFixture const db;
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::IgnitionModel model;
    auto const added = model.execute(lims::AddIgnitionTest{});
    auto const partial =
        model.execute(lims::RecordIgnitionValues{.id = added.id, .container = mass(100), .before = mass(299)});
    CHECK_FALSE(partial.result.has_value());
}

// Mutation: divide anyway, which gives infinity or NaN and no JSON number at all.
TEST_CASE("a_sample_of_no_mass_has_no_result", "[lims][ignition]") {
    DbFixture const db;
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::IgnitionModel model;
    auto const added = model.execute(lims::AddIgnitionTest{});
    auto const recorded = model.execute(
        lims::RecordIgnitionValues{.id = added.id, .container = mass(100), .before = mass(100), .after = mass(90)});
    CHECK_FALSE(recorded.result.has_value());
}
