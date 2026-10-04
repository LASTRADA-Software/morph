// SPDX-License-Identifier: Apache-2.0
//
// The server side of a wire `cancel`: RemoteServer maps it onto the stop
// source of the execute it names, for the connection and principal that made
// the call and nobody else, and answers every cancel with the same `ok`.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <core/async/AsyncQueue.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "test_support.hpp"

using morph::testing::WaitReply;
using morph::testing::waitUntil;

// A named namespace, not an anonymous one: ActionDispatcher::registerAction
// files each action's schema, and glaze's reflection cannot name a type with
// internal linkage under MSVC (C7631).
namespace remote_cancel_test {

struct Park {
    int tag = 0;
};

/// What the handler reports back. The server constructs the model, so the
/// test reaches it through a global.
struct Probe {
    std::atomic<int> started{0};
    std::atomic<int> stopped{0};
    std::atomic<int> finished{0};
    std::unique_ptr<core::async::AsyncQueue<int>> queue;
};

inline Probe& probe() {
    static Probe instance;
    return instance;
}

/// A Task handler that blocks until an item arrives on the probe's queue, or
/// until it is stopped: only a push or a stop can end it.
struct ParkModel {
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    core::async::Task<int> execute(Park action) {
        probe().started.fetch_add(1);
        try {
            auto const item = co_await probe().queue->pop();
            probe().finished.fetch_add(1);
            co_return item.value_or(0) + action.tag;
        } catch (const core::async::OperationCancelled&) {
            probe().stopped.fetch_add(1);
            probe().finished.fetch_add(1);
            throw;
        }
    }
};

}  // namespace remote_cancel_test

using remote_cancel_test::Park;
using remote_cancel_test::ParkModel;
using remote_cancel_test::probe;

template <>
struct morph::model::ModelTraits<ParkModel> {
    static constexpr std::string_view typeId() { return "RC_Park"; }
};
template <>
struct morph::model::ActionTraits<Park> {
    using Result = int;
    static constexpr std::string_view typeId() { return "RC_ParkAction"; }
    static std::string toJson(const Park&) { return "{}"; }
    static Park fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& result) { return std::to_string(result); }
    static int resultFromJson(std::string_view json) { return std::stoi(std::string{json}); }
};

namespace {

/// Derives the principal from the token alone, so a claimed principal that
/// disagrees with the token is visible. Type-level `authorize` admits exactly
/// the callers it can authenticate; ownership admits an unowned instance to
/// everyone and an owned one to its owner.
struct TokenAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] static std::optional<std::string> principalFor(std::string_view token) {
        if (token == "tok-alice") {
            return std::string{"alice"};
        }
        if (token == "tok-bob") {
            return std::string{"bob"};
        }
        return std::nullopt;
    }
    [[nodiscard]] bool authorize(const morph::session::Context& ctx, std::string_view,
                                 std::string_view) const override {
        return principalFor(ctx.token).has_value();
    }
    [[nodiscard]] std::optional<std::string> authenticate(const morph::session::Context& ctx) const override {
        return principalFor(ctx.token);
    }
    [[nodiscard]] bool authorizeInstance(const morph::session::Context& ctx, std::string_view, std::string_view,
                                         std::uint64_t, std::string_view ownerPrincipal) const override {
        return ownerPrincipal.empty() || ownerPrincipal == ctx.principal;
    }
};

/// Who a message claims to come from.
struct Caller {
    std::string token;
    std::string claimed;
};

/// One server with the parking model, and a queue whose consumer is resumed on
/// the executor it parked on.
struct Rig {
    // Declared before the executors, so they outlive every task the pool runs.
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    core::async::ThreadPoolExecutor foreign{1};
    morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<morph::backend::RemoteServer> server;

    explicit Rig(std::shared_ptr<morph::session::IAuthorizer> authorizer = nullptr) {
        registry.registerModel<ParkModel>("RC_Park");
        dispatcher.registerAction<ParkModel, Park>("RC_Park", "RC_ParkAction");
        server = std::make_shared<morph::backend::RemoteServer>(pool, std::move(authorizer), dispatcher, registry);
        probe().started = 0;
        probe().stopped = 0;
        probe().finished = 0;
        probe().queue = std::make_unique<core::async::AsyncQueue<int>>(foreign, core::async::AsyncQueueOptions{});
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;

    // Unparks any handler still waiting -- a case that failed leaves one -- and
    // waits for it to end before the queue goes: a queue must not die under a
    // parked consumer.
    ~Rig() {
        probe().queue->close();
        static_cast<void>(waitUntil([] { return probe().finished.load() == probe().started.load(); }));
        probe().queue.reset();
    }

    [[nodiscard]] morph::wire::Envelope ask(const morph::wire::Envelope& env, morph::backend::ConnectionId cid) const {
        WaitReply reply;
        server->handle(morph::wire::encode(env), std::ref(reply), cid);
        REQUIRE(reply.await());
        return reply.env;
    }

    [[nodiscard]] std::uint64_t registerOn(morph::backend::ConnectionId cid, const Caller& caller) const {
        auto env = morph::wire::makeRegister("RC_Park");
        env.session.token = caller.token;
        env.session.principal = caller.claimed;
        auto const reply = ask(env, cid);
        REQUIRE(reply.kind == "ok");
        return reply.modelId;
    }
};

/// Where an execute is sent, and under which `callId`.
struct Target {
    morph::backend::ConnectionId cid{0};
    std::uint64_t modelId{0};
    std::uint64_t callId{0};
};

morph::wire::Envelope parkCall(const Target& target, const Caller& caller) {
    morph::wire::Envelope env;
    env.kind = "execute";
    env.callId = target.callId;
    env.modelId = target.modelId;
    env.modelType = "RC_Park";
    env.actionType = "RC_ParkAction";
    env.body = "{}";
    env.session.token = caller.token;
    env.session.principal = caller.claimed;
    return env;
}

morph::wire::Envelope cancelOf(std::uint64_t targetCallId, const Caller& caller) {
    auto env = morph::wire::makeCancel(targetCallId);
    env.callId = targetCallId + 1000U;
    env.session.token = caller.token;
    env.session.principal = caller.claimed;
    return env;
}

/// Pushes one item and waits for the parked call's reply. A call nothing
/// stopped answers `ok` with the item; a stopped one has already unwound, so
/// the item reaches nobody and the reply is the stop's `err`.
morph::wire::Envelope releaseAndAwait(WaitReply& call) {
    static_cast<void>(probe().queue->push(41));
    REQUIRE(call.await());
    return call.env;
}

}  // namespace

TEST_CASE("RemoteServer: a cancel stops the Task handler of the call it names", "[remote][cancel]") {
    Rig const rig;
    auto const cid = rig.server->openConnection();
    auto const mid = rig.registerOn(cid, {});

    WaitReply call;
    rig.server->handle(morph::wire::encode(parkCall({.cid = cid, .modelId = mid, .callId = 7}, {})), std::ref(call),
                       cid);
    REQUIRE(waitUntil([] { return probe().started.load() == 1; }));

    auto const cancelled = rig.ask(cancelOf(7, {}), cid);
    // The cancel's own reply, under its own callId: were it addressed to the
    // execute, the client would settle the call with it.
    CHECK(cancelled.kind == "ok");
    CHECK(cancelled.callId == 1007U);

    REQUIRE(call.await());
    CHECK(call.env.kind == "err");
    CHECK(call.env.callId == 7U);
    CHECK(probe().stopped.load() == 1);
}

TEST_CASE("RemoteServer: a cancel from another connection has no effect on the call", "[remote][cancel]") {
    Rig const rig;
    auto const owner = rig.server->openConnection();
    auto const other = rig.server->openConnection();
    auto const mid = rig.registerOn(owner, {});

    WaitReply call;
    rig.server->handle(morph::wire::encode(parkCall({.cid = owner, .modelId = mid, .callId = 7}, {})), std::ref(call),
                       owner);
    REQUIRE(waitUntil([] { return probe().started.load() == 1; }));

    // Same callId, same (empty) principal: only the connection differs.
    auto const foreign = rig.ask(cancelOf(7, {}), other);
    CHECK(foreign.kind == "ok");

    auto const reply = releaseAndAwait(call);
    CHECK(reply.kind == "ok");
    CHECK(reply.body == "41");
    CHECK(probe().stopped.load() == 0);
}

TEST_CASE("RemoteServer: a cancel keys on the verified principal, not the claimed one", "[remote][cancel]") {
    // An unowned instance two principals both use over one connection -- a
    // gateway multiplexing its users -- so `authorizeInstance` admits both,
    // and the principal recorded at the execute is all that tells them apart.
    Rig const rig{std::make_shared<TokenAuthorizer>()};
    auto const cid = rig.server->openConnection();
    auto const mid = rig.registerOn(cid, {.token = {}, .claimed = {}});
    Caller const alice{.token = "tok-alice", .claimed = "alice"};

    WaitReply call;
    rig.server->handle(morph::wire::encode(parkCall({.cid = cid, .modelId = mid, .callId = 7}, alice)), std::ref(call),
                       cid);
    REQUIRE(waitUntil([] { return probe().started.load() == 1; }));

    SECTION("bob's token claiming to be alice has no effect") {
        auto const forged = rig.ask(cancelOf(7, {.token = "tok-bob", .claimed = "alice"}), cid);
        CHECK(forged.kind == "ok");

        auto const reply = releaseAndAwait(call);
        CHECK(reply.kind == "ok");
        CHECK(probe().stopped.load() == 0);
    }

    SECTION("a claim of alice with no token has no effect") {
        auto const tokenless = rig.ask(cancelOf(7, {.token = {}, .claimed = "alice"}), cid);
        CHECK(tokenless.kind == "ok");

        auto const reply = releaseAndAwait(call);
        CHECK(reply.kind == "ok");
        CHECK(probe().stopped.load() == 0);
    }

    SECTION("control: alice's own token stops it, whatever she claims") {
        auto const own = rig.ask(cancelOf(7, {.token = "tok-alice", .claimed = "mallory"}), cid);
        CHECK(own.kind == "ok");

        REQUIRE(call.await());
        CHECK(call.env.kind == "err");
        CHECK(probe().stopped.load() == 1);
    }
}

TEST_CASE("RemoteServer: on an owned instance only the owner's cancel stops the call", "[remote][cancel]") {
    // Alice owns the instance. A cancel carrying alice's verified principal
    // stops it; nothing else reaches it -- not even bob on alice's connection.
    Rig const rig{std::make_shared<TokenAuthorizer>()};
    auto const cid = rig.server->openConnection();
    Caller const alice{.token = "tok-alice", .claimed = "alice"};
    auto const mid = rig.registerOn(cid, alice);

    WaitReply call;
    rig.server->handle(morph::wire::encode(parkCall({.cid = cid, .modelId = mid, .callId = 9}, alice)), std::ref(call),
                       cid);
    REQUIRE(waitUntil([] { return probe().started.load() == 1; }));

    auto const bob = rig.ask(cancelOf(9, {.token = "tok-bob", .claimed = "bob"}), cid);
    CHECK(bob.kind == "ok");
    auto const own = rig.ask(cancelOf(9, alice), cid);
    CHECK(own.kind == "ok");

    REQUIRE(call.await());
    CHECK(call.env.kind == "err");
    CHECK(probe().stopped.load() == 1);
}

TEST_CASE("RemoteServer: a cancel for an unknown, finished or unscoped call is a no-op ok", "[remote][cancel]") {
    Rig const rig;
    auto const cid = rig.server->openConnection();
    auto const mid = rig.registerOn(cid, {});

    SECTION("unknown") {
        auto const reply = rig.ask(cancelOf(12345, {}), cid);
        CHECK(reply.kind == "ok");
        CHECK(reply.callId == 13345U);
    }

    SECTION("finished") {
        WaitReply call;
        rig.server->handle(morph::wire::encode(parkCall({.cid = cid, .modelId = mid, .callId = 3}, {})),
                           std::ref(call), cid);
        REQUIRE(waitUntil([] { return probe().started.load() == 1; }));
        CHECK(releaseAndAwait(call).kind == "ok");

        auto const reply = rig.ask(cancelOf(3, {}), cid);
        CHECK(reply.kind == "ok");
    }

    SECTION("unscoped: a message with no connection names no call") {
        auto const unscoped = rig.registerOn(0, {});
        WaitReply call;
        rig.server->handle(morph::wire::encode(parkCall({.cid = 0, .modelId = unscoped, .callId = 5}, {})),
                           std::ref(call), 0);
        REQUIRE(waitUntil([] { return probe().started.load() == 1; }));

        CHECK(rig.ask(cancelOf(5, {}), 0).kind == "ok");

        auto const reply = releaseAndAwait(call);
        CHECK(reply.kind == "ok");
        CHECK(probe().stopped.load() == 0);
    }
}

TEST_CASE("RemoteServer: hello advertises the cancel capability", "[remote][cancel][protocol]") {
    Rig const rig;
    auto const reply = rig.ask(morph::wire::makeHello(), 0);
    REQUIRE(reply.kind == "ok");
    CHECK(morph::wire::helloAdvertises(reply, morph::wire::kCapabilityCancel));
}

TEST_CASE("wire::helloAdvertises: a server that predates the capability list advertises nothing",
          "[wire][cancel][protocol]") {
    CHECK_FALSE(
        morph::wire::helloAdvertises(morph::wire::makeOk(1, R"({"min":1,"max":1})"), morph::wire::kCapabilityCancel));
    CHECK_FALSE(morph::wire::helloAdvertises(morph::wire::makeErr("unknown envelope kind: hello", 1),
                                             morph::wire::kCapabilityCancel));
    CHECK_FALSE(morph::wire::helloAdvertises(morph::wire::makeOk(1, "not json"), morph::wire::kCapabilityCancel));
    CHECK(morph::wire::helloAdvertises(morph::wire::makeOk(1, R"({"min":1,"max":1,"capabilities":["cancel"]})"),
                                       morph::wire::kCapabilityCancel));
}

TEST_CASE("wire::makeCancel carries the target in cancelCallId and round-trips", "[wire][cancel]") {
    auto env = morph::wire::makeCancel(77);
    env.callId = 78;
    auto const decoded = morph::wire::decode(morph::wire::encode(env));
    CHECK(decoded.kind == "cancel");
    CHECK(decoded.callId == 78U);
    CHECK(decoded.cancelCallId == 77U);
}
