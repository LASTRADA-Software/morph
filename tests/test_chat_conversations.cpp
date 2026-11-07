// SPDX-License-Identifier: Apache-2.0
//
// One conversation per person, screen and record, kept for a configured time and bounded in size.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/ui/chat/conversations.hpp>
#include <morph/ui/chat/provider.hpp>
#include <morph/ui/chat/service.hpp>
#include <string>
#include <vector>

namespace chat = morph::ui::chat;

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

chat::Message said(std::string text) { return {.role = "user", .text = std::move(text), .calls = {}, .results = {}}; }
chat::Message replied(std::string text) {
    return {.role = "assistant", .text = std::move(text), .calls = {}, .results = {}};
}
chat::Message calling() {
    return {.role = "assistant",
            .text = {},
            .calls = {chat::ToolCall{.id = "1", .name = "tool", .argumentsJson = "{}"}},
            .results = {}};
}
chat::Message results() {
    return {.role = "user",
            .text = {},
            .calls = {},
            .results = {chat::ToolResult{.callId = "1", .text = "ok", .isError = false}}};
}

chat::ChatLimits limits() { return {}; }

}  // namespace

TEST_CASE("two_records_on_one_screen_keep_separate_conversations", "[chat][conversation]") {
    chat::ConversationStore store{limits()};
    auto const now = Clock::now();
    store.append({.principal = "alice", .screenId = "s", .identity = "1"}, said("one"), now);
    store.append({.principal = "alice", .screenId = "s", .identity = "2"}, said("two"), now);
    auto const first = store.history({.principal = "alice", .screenId = "s", .identity = "1"}, now);
    REQUIRE(first.size() == 1);
    CHECK(first.front().text == "one");
    CHECK(store.history({.principal = "alice", .screenId = "s", .identity = "2"}, now).front().text == "two");
}

TEST_CASE("a_screen_with_no_record_has_one_conversation", "[chat][conversation]") {
    chat::ConversationStore store{limits()};
    auto const now = Clock::now();
    chat::ConversationKey const key{.principal = "alice", .screenId = "s", .identity = ""};
    store.append(key, said("one"), now);
    store.append(key, replied("two"), now);
    CHECK(store.history(key, now).size() == 2);
}

TEST_CASE("two_people_on_one_record_do_not_share", "[chat][conversation]") {
    chat::ConversationStore store{limits()};
    auto const now = Clock::now();
    store.append({.principal = "alice", .screenId = "s", .identity = "1"}, said("mine"), now);
    CHECK(store.history({.principal = "bob", .screenId = "s", .identity = "1"}, now).empty());
}

TEST_CASE("the_idle_limit_ends_a_conversation", "[chat][conversation]") {
    auto configured = limits();
    configured.idleRetention = 10min;
    chat::ConversationStore store{configured};
    auto const start = Clock::now();
    chat::ConversationKey const key{.principal = "alice", .screenId = "s", .identity = "1"};
    store.append(key, said("one"), start);
    // Reading it counts as using it, so it stays for another ten minutes.
    CHECK(store.history(key, start + 9min).size() == 1);
    CHECK(store.history(key, start + 18min).size() == 1);
    CHECK(store.history(key, start + 29min).empty());
}

TEST_CASE("an_append_drops_every_expired_conversation", "[chat][conversation]") {
    auto configured = limits();
    configured.idleRetention = 10min;
    chat::ConversationStore store{configured};
    auto const start = Clock::now();
    store.append({.principal = "alice", .screenId = "s", .identity = "1"}, said("one"), start);
    store.append({.principal = "bob", .screenId = "s", .identity = "2"}, said("two"), start + 11min);
    CHECK(store.size() == 1);
}

TEST_CASE("endAll_ends_only_that_principals_conversations", "[chat][conversation]") {
    chat::ConversationStore store{limits()};
    auto const now = Clock::now();
    store.append({.principal = "alice", .screenId = "s", .identity = "1"}, said("a"), now);
    store.append({.principal = "alice", .screenId = "t", .identity = "1"}, said("a"), now);
    store.append({.principal = "bob", .screenId = "s", .identity = "1"}, said("b"), now);
    store.endAll("alice");
    CHECK(store.history({.principal = "alice", .screenId = "s", .identity = "1"}, now).empty());
    CHECK(store.history({.principal = "alice", .screenId = "t", .identity = "1"}, now).empty());
    CHECK(store.history({.principal = "bob", .screenId = "s", .identity = "1"}, now).size() == 1);
}

TEST_CASE("trimming_never_leaves_an_orphan_result", "[chat][conversation]") {
    auto configured = limits();
    configured.maxMessagesPerConversation = 4;
    chat::ConversationStore store{configured};
    auto const now = Clock::now();
    chat::ConversationKey const key{.principal = "alice", .screenId = "s", .identity = "1"};
    store.append(key, said("one"), now);
    store.append(key, calling(), now);
    store.append(key, results(), now);
    store.append(key, replied("two"), now);
    store.append(key, said("three"), now);
    auto const history = store.history(key, now);
    // Dropping only the oldest message would start the history at the call, whose result a provider cannot place.
    REQUIRE(history.size() == 1);
    CHECK(history.front().role == "user");
    CHECK(history.front().results.empty());
    CHECK(history.front().text == "three");
}

TEST_CASE("the_per_principal_conversation_cap_evicts_the_oldest", "[chat][conversation]") {
    auto configured = limits();
    configured.maxConversationsPerPrincipal = 2;
    chat::ConversationStore store{configured};
    auto const now = Clock::now();
    store.append({.principal = "alice", .screenId = "s", .identity = "1"}, said("one"), now);
    store.append({.principal = "alice", .screenId = "s", .identity = "2"}, said("two"), now + 1min);
    // Reading the first makes the second the least recently used.
    static_cast<void>(store.history({.principal = "alice", .screenId = "s", .identity = "1"}, now + 2min));
    store.append({.principal = "alice", .screenId = "s", .identity = "3"}, said("three"), now + 3min);
    store.append({.principal = "bob", .screenId = "s", .identity = "4"}, said("other"), now + 3min);
    CHECK(store.history({.principal = "alice", .screenId = "s", .identity = "2"}, now + 4min).empty());
    CHECK(store.history({.principal = "alice", .screenId = "s", .identity = "1"}, now + 4min).size() == 1);
    CHECK(store.history({.principal = "alice", .screenId = "s", .identity = "3"}, now + 4min).size() == 1);
    CHECK(store.history({.principal = "bob", .screenId = "s", .identity = "4"}, now + 4min).size() == 1);
}
