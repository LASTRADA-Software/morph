// SPDX-License-Identifier: Apache-2.0
//
// The client's offline queue (spec 5 §5, `"offline": "queue"`): a queued call the server cannot be reached for is held
// and its caller stays pending; replay re-sends the held calls in order and settles each caller with its replay.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <exception>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/ui/queueing_dispatcher.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace {

using morph::ui::CallOptions;
using morph::ui::QueueingDispatcher;
using morph::ui::testing::ScriptedDispatcher;

constexpr CallOptions kQueued{.evaluate = false, .queueOffline = true};

/// What became of one call: still pending, answered, or failed.
struct Outcome {
    std::optional<std::string> reply;
    std::optional<std::string> failure;
};

struct Rig {
    morph::testing::StepExecutor owner;
    ScriptedDispatcher scripted{owner};
    QueueingDispatcher queueing{scripted, &owner};
    std::shared_ptr<morph::ui::ModelChannel> channel =
        queueing.bind("Kanban_BoardModel", morph::bridge::BindSharing::Private, "");
    std::vector<std::shared_ptr<Outcome>> outcomes;

    /// Sends @p action with @p options, recording what becomes of it.
    std::shared_ptr<Outcome> send(std::string const& action, CallOptions options = kQueued) {
        auto outcome = std::make_shared<Outcome>();
        channel->execute(action, "{}", options)
            .then([outcome](std::string const& reply) { outcome->reply = reply; })
            .onError([outcome](std::exception_ptr const& error) {
                outcome->failure = morph::ui::detail::failureText(error);
            });
        owner.runAll();
        return outcome;
    }

    /// The server cannot be reached for call @p index.
    void unreachable(std::size_t index) {
        scripted.calls.at(index).answered = true;
        scripted.calls.at(index).promise.reject(std::make_exception_ptr(morph::backend::DisconnectedError{}));
        owner.runAll();
    }

    void reply(std::size_t index, std::string json) {
        scripted.reply(index, std::move(json));
        owner.runAll();
    }

    void replay() {
        queueing.replay();
        owner.runAll();
    }
};

}  // namespace

// Mutation: reject the caller on DisconnectedError (the mutation fails at once instead of waiting for its replay).
TEST_CASE("QueueingDispatcher holds a queued call the server cannot be reached for, and settles it on replay",
          "[ui][offline]") {
    Rig rig;
    auto const moved = rig.send("MoveTaskPosition");
    rig.unreachable(0);
    CHECK_FALSE(moved->reply.has_value());
    CHECK_FALSE(moved->failure.has_value());
    CHECK(rig.queueing.held() == 1);

    rig.replay();
    REQUIRE(rig.scripted.calls.size() == 2);
    CHECK(rig.scripted.calls.at(1).action == "MoveTaskPosition");
    rig.reply(1, R"({"ok":true})");
    CHECK(moved->reply == R"({"ok":true})");
    CHECK(rig.queueing.held() == 0);
}

// Mutation: send a call made while others are held straight away (it overtakes them).
TEST_CASE("QueueingDispatcher keeps the order: a call behind a held one waits, and replays after it",
          "[ui][offline]") {
    Rig rig;
    auto const first = rig.send("MoveTaskPosition");
    rig.unreachable(0);
    auto const second = rig.send("AddComment");
    CHECK(rig.scripted.calls.size() == 1);
    CHECK(rig.queueing.held() == 2);

    rig.replay();
    REQUIRE(rig.scripted.calls.size() == 2);
    CHECK(rig.scripted.calls.at(1).action == "MoveTaskPosition");
    rig.reply(1, "1");
    REQUIRE(rig.scripted.calls.size() == 3);
    CHECK(rig.scripted.calls.at(2).action == "AddComment");
    rig.reply(2, "2");
    CHECK(first->reply == "1");
    CHECK(second->reply == "2");
}

// Mutation: drop a call whose replay is still unreachable (it is lost instead of retried).
TEST_CASE("QueueingDispatcher keeps a call whose replay still cannot reach the server, and the calls behind it",
          "[ui][offline]") {
    Rig rig;
    auto const first = rig.send("MoveTaskPosition");
    rig.unreachable(0);
    auto const second = rig.send("AddComment");
    rig.replay();
    rig.unreachable(1);
    CHECK(rig.queueing.held() == 2);
    CHECK(rig.scripted.calls.size() == 2);

    rig.replay();
    REQUIRE(rig.scripted.calls.size() == 3);
    CHECK(rig.scripted.calls.at(2).action == "MoveTaskPosition");
    rig.reply(2, "1");
    rig.reply(3, "2");
    CHECK(first->reply == "1");
    CHECK(second->reply == "2");
}

// Mutation: hold a replay the server refused (onError never runs, and the queue never drains).
TEST_CASE("QueueingDispatcher fails a caller whose replay the server refuses, and replays the next",
          "[ui][offline]") {
    Rig rig;
    auto const refused = rig.send("MoveTaskPosition");
    rig.unreachable(0);
    auto const next = rig.send("AddComment");
    rig.replay();
    rig.scripted.fail(1, "conflict", "the column is full");
    rig.owner.runAll();
    CHECK(refused->failure.has_value());
    REQUIRE(rig.scripted.calls.size() == 3);
    rig.reply(2, "2");
    CHECK(next->reply == "2");
    CHECK(rig.queueing.held() == 0);
}

// Mutation: hold every call that cannot reach the server, queued or not.
TEST_CASE("QueueingDispatcher never holds a call that does not ask to be queued", "[ui][offline]") {
    Rig rig;
    auto const plain = rig.send("GetBoardState", CallOptions{});
    rig.unreachable(0);
    CHECK(plain->failure.has_value());
    CHECK(rig.queueing.held() == 0);
}
