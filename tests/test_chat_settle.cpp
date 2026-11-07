// SPDX-License-Identifier: Apache-2.0
//
// What server-hosted chat rests on: a call with no connection, carrying a client's session and the id of that client's
// instance, runs on that instance and reaches that client; and a registration can give an instance a log of its own.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model.hpp>
#include <morph/core/records.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_support.hpp"

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace chat_settle {

struct Note {
    std::int64_t id = 0;
    std::string text;
};
struct SetText {
    std::string text;
};
struct GetText {};
struct AddNote {
    std::string text;
};

// Private state: only a call on this very instance can change what `GetText` answers.
struct WorkModel {
    std::string text;
    std::string execute(SetText const& action) {
        text = action.text;
        return text;
    }
    std::string execute(GetText const& /*action*/) const { return text; }
    Note execute(AddNote const& action) const { return Note{.id = 1, .text = action.text}; }
};

struct LoggedAdd {
    std::string text;
};
struct LoggedModel {
    std::string execute(LoggedAdd const& action) const { return action.text; }
};

}  // namespace chat_settle
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(chat_settle::WorkModel, "Settle_Work")
BRIDGE_REGISTER_ACTION(chat_settle::WorkModel, chat_settle::SetText, "Settle_Set")
BRIDGE_REGISTER_EFFECTS(chat_settle::SetText, morph::effects::changesInstance)
BRIDGE_REGISTER_ACTION(chat_settle::WorkModel, chat_settle::GetText, "Settle_Get")
BRIDGE_REGISTER_ACTION(chat_settle::WorkModel, chat_settle::AddNote, "Settle_AddNote")
BRIDGE_REGISTER_RECORD(chat_settle::Note, "Settle_Note")
BRIDGE_REGISTER_EFFECTS(chat_settle::AddNote, morph::effects::changes<chat_settle::Note>)
BRIDGE_REGISTER_MODEL(chat_settle::LoggedModel, "Settle_Logged")
BRIDGE_REGISTER_ACTION(chat_settle::LoggedModel, chat_settle::LoggedAdd, "Settle_LoggedAdd")

namespace {

using namespace std::chrono_literals;
namespace wire = morph::wire;

struct Authorizer final : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(morph::session::Context const& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return true;
    }
    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        return ctx.token.empty() ? std::nullopt : std::optional{ctx.token};
    }
};

// Negotiates events on the connection it is asked on, as the UI service's `ui-hello` does.
struct HelloService final : morph::backend::IEnvelopeService {
    [[nodiscard]] bool serves(std::string_view kind) const override { return kind == "settle-hello"; }
    [[nodiscard]] std::string handle(wire::Envelope const& request,
                                     morph::session::IAuthorizer const& /*authorizer*/) override {
        return wire::encode(wire::makeOk(request.callId));
    }
    [[nodiscard]] std::string handle(wire::Envelope const& request, morph::session::IAuthorizer const& authorizer,
                                     morph::backend::ConnectionFacts& facts) override {
        facts.events = true;
        return handle(request, authorizer);
    }
};

morph::backend::ServerConfig settleConfig() {
    morph::backend::ServerConfig config;
    config.services.push_back(std::make_shared<HelloService>());
    return config;
}

struct EventWire {
    std::mutex mutex;
    std::vector<std::string> sent;

    morph::backend::ConnectionWriter writer() {
        return {.write =
                    [this](std::string frame) {
                        std::scoped_lock const lock{mutex};
                        sent.push_back(std::move(frame));
                    },
                .backlog = [] { return std::size_t{0}; }};
    }

    std::vector<std::string> frames() {
        std::scoped_lock const lock{mutex};
        return sent;
    }
};

bool waitFor(auto done) {
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

struct Rig {
    morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<morph::backend::RemoteServer> server =
        std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<Authorizer>(), settleConfig());
    EventWire wire;

    wire::Envelope call(wire::Envelope env, morph::backend::ConnectionId cid) {
        static std::atomic<std::uint64_t> nextCall{100};
        env.callId = nextCall.fetch_add(1);
        env.session.token = "alice";
        auto replied = std::make_shared<std::promise<std::string>>();
        auto answer = replied->get_future();
        server->handle(wire::encode(env), [replied](std::string const& text) { replied->set_value(text); }, cid);
        REQUIRE(answer.wait_for(5s) == std::future_status::ready);
        return wire::decode(answer.get());
    }

    // A GUI connection: it holds a writer the server's events reach.
    morph::backend::ConnectionId gui() {
        auto const cid = server->openConnection();
        server->setConnectionWriter(cid, wire.writer());
        return cid;
    }

    std::uint64_t privateInstance(std::string typeId, morph::backend::ConnectionId cid) {
        auto const reply = call(wire::makeRegister(std::move(typeId)), cid);
        REQUIRE(reply.kind == "ok");
        REQUIRE(reply.modelId != 0);
        return reply.modelId;
    }

    wire::Envelope run(std::uint64_t modelId, std::string modelType, std::string action, std::string body,
                       morph::backend::ConnectionId cid) {
        wire::Envelope env;
        env.kind = "execute";
        env.modelId = modelId;
        env.modelType = std::move(modelType);
        env.actionType = std::move(action);
        env.body = std::move(body);
        return call(std::move(env), cid);
    }
};

void negotiate(Rig& rig, morph::backend::ConnectionId cid, std::string const& watchBody) {
    wire::Envelope hello;
    hello.kind = "settle-hello";
    REQUIRE(rig.call(std::move(hello), cid).kind == "ok");
    wire::Envelope watch;
    watch.kind = std::string{wire::kWatchKind};
    watch.body = watchBody;
    REQUIRE(rig.call(std::move(watch), cid).kind == "ok");
}

}  // namespace

TEST_CASE("unscoped_execute_reaches_a_connections_private_instance", "[chat][settle]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const instance = rig.privateInstance("Settle_Work", cid);

    REQUIRE(rig.run(instance, "Settle_Work", "Settle_Set", R"({"text":"hello"})", 0).kind == "ok");

    auto const read = rig.run(instance, "Settle_Work", "Settle_Get", "{}", cid);
    REQUIRE(read.kind == "ok");
    CHECK(read.body.find("hello") != std::string::npos);
}

TEST_CASE("unscoped_execute_on_a_private_instance_reaches_the_gui_as_an_event", "[chat][settle]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const instance = rig.privateInstance("Settle_Work", cid);
    negotiate(rig, cid,
              R"({"records":[],"instances":[{"modelType":"Settle_Work","primary":"","instance":)" +
                  std::to_string(instance) + "}]}");

    REQUIRE(rig.run(instance, "Settle_Work", "Settle_Set", R"({"text":"hello"})", 0).kind == "ok");

    REQUIRE(waitFor([&] { return !rig.wire.frames().empty(); }));
    auto const event = wire::decode(rig.wire.frames().front());
    CHECK(event.kind == wire::kEventKind);
    CHECK(event.body.find(std::to_string(instance)) != std::string::npos);
}

TEST_CASE("a_records_change_from_an_unscoped_execute_reaches_a_watcher_with_the_same_principal", "[chat][settle]") {
    Rig rig;
    auto const cid = rig.gui();
    auto const instance = rig.privateInstance("Settle_Work", cid);
    negotiate(rig, cid, R"({"records":["Settle_Note"],"instances":[]})");

    REQUIRE(rig.run(instance, "Settle_Work", "Settle_AddNote", R"({"text":"hello"})", 0).kind == "ok");

    REQUIRE(waitFor([&] { return !rig.wire.frames().empty(); }));
    CHECK(wire::decode(rig.wire.frames().front()).body.find("Settle_Note") != std::string::npos);
}

TEST_CASE("a_registration_attaching_a_log_gives_exactly_one_entry_per_action", "[chat][settle]") {
    auto const log = std::make_shared<morph::journal::InMemoryActionLog>(morph::testing::storageOwner());
    morph::model::detail::defaultRegistry().registerModel<chat_settle::LoggedModel>("Settle_Logged", [log] {
        auto holder = std::make_unique<morph::model::detail::ModelHolder<chat_settle::LoggedModel>>();
        holder->attachActionLog(log, {});
        return holder;
    });
    Rig rig;
    auto const cid = rig.gui();
    auto const instance = rig.privateInstance("Settle_Logged", cid);

    REQUIRE(rig.run(instance, "Settle_Logged", "Settle_LoggedAdd", R"({"text":"hello"})", 0).kind == "ok");

    // The log's owner is this thread, so it runs the append.
    REQUIRE(waitFor([&] {
        morph::testing::storageOwner().runFor(5ms);
        return !log->entries().empty();
    }));
    REQUIRE(log->entries().size() == 1);
    CHECK(log->entries().front().actionType == "Settle_LoggedAdd");
    CHECK(log->entries().front().principal == "alice");
}
