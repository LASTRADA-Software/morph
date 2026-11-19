// SPDX-License-Identifier: Apache-2.0
//
// What a client and the chat service say to each other, and the scripted provider the chat tests run on.

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <memory>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/chat/provider.hpp>
#include <morph/ui/testing/fake_chat_provider.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chat = morph::ui::chat;

namespace {

// What one provider call delivered.
struct Delivered {
    std::vector<std::string> text;
    std::optional<std::expected<chat::Response, std::string>> done;

    chat::ProviderSink sink() {
        return {.text = [this](std::string const& piece) { text.push_back(piece); },
                .done = [this](std::expected<chat::Response, std::string> outcome) { done = std::move(outcome); }};
    }
};

}  // namespace

TEST_CASE("chat_bodies_round_trip", "[chat][protocol]") {
    chat::TurnRequest request;
    request.screenId = "lims.ignition";
    request.identity = "\"42\"";
    request.paramsJson = R"({"sampleId":42})";
    request.models.push_back(
        {.path = "", .alias = "ignition", .modelType = "IgnitionModel", .instanceId = 7, .primary = "42"});
    request.text = "container 100, before 299, after 223";
    auto const turn = chat::readTurnRequest(chat::writeMessage(request));
    REQUIRE(turn.has_value());
    CHECK(turn->screenId == "lims.ignition");
    CHECK(turn->identity == "\"42\"");
    CHECK(turn->paramsJson == R"({"sampleId":42})");
    REQUIRE(turn->models.size() == 1);
    CHECK(turn->models.front().instanceId == 7);
    CHECK(turn->models.front().primary == "42");
    CHECK(turn->text == request.text);

    chat::TurnState state;
    state.turnId = 3;
    state.status = chat::TurnStatus::Cancelled;
    state.text = "stopped";
    state.steps.push_back({.name = "ignition__AddIgnitionTest", .ok = true, .message = "{}"});
    auto const read = chat::readTurnState(chat::writeMessage(state));
    REQUIRE(read.has_value());
    CHECK(read->status == chat::TurnStatus::Cancelled);
    REQUIRE(read->steps.size() == 1);
    CHECK(read->steps.front().ok);

    auto const event = chat::readTurnEvent(
        chat::writeMessage(chat::TurnEvent{.turnId = 3, .seq = 9, .kind = "text", .text = "line\none"}));
    REQUIRE(event.has_value());
    CHECK(event->seq == 9);
    CHECK(event->text == "line\none");

    auto const probe = chat::readStateRequest("{}");
    REQUIRE(probe.has_value());
    CHECK_FALSE(probe->turnId.has_value());
    auto const accepted = chat::readTurnAccepted(chat::writeMessage(chat::TurnAccepted{.turnId = 11}));
    REQUIRE(accepted.has_value());
    CHECK(accepted->turnId == 11);
}

TEST_CASE("a_malformed_turn_body_is_refused_not_thrown", "[chat][protocol]") {
    CHECK_FALSE(chat::readTurnRequest("[1]").has_value());
    CHECK_FALSE(chat::readTurnRequest("not json").has_value());
    CHECK_FALSE(chat::readTurnRequest("").has_value());
    CHECK_FALSE(chat::readTurnState(R"({"status":"nonsense"})").has_value());
    CHECK_FALSE(chat::readTurnEvent("{").has_value());
}

TEST_CASE("the_fake_provider_plays_its_script_then_fails", "[chat][protocol]") {
    chat::testing::FakeChatProvider fake;
    fake.script(chat::Response{.text = "first", .calls = {}, .opaque = {}});
    fake.script(chat::Response{.text = "", .calls = {chat::ToolCall{.id = "1", .name = "a", .argumentsJson = "{}"}}});

    Delivered first;
    static_cast<void>(fake.stream(chat::Request{.system = "s", .messages = {}, .tools = {}}, first.sink()));
    REQUIRE(first.done.has_value());
    REQUIRE(first.done->has_value());
    CHECK((*first.done)->text == "first");
    CHECK(first.text == std::vector<std::string>{"first"});

    Delivered second;
    static_cast<void>(fake.stream(chat::Request{}, second.sink()));
    REQUIRE(second.done->has_value());
    CHECK((*second.done)->calls.size() == 1);

    Delivered third;
    static_cast<void>(fake.stream(chat::Request{}, third.sink()));
    REQUIRE(third.done.has_value());
    CHECK_FALSE(third.done->has_value());
    CHECK(fake.calls() == 3);
    REQUIRE(fake.requests().size() == 3);
    CHECK(fake.requests().front().system == "s");
}

TEST_CASE("a_held_fake_call_reports_its_cancel", "[chat][protocol]") {
    chat::testing::FakeChatProvider fake;
    fake.script(chat::Response{.text = "late", .calls = {}, .opaque = {}});
    fake.hold(true);

    Delivered held;
    auto const call = fake.stream(chat::Request{}, held.sink());
    CHECK_FALSE(held.done.has_value());
    call->cancel();
    CHECK(fake.cancelled() == 1);
    fake.release();
    CHECK_FALSE(held.done.has_value());

    Delivered released;
    fake.hold(true);
    fake.script(chat::Response{.text = "now", .calls = {}, .opaque = {}});
    static_cast<void>(fake.stream(chat::Request{}, released.sink()));
    CHECK_FALSE(released.done.has_value());
    fake.release();
    REQUIRE(released.done.has_value());
    CHECK((*released.done)->text == "now");
}

TEST_CASE("a_turns_parts_arguments_and_title_survive_the_wire", "[chat][protocol]") {
    chat::TurnState state;
    state.turnId = 4;
    state.text = "a\n\nb";
    state.parts.push_back({.kind = "text", .text = "a", .step = {}});
    state.parts.push_back(
        {.kind = "tool", .text = {}, .step = {.name = "t", .ok = true, .message = "m", .arguments = R"({"k":1})"}});
    auto const read = chat::readTurnState(chat::writeMessage(state));
    REQUIRE(read.has_value());
    REQUIRE(read->parts.size() == 2);
    CHECK(read->parts.at(1).step.arguments == R"({"k":1})");
    CHECK(read->parts.at(1).step.ok);

    auto const event = chat::readTurnEvent(chat::writeMessage(chat::TurnEvent{
        .turnId = 1, .seq = 2, .kind = "tool", .text = "t", .arguments = "{}", .ok = true, .newRound = true}));
    REQUIRE(event.has_value());
    CHECK(event->arguments == "{}");
    CHECK(event->ok);
    CHECK(event->newRound);

    chat::TurnRequest request;
    request.title = "16/1";
    auto const readRequest = chat::readTurnRequest(chat::writeMessage(request));
    REQUIRE(readRequest.has_value());
    CHECK(readRequest->title == "16/1");

    // A state from a server that sends no parts reads with none.
    auto const legacy = chat::readTurnState(R"({"turnId":4,"status":"done","text":"x","steps":[]})");
    REQUIRE(legacy.has_value());
    CHECK(legacy->parts.empty());
}

// Mutation: drop `draft` from `TurnRequest`, `TurnState` or the event: the document is lost on the way.
TEST_CASE("a_draft_round_trips_in_the_request_the_state_and_the_event", "[chat][protocol][draft]") {
    chat::TurnRequest request;
    request.screenId = "lims.ignition";
    request.text = "move the field";
    request.draft = R"({"vocab":"ui/1","id":"lims.ignition"})";
    auto const turn = chat::readTurnRequest(chat::writeMessage(request));
    REQUIRE(turn.has_value());
    REQUIRE(turn->draft.has_value());
    CHECK(*turn->draft == *request.draft);

    chat::TurnState state;
    state.turnId = 3;
    state.draft = request.draft;
    auto const read = chat::readTurnState(chat::writeMessage(state));
    REQUIRE(read.has_value());
    CHECK(read->draft == request.draft);

    auto const event = chat::readTurnEvent(
        chat::writeMessage(chat::TurnEvent{.turnId = 3, .seq = 1, .kind = "draft", .text = *request.draft}));
    REQUIRE(event.has_value());
    CHECK(event->kind == "draft");
    CHECK(event->text == *request.draft);
}

// Mutation: write `draft` as an empty string, or `null`, when there is none.
TEST_CASE("a_message_without_a_draft_does_not_say_one", "[chat][protocol][draft]") {
    chat::TurnRequest request;
    request.screenId = "s";
    request.text = "hi";
    CHECK_FALSE(chat::writeMessage(request).contains("draft"));
    CHECK_FALSE(chat::writeMessage(chat::TurnState{}).contains("draft"));
    auto const turn = chat::readTurnRequest(chat::writeMessage(request));
    REQUIRE(turn.has_value());
    CHECK_FALSE(turn->draft.has_value());
}

// An older peer neither sends nor understands the field; each side reads what the other wrote.
// Mutation: make an unknown key a refusal in `readTurnRequest`, or require `draft`.
TEST_CASE("an_older_peer_and_a_newer_one_read_each_others_messages", "[chat][protocol][draft]") {
    auto const fromOld = chat::readTurnRequest(R"({"screenId":"s","text":"hi","models":[]})");
    REQUIRE(fromOld.has_value());
    CHECK_FALSE(fromOld->draft.has_value());
    auto const withUnknown = chat::readTurnRequest(R"({"screenId":"s","text":"hi","draft":"{}","fromTheFuture":[1]})");
    REQUIRE(withUnknown.has_value());
    CHECK(withUnknown->draft == std::optional<std::string>{"{}"});
    auto const oldState = chat::readTurnState(R"({"turnId":1,"status":"done","text":"x"})");
    REQUIRE(oldState.has_value());
    CHECK_FALSE(oldState->draft.has_value());
}

// Mutation: drop `dictation` from TurnState, or write a null text into a finished state.
TEST_CASE("chat_wire_dictation_bodies_round_trip", "[chat][protocol][dictation]") {
    chat::TranscribeRequest const request{.audio = "UklGRg==", .mediaType = "audio/wav", .language = "de"};
    auto const readRequest = chat::readTranscribeRequest(chat::writeMessage(request));
    REQUIRE(readRequest.has_value());
    CHECK(readRequest->audio == "UklGRg==");
    CHECK(readRequest->mediaType == "audio/wav");
    CHECK(readRequest->language == "de");
    CHECK(chat::readTranscribeAccepted(chat::writeMessage(chat::TranscribeAccepted{.id = 9}))->id == 9);
    CHECK(chat::readTranscriptionQuery(chat::writeMessage(chat::TranscriptionQuery{.id = 9}))->id == 9);

    chat::TranscriptionState const done{
        .id = 9, .status = chat::TranscriptionStatus::Done, .text = "hello", .error = std::nullopt};
    std::string const doneText = chat::writeMessage(done);
    CHECK(doneText.contains(R"("status":"done")"));
    CHECK_FALSE(doneText.contains("error"));
    auto const readDone = chat::readTranscriptionState(doneText);
    REQUIRE(readDone.has_value());
    CHECK(readDone->status == chat::TranscriptionStatus::Done);
    CHECK(readDone->text == "hello");

    chat::TranscriptionEvent const event{.kind = std::string{chat::kTranscriptionEventKind}, .state = done};
    auto const readEvent = chat::readTranscriptionEvent(chat::writeMessage(event));
    REQUIRE(readEvent.has_value());
    CHECK(readEvent->kind == "transcription");
    CHECK(readEvent->state.id == 9);

    chat::TurnState offer;
    offer.dictation = chat::DictationOffer{.maxSeconds = 60, .maxBytes = 4194304};
    auto const readOffer = chat::readTurnState(chat::writeMessage(offer));
    REQUIRE(readOffer.has_value());
    REQUIRE(readOffer->dictation.has_value());
    CHECK(readOffer->dictation->maxSeconds == 60);
    CHECK(readOffer->dictation->maxBytes == 4194304);
    CHECK_FALSE(chat::writeMessage(chat::TurnState{}).contains("dictation"));
}

// Mutation: drop the event's `kind`, so an older client could not tell it from a turn's.
TEST_CASE("chat_wire_an_older_client_drops_a_transcription_event", "[chat][protocol][dictation]") {
    chat::TranscriptionEvent const event{
        .kind = std::string{chat::kTranscriptionEventKind},
        .state = {.id = 3, .status = chat::TranscriptionStatus::Failed, .text = std::nullopt, .error = "cancelled"}};
    auto const asTurn = chat::readTurnEvent(chat::writeMessage(event));
    REQUIRE(asTurn.has_value());
    CHECK(asTurn->turnId == 0);
    CHECK(asTurn->seq == 0);
    CHECK(asTurn->kind == "transcription");
}
