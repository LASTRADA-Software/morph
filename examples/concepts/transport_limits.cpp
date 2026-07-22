// SPDX-License-Identifier: Apache-2.0
//
// Concept: transport-level resource limits (morph::backend::LimitPolicy) — an
// opt-in cap a RemoteServer enforces regardless of which transport carries its
// wire envelopes (in-process simulation, WebSocket, raw socket, …).
//
// `LimitPolicy` is all-zero ("unbounded") by default, matching the server's
// pre-existing behavior exactly until a deployer calls `setLimitPolicy()`.
// This example demonstrates `maxLiveModels`: the simplest knob to exercise
// deterministically, with no timing involved (unlike `executeTimeout`/
// `maxInFlightExecutes`, which need a slow action running concurrently).
//
// Full design reference: docs/spec/core/backend.md ("`LimitPolicy` — opt-in
// resource limits").

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <string>

namespace {

// Runs every posted task synchronously, so RemoteServer::handle() resolves
// before it returns — keeps this example free of polling boilerplate.
struct InlineExecutor : morph::exec::IExecutor {
    void post(std::function<void()> task) override { task(); }
};

struct CapturedReply {
    morph::wire::Envelope env;
    void operator()(const std::string& raw) { env = morph::wire::decode(raw); }
};

}  // namespace

// "LimitsDemo" is this file's unique type-id prefix.

struct LimitsDemoAction {
    int x = 0;
};
struct LimitsDemoModel {
    int execute(const LimitsDemoAction& action) { return action.x; }
};

BRIDGE_REGISTER_MODEL(LimitsDemoModel, "LimitsDemo_Model")
BRIDGE_REGISTER_ACTION(LimitsDemoModel, LimitsDemoAction, "LimitsDemo_Action")

TEST_CASE("transport limits: maxLiveModels rejects a register beyond the cap", "[concepts][limits]") {
    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    morph::backend::LimitPolicy policy;
    policy.maxLiveModels = 2;  // this server never holds more than 2 instances live at once
    server->setLimitPolicy(policy);

    CapturedReply first;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LimitsDemo_Model")), std::ref(first));
    REQUIRE(first.env.kind == "ok");

    CapturedReply second;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LimitsDemo_Model")), std::ref(second));
    REQUIRE(second.env.kind == "ok");

    // The 3rd register exceeds the cap and is rejected — no instance is created.
    CapturedReply third;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LimitsDemo_Model")), std::ref(third));
    REQUIRE(third.env.kind == "err");
    REQUIRE(third.env.message == "too many models");

    // Freeing a slot (deregistering one live instance) lets a new register through again.
    CapturedReply dereg;
    server->handle(morph::wire::encode(morph::wire::makeDeregister(first.env.modelId)), std::ref(dereg));
    REQUIRE(dereg.env.kind == "ok");

    CapturedReply fourth;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LimitsDemo_Model")), std::ref(fourth));
    REQUIRE(fourth.env.kind == "ok");
}

TEST_CASE("transport limits: an unconfigured server imposes no cap (opt-in, not a default restriction)",
          "[concepts][limits]") {
    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    // No setLimitPolicy() call at all: registering many instances just works,
    // exactly as it did before LimitPolicy existed.

    for (int i = 0; i < 10; ++i) {
        CapturedReply reply;
        server->handle(morph::wire::encode(morph::wire::makeRegister("LimitsDemo_Model")), std::ref(reply));
        REQUIRE(reply.env.kind == "ok");
    }
}
