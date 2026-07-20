// SPDX-License-Identifier: Apache-2.0

// Coverage for morph::backend::RemoteServer::LimitPolicy — the opt-in,
// connection-agnostic resource limits (maxLiveModels, maxInFlightExecutes,
// executeTimeout). See docs/spec/core/backend.md.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <string>

#include "test_support.hpp"

using namespace std::chrono_literals;

// ── Fixture models ────────────────────────────────────────────────────────────

// Must have external linkage so Glaze's reflection can mangle the type name.
struct LPEchoAction {
    std::string s;
};
struct LPEchoModel {
    std::string execute(const LPEchoAction& act) { return act.s; }
};

BRIDGE_REGISTER_MODEL(LPEchoModel, "LP_EchoModel")
BRIDGE_REGISTER_ACTION(LPEchoModel, LPEchoAction, "LP_EchoAction")

// ── maxLiveModels ─────────────────────────────────────────────────────────────

TEST_CASE("LimitPolicy: default policy imposes no cap on registers (regression)", "[limits][limit-policy]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    // No setLimitPolicy() call at all: an unconfigured server must behave
    // exactly as it did before this feature existed.
    for (int i = 0; i < 50; ++i) {
        morph::testing::WaitReply waiter;
        server->handle(morph::wire::encode(morph::wire::makeRegister("LP_EchoModel")), std::ref(waiter));
        REQUIRE(waiter.await());
        REQUIRE(waiter.env.kind == "ok");
    }
}

TEST_CASE("LimitPolicy: maxLiveModels rejects register beyond the cap", "[limits][limit-policy]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::backend::LimitPolicy policy;
    policy.maxLiveModels = 2;
    server->setLimitPolicy(policy);

    morph::testing::WaitReply first;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LP_EchoModel")), std::ref(first));
    REQUIRE(first.await());
    REQUIRE(first.env.kind == "ok");

    morph::testing::WaitReply second;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LP_EchoModel")), std::ref(second));
    REQUIRE(second.await());
    REQUIRE(second.env.kind == "ok");

    morph::testing::WaitReply third;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LP_EchoModel")), std::ref(third));
    REQUIRE(third.await());
    REQUIRE(third.env.kind == "err");
    REQUIRE(third.env.message == "too many models");

    // Deregistering one frees a slot for a subsequent register.
    morph::testing::WaitReply dereg;
    server->handle(morph::wire::encode(morph::wire::makeDeregister(first.env.modelId)), std::ref(dereg));
    REQUIRE(dereg.await());
    REQUIRE(dereg.env.kind == "ok");

    morph::testing::WaitReply fourth;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LP_EchoModel")), std::ref(fourth));
    REQUIRE(fourth.await());
    REQUIRE(fourth.env.kind == "ok");
}
