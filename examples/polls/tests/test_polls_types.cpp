// SPDX-License-Identifier: Apache-2.0
#include "polls/core/errors.hpp"
#include "polls/core/types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string_view>

TEST_CASE("OptionId/PollEventId are independently hasValue()-capable", "[polls][types]") {
    CHECK_FALSE(polls::OptionId{}.hasValue());
    CHECK(polls::OptionId{.value = 1}.hasValue());
    CHECK_FALSE(polls::PollEventId{}.hasValue());
    CHECK(polls::PollEventId{.value = 1}.hasValue());
}

TEST_CASE("OptionId equality follows the payload", "[polls][types]") {
    CHECK(polls::OptionId{.value = 5} == polls::OptionId{.value = 5});
    CHECK_FALSE(polls::OptionId{.value = 5} == polls::OptionId{.value = 6});
}

TEST_CASE("kTokenBytes is a plausible unguessable-token length", "[polls][types]") {
    STATIC_REQUIRE(polls::kTokenBytes >= 16);  // enough entropy to resist guessing
}

TEST_CASE("PollsError hierarchy: each derived type carries its own message", "[polls][types]") {
    CHECK(std::string_view{polls::NotFound{"poll not found"}.what()} == "poll not found");
    CHECK(std::string_view{polls::Forbidden{"not the admin"}.what()} == "not the admin");
    CHECK(std::string_view{polls::Conflict{"already finalized"}.what()} == "already finalized");
}

TEST_CASE("polls id types: fromRowId rejects the one value they cannot represent", "[polls][types]") {
    // 0 is these types' "not entered" sentinel, so an id of 0 would arrive as
    // *absent* and a real record would read as "no record" (morph#215). Row
    // ids start at 1, so this never fires in practice -- the point is that a
    // seeded, migrated, or externally supplied 0 fails loudly at the boundary
    // instead of collapsing silently one layer below the QML surface.
    CHECK_THROWS_AS(polls::OptionId::fromRowId(0), polls::PollsError);
    CHECK_THROWS_AS(polls::PollEventId::fromRowId(0), polls::PollsError);
}

TEST_CASE("polls id types: fromRowId wraps an ordinary row id unchanged", "[polls][types]") {
    // The control case: without it the check above would pass against a
    // factory that rejected everything.
    auto const option = polls::OptionId::fromRowId(7);
    CHECK(option.hasValue());
    CHECK(*option == 7);

    auto const event = polls::PollEventId::fromRowId(9223372036854775807);
    CHECK(event.hasValue());
    CHECK(*event == 9223372036854775807);
}
