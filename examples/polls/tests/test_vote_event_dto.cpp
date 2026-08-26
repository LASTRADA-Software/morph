// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <polls/dto/event_dto.hpp>
#include <polls/dto/poll_dto.hpp>
#include <polls/dto/vote_dto.hpp>
#include <string>

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

// ---------------------------------------------------------------------------
// x-submitMode: the three schema-driven actions opt out of auto-submit
//
// gui/qml/VoteView.qml binds `controller: page.pollBridge` on all three of
// these forms. The shipped renderer's default is to call
// submitIfValid() the instant the form is valid, which for a mutation means
// one dispatch per keystroke -- and FinalizePoll is an irreversible state
// transition. `explicitSubmit = true` on the action is what stops that, by
// making schemaJson<A>() emit the top-level "x-submitMode": "explicit" key
// DynamicForm reads (docs/spec/forms/forms.md, "Explicit submit mode").
//
// Parsed rather than substring-matched, so a key that landed inside
// `properties` -- where the renderer would never look -- fails here.
// ---------------------------------------------------------------------------

namespace {

void checkExplicitSubmitMode(const std::string& schema) {
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    const auto& root = parsed.value();
    REQUIRE(root.contains("x-submitMode"));
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
    CHECK(root["x-submitMode"].get<std::string>() == "explicit");
    REQUIRE(root.contains("properties"));
    CHECK_FALSE(root["properties"].contains("x-submitMode"));
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

}  // namespace

TEST_CASE("AddComment/FinalizePoll/UndoLastVoteChange emit x-submitMode:\"explicit\"", "[polls][dto][forms]") {
    checkExplicitSubmitMode(morph::forms::schemaJson<polls::AddComment>());
    checkExplicitSubmitMode(morph::forms::schemaJson<polls::FinalizePoll>());
    checkExplicitSubmitMode(morph::forms::schemaJson<polls::UndoLastVoteChange>());
}

TEST_CASE("The actions PollBridge dispatches in typed C++ carry no x-submitMode", "[polls][dto][forms]") {
    // The flag is a *renderer* instruction, and these five never reach the
    // renderer: CreatePoll/SubmitVotes/UpdateVotes are driven by hand-rolled
    // QML (their array-of-objects fields have no DynamicForm control), and
    // OpenPoll/GetPollState/GetEventsSince carry nothing a person types.
    // Declaring it on them would state something untrue about how they run.
    CHECK_FALSE(morph::forms::schemaJson<polls::CreatePoll>().contains("x-submitMode"));
    CHECK_FALSE(morph::forms::schemaJson<polls::SubmitVotes>().contains("x-submitMode"));
    CHECK_FALSE(morph::forms::schemaJson<polls::UpdateVotes>().contains("x-submitMode"));
    CHECK_FALSE(morph::forms::schemaJson<polls::OpenPoll>().contains("x-submitMode"));
    CHECK_FALSE(morph::forms::schemaJson<polls::GetPollState>().contains("x-submitMode"));
    CHECK_FALSE(morph::forms::schemaJson<polls::GetEventsSince>().contains("x-submitMode"));
}
