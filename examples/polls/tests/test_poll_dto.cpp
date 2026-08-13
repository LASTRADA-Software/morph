// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <polls/dto/poll_dto.hpp>

TEST_CASE("CreatePoll requires a bounded title and 2-20 bounded-label options", "[polls][dto]") {
    polls::CreatePoll action;
    CHECK_FALSE(action.validate());  // no title, no options
    action.title = "Team offsite";
    CHECK_FALSE(action.validate());  // still no options
    action.options = {{"2026-09-01"}};
    CHECK_FALSE(action.validate());  // only one option
    action.options.push_back({"2026-09-02"});
    CHECK(action.validate());
    action.options.push_back({""});
    CHECK_FALSE(action.validate());  // empty label
    action.title = std::string(polls::kMaxTitleBytes + 1, 't');
    action.options = {{"a"}, {"b"}};
    CHECK_FALSE(action.validate());  // title too long
}

TEST_CASE("OpenPoll requires a non-empty pollId", "[polls][dto]") {
    CHECK_FALSE(polls::OpenPoll{}.validate());
    CHECK(polls::OpenPoll{.pollId = "abc"}.validate());
}

TEST_CASE("GetPollStateResult contains all nested views with correct field values", "[polls][dto]") {
    polls::GetPollStateResult result;
    result.pollId = "abc";
    result.title = "Team offsite";
    result.options.push_back({.id = polls::OptionId{.value = 1}, .label = "2026-09-01",
                               .yesCount = polls::Count::fromDouble(2.0)});
    result.votes.push_back({.participantName = "alice", .optionId = polls::OptionId{.value = 1},
                             .choice = polls::VoteChoice::Yes});
    result.comments.push_back({.participantName = "alice", .body = "works for me"});

    // Verify field values directly; JSON round-trip via ActionTraits<GetPollState>::resultToJson/resultFromJson
    // will be added in Task 3's reflection registration.
    CHECK(result.pollId == "abc");
    CHECK(result.title == "Team offsite");
    CHECK(result.finalized == polls::Finalized::No);
    CHECK(!result.finalizedOptionId.hasValue());
    CHECK(result.options.size() == 1);
    CHECK(result.options[0].id == polls::OptionId{.value = 1});
    CHECK(result.options[0].label == "2026-09-01");
    CHECK(result.options[0].yesCount == polls::Count::fromDouble(2.0));
    CHECK(result.votes.size() == 1);
    CHECK(result.votes[0].participantName == "alice");
    CHECK(result.votes[0].optionId == polls::OptionId{.value = 1});
    CHECK(result.votes[0].choice == polls::VoteChoice::Yes);
    CHECK(result.comments.size() == 1);
    CHECK(result.comments[0].participantName == "alice");
    CHECK(result.comments[0].body == "works for me");
}
