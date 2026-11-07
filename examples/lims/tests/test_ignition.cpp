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

TEST_CASE("adding_and_recording_an_ignition_test_stores_it", "[lims][ignition]") {
    DbFixture const db;
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::IgnitionModel model;
    auto const added = model.execute(lims::AddIgnitionTest{});
    CHECK_FALSE(added.container.has_value());
    auto const recorded = model.execute(
        lims::RecordIgnitionValues{.id = added.id, .container = 100.0, .before = 299.0, .after = 223.0});
    CHECK(recorded.id == added.id);
    // A fresh instance reads it, so it is the store that holds it and not this one.
    lims::IgnitionModel fresh;
    auto const listed = fresh.execute(lims::ListIgnitionTests{});
    REQUIRE(listed.tests.size() == 1);
    CHECK(listed.tests.at(0).container == 100.0);
    CHECK(listed.tests.at(0).before == 299.0);
    CHECK(listed.tests.at(0).after == 223.0);
}

TEST_CASE("recording_only_some_values_keeps_the_others", "[lims][ignition]") {
    DbFixture const db;
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::IgnitionModel model;
    auto const added = model.execute(lims::AddIgnitionTest{});
    static_cast<void>(model.execute(lims::RecordIgnitionValues{.id = added.id, .container = 100.0}));
    auto const second = model.execute(lims::RecordIgnitionValues{.id = added.id, .before = 299.0});
    CHECK(second.container == 100.0);
    CHECK(second.before == 299.0);
    CHECK_FALSE(second.after.has_value());
}

TEST_CASE("recording_on_an_unknown_test_is_refused", "[lims][ignition]") {
    DbFixture const db;
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::IgnitionModel model;
    CHECK_THROWS_AS(model.execute(lims::RecordIgnitionValues{.id = 999, .container = 1.0}), lims::NotFound);
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
