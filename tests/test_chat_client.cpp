// SPDX-License-Identifier: Apache-2.0
//
// The client's side of a chat: what it sends, what it shows, and how it recovers what an event lost.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/chat/client.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/service_client.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui = morph::ui;
namespace chat = morph::ui::chat;
namespace wire = morph::wire;

namespace {

struct FakeExchange {
    struct Sent {
        wire::Envelope request;
        std::function<void(ui::ExchangeReply)> done;
    };
    std::vector<Sent> sent;

    ui::Exchange exchange() {
        return [this](wire::Envelope request, std::function<void(ui::ExchangeReply)> done) {
            sent.push_back({.request = std::move(request), .done = std::move(done)});
        };
    }

    [[nodiscard]] std::size_t count(std::string_view kind) const {
        std::size_t found = 0;
        for (Sent const& one : sent) {
            found += one.request.kind == kind ? 1U : 0U;
        }
        return found;
    }

    // Answers the oldest unanswered envelope of @p kind.
    void answer(std::string_view kind, ui::ExchangeReply const& reply) {
        for (Sent& one : sent) {
            if (one.request.kind == kind && one.done) {
                auto const done = std::exchange(one.done, nullptr);
                done(reply);
                return;
            }
        }
        FAIL("nothing to answer of kind " << kind);
    }

    void ok(std::string_view kind, std::string body = {}) { answer(kind, wire::makeOk(0, std::move(body))); }
};

wire::EventBody eventOf(chat::TurnEvent const& event) {
    return {.topic = std::string{chat::kChatTopic}, .payload = glz::raw_json{chat::writeMessage(event)}};
}

chat::TurnRequest turnOn(std::string screen, std::string identity, std::string text) {
    chat::TurnRequest request;
    request.screenId = std::move(screen);
    request.identity = std::move(identity);
    request.text = std::move(text);
    return request;
}

std::string stateOf(std::uint64_t id, chat::TurnStatus status, std::string text, std::uint64_t seq = 0,
                    std::string error = {}) {
    chat::TurnState state;
    state.turnId = id;
    state.status = status;
    state.text = std::move(text);
    state.seq = seq;
    state.error = std::move(error);
    return chat::writeMessage(state);
}

struct Rig {
    FakeExchange wire_;
    int changes = 0;
    morph::session::Context session{.principal = "alice", .token = "t", .requestId = {}, .locale = {}, .metadata = {}};
    chat::ChatClient client{wire_.exchange(), [this] { return session; }, [this] { ++changes; }};

    // A turn accepted as id 7, with its first state read still unanswered.
    void started() {
        client.send(turnOn("s", "1", "hello"));
        wire_.ok(chat::kTurnKind, chat::writeMessage(chat::TurnAccepted{.turnId = 7}));
    }

    [[nodiscard]] std::vector<std::string> shown() const {
        std::vector<std::string> out;
        for (chat::ChatClient::Entry const& entry : client.entries()) {
            out.push_back(entry.kind + ":" + entry.text);
        }
        return out;
    }
};

using Shown = std::vector<std::string>;

}  // namespace

TEST_CASE("the_probe_finds_a_chat_or_not", "[chat][client]") {
    Rig rig;
    CHECK_FALSE(rig.client.available());
    rig.client.probe();
    REQUIRE(rig.wire_.sent.back().request.kind == chat::kStateKind);
    CHECK(rig.wire_.sent.back().request.session.token == "t");
    rig.wire_.ok(chat::kStateKind, chat::writeMessage(chat::TurnState{}));
    CHECK(rig.client.available());
    CHECK(rig.changes > 0);

    rig.client.probe();
    rig.wire_.answer(chat::kStateKind, wire::makeErr("unknown envelope kind: ui-chat-state", 0));
    CHECK_FALSE(rig.client.available());
    CHECK(rig.client.entries().empty());

    rig.client.probe();
    rig.wire_.answer(chat::kStateKind, std::unexpected(std::string{"not delivered"}));
    CHECK_FALSE(rig.client.available());
}

TEST_CASE("sending_shows_the_words_and_carries_the_session", "[chat][client]") {
    Rig rig;
    rig.client.send(turnOn("s", "1", "hello"));
    CHECK(rig.shown() == Shown{"user:hello"});
    CHECK(rig.client.running());
    auto const& sent = rig.wire_.sent.back().request;
    CHECK(sent.kind == chat::kTurnKind);
    CHECK(sent.session.token == "t");
    auto const body = chat::readTurnRequest(sent.body);
    REQUIRE(body.has_value());
    CHECK(body->text == "hello");
}

TEST_CASE("a_refused_turn_says_why_and_stops", "[chat][client]") {
    Rig rig;
    rig.client.send(turnOn("s", "1", "hello"));
    rig.wire_.answer(chat::kTurnKind, wire::makeErr("too many turns are running", 0));
    CHECK(rig.shown() == Shown{"user:hello", "error:too many turns are running"});
    CHECK_FALSE(rig.client.running());
}

TEST_CASE("text_events_build_the_reply_in_order", "[chat][client]") {
    Rig rig;
    rig.started();
    rig.client.deliverEvent(eventOf({.turnId = 7, .seq = 2, .kind = "text", .text = "lo"}));
    // An older event than one already applied is dropped.
    rig.client.deliverEvent(eventOf({.turnId = 7, .seq = 1, .kind = "text", .text = "hel"}));
    CHECK(rig.shown() == Shown{"user:hello", "assistant:lo"});
    // Another turn's event is dropped.
    rig.client.deliverEvent(eventOf({.turnId = 8, .seq = 3, .kind = "text", .text = "x"}));
    CHECK(rig.shown() == Shown{"user:hello", "assistant:lo"});
    rig.client.deliverEvent(eventOf({.turnId = 7, .seq = 3, .kind = "tool", .text = "lab__Add"}));
    CHECK(rig.shown() == Shown{"user:hello", "tool:lab__Add", "assistant:lo"});
}

TEST_CASE("the_final_state_replaces_what_events_built", "[chat][client]") {
    Rig rig;
    rig.started();
    rig.client.deliverEvent(eventOf({.turnId = 7, .seq = 1, .kind = "text", .text = "partial"}));
    rig.client.deliverEvent(eventOf({.turnId = 7, .seq = 2, .kind = "done", .text = ""}));
    CHECK(rig.client.running());
    rig.wire_.ok(chat::kStateKind, stateOf(7, chat::TurnStatus::Done, "the whole reply", 2));
    CHECK_FALSE(rig.client.running());
    CHECK(rig.shown() == Shown{"user:hello", "assistant:the whole reply"});
}

TEST_CASE("an_event_already_in_the_state_is_not_applied_twice", "[chat][client]") {
    Rig rig;
    rig.started();
    rig.wire_.ok(chat::kStateKind, stateOf(7, chat::TurnStatus::Running, "abc", 3));
    rig.client.deliverEvent(eventOf({.turnId = 7, .seq = 3, .kind = "text", .text = "c"}));
    CHECK(rig.shown() == Shown{"user:hello", "assistant:abc"});
    rig.client.deliverEvent(eventOf({.turnId = 7, .seq = 4, .kind = "text", .text = "d"}));
    CHECK(rig.shown() == Shown{"user:hello", "assistant:abcd"});
}

TEST_CASE("a_failed_turn_shows_its_reason", "[chat][client]") {
    Rig rig;
    rig.started();
    rig.wire_.ok(chat::kStateKind, stateOf(7, chat::TurnStatus::Failed, "so far", 1, "the limit"));
    CHECK_FALSE(rig.client.running());
    CHECK(rig.shown() == Shown{"user:hello", "assistant:so far", "error:the limit"});
}

TEST_CASE("a_resync_recovers_a_lost_end", "[chat][client]") {
    Rig rig;
    rig.started();
    rig.wire_.ok(chat::kStateKind, stateOf(7, chat::TurnStatus::Running, "", 0));
    std::size_t const before = rig.wire_.count(chat::kStateKind);
    rig.client.deliverEvent({.topic = std::string{wire::kResyncTopic}, .payload = glz::raw_json{"{}"}});
    REQUIRE(rig.wire_.count(chat::kStateKind) == before + 1);
    rig.wire_.ok(chat::kStateKind, stateOf(7, chat::TurnStatus::Done, "finished", 5));
    CHECK_FALSE(rig.client.running());
    CHECK(rig.shown() == Shown{"user:hello", "assistant:finished"});
}

TEST_CASE("a_second_send_while_running_is_refused", "[chat][client]") {
    Rig rig;
    rig.started();
    rig.client.send(turnOn("s", "1", "again"));
    CHECK(rig.wire_.count(chat::kTurnKind) == 1);
    CHECK(rig.shown() == Shown{"user:hello"});
}

TEST_CASE("switching_conversation_keeps_the_running_turn_in_its_own", "[chat][client]") {
    Rig rig;
    rig.started();
    rig.client.selectConversation("s", "2");
    CHECK(rig.client.entries().empty());
    rig.client.deliverEvent(eventOf({.turnId = 7, .seq = 1, .kind = "text", .text = "for one"}));
    CHECK(rig.client.entries().empty());
    rig.client.selectConversation("s", "1");
    CHECK(rig.shown() == Shown{"user:hello", "assistant:for one"});
}

TEST_CASE("another_topic_is_ignored", "[chat][client]") {
    Rig rig;
    rig.started();
    std::size_t const sent = rig.wire_.sent.size();
    rig.client.deliverEvent({.topic = std::string{wire::kChangesTopic}, .payload = glz::raw_json{"{}"}});
    CHECK(rig.wire_.sent.size() == sent);
    CHECK(rig.shown() == Shown{"user:hello"});
}

TEST_CASE("a_malformed_event_is_ignored", "[chat][client]") {
    Rig rig;
    rig.started();
    rig.client.deliverEvent({.topic = std::string{chat::kChatTopic}, .payload = glz::raw_json{"[1]"}});
    CHECK(rig.shown() == Shown{"user:hello"});
}

TEST_CASE("cancel_asks_the_server_and_reads_the_end", "[chat][client]") {
    Rig rig;
    rig.started();
    rig.wire_.ok(chat::kStateKind, stateOf(7, chat::TurnStatus::Running, "", 0));
    rig.client.cancel();
    REQUIRE(rig.wire_.count(chat::kCancelKind) == 1);
    rig.wire_.ok(chat::kCancelKind);
    rig.wire_.ok(chat::kStateKind, stateOf(7, chat::TurnStatus::Cancelled, "partly", 1));
    CHECK_FALSE(rig.client.running());
}

TEST_CASE("end_calls_done_and_forgets", "[chat][client]") {
    Rig rig;
    rig.client.send(turnOn("s", "1", "hello"));
    bool done = false;
    rig.client.end([&] { done = true; });
    REQUIRE(rig.wire_.count(chat::kEndKind) == 1);
    CHECK(rig.wire_.sent.back().request.session.token == "t");
    CHECK_FALSE(done);
    rig.wire_.ok(chat::kEndKind);
    CHECK(done);
    CHECK_FALSE(rig.client.running());
    CHECK(rig.client.entries().empty());
}

TEST_CASE("end_calls_done_when_the_exchange_fails", "[chat][client]") {
    Rig rig;
    bool done = false;
    rig.client.end([&] { done = true; });
    rig.wire_.answer(chat::kEndKind, std::unexpected(std::string{"the connection is closed"}));
    CHECK(done);
}

TEST_CASE("a_reconnect_ends_a_running_turn_and_keeps_the_entries", "[chat][client]") {
    Rig rig;
    rig.started();
    rig.client.deliverEvent(eventOf({.turnId = 7, .seq = 1, .kind = "text", .text = "partial"}));
    rig.client.reconnected();
    CHECK_FALSE(rig.client.running());
    Shown const shown = rig.shown();
    REQUIRE(shown.size() == 3);
    CHECK(shown.at(1) == "assistant:partial");
    CHECK(shown.at(2).starts_with("error:"));
    CHECK(rig.wire_.sent.back().request.kind == chat::kStateKind);
    // The new connection can send again.
    rig.client.send(turnOn("s", "1", "once more"));
    CHECK(rig.client.running());
}

TEST_CASE("a_reply_after_destruction_reaches_nothing", "[chat][client]") {
    FakeExchange exchange;
    int changes = 0;
    {
        chat::ChatClient client{exchange.exchange(), [] { return morph::session::Context{}; }, [&] { ++changes; }};
        client.send(turnOn("s", "1", "hello"));
        changes = 0;
    }
    exchange.ok(chat::kTurnKind, chat::writeMessage(chat::TurnAccepted{.turnId = 7}));
    CHECK(changes == 0);
}
