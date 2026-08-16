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
