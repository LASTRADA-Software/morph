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
    fake.script(chat::Response{.text = "first", .calls = {}});
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
    fake.script(chat::Response{.text = "late", .calls = {}});
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
    fake.script(chat::Response{.text = "now", .calls = {}});
    static_cast<void>(fake.stream(chat::Request{}, released.sink()));
    CHECK_FALSE(released.done.has_value());
    fake.release();
    REQUIRE(released.done.has_value());
    CHECK((*released.done)->text == "now");
}
