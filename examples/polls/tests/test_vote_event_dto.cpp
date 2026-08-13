// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <polls/dto/event_dto.hpp>
#include <polls/dto/vote_dto.hpp>

TEST_CASE("SubmitVotes/UpdateVotes require a bounded participantName and at least one vote", "[polls][dto]") {
    polls::SubmitVotes action;
    CHECK_FALSE(action.validate());
    action.participantName = "alice";
    CHECK_FALSE(action.validate());  // no votes yet
    action.votes.push_back({.optionId = polls::OptionId{.value = 1}, .choice = polls::VoteChoice::Yes});
    CHECK(action.validate());
}

TEST_CASE("AddComment requires a bounded body", "[polls][dto]") {
    polls::AddComment action{.participantName = "alice", .body = ""};
    CHECK_FALSE(action.validate());
    action.body = std::string(polls::kMaxCommentBytes + 1, 'x');
    CHECK_FALSE(action.validate());
    action.body = "works for me";
    CHECK(action.validate());
}

TEST_CASE("FinalizePoll requires a real optionId", "[polls][dto]") {
    CHECK_FALSE(polls::FinalizePoll{}.validate());
    CHECK(polls::FinalizePoll{.optionId = polls::OptionId{.value = 1}}.validate());
}

TEST_CASE("GetEventsSince{} (lastEventId unset) validates -- it means \"from the beginning\"", "[polls][dto]") {
    CHECK(polls::GetEventsSince{}.validate());
}
