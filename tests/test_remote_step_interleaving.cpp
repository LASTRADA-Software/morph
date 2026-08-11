// SPDX-License-Identifier: Apache-2.0

// Covers the public deterministic-interleaving-harness seam for `RemoteServer`
// (issue #55, use case 2): hand-stepping `RemoteServer`'s real per-model
// ordering via `morph::testing::StepExecutor`, without naming
// `morph::exec::detail::StrandExecutor` or `morph::exec::detail::ModelId`.

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

#include "test_support.hpp"

struct OrderAction {
    int tag = 0;
};
struct OrderModel {
    int execute(const OrderAction& action) { return action.tag; }
};

BRIDGE_REGISTER_MODEL(OrderModel, "StepIL_OrderModel")
BRIDGE_REGISTER_ACTION(OrderModel, OrderAction, "StepIL_OrderAction")

namespace {

/// @brief Registers a fresh `OrderModel` instance and returns the
///        server-assigned model id as a plain `uint64_t` -- the wire's own
///        vocabulary, never `morph::exec::detail::ModelId`.
uint64_t registerModel(const std::shared_ptr<morph::backend::RemoteServer>& server, morph::testing::StepExecutor& exec) {
    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(morph::wire::makeRegister("StepIL_OrderModel")), std::ref(waiter));
    exec.runAll();
    REQUIRE(waiter.ready.load());
    REQUIRE(waiter.env.kind == "ok");
    return waiter.env.modelId;
}

morph::wire::Envelope executeEnvelope(uint64_t modelId, int tag, uint64_t callId) {
    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = callId;
    req.modelId = modelId;
    req.modelType = "StepIL_OrderModel";
    req.actionType = "StepIL_OrderAction";
    req.body = R"({"tag":)" + std::to_string(tag) + "}";
    return req;
}

}  // namespace

TEST_CASE("morph::testing::StepExecutor: hand-stepping RemoteServer preserves per-model submission order",
          "[remote][step-executor]") {
    morph::testing::StepExecutor exec;
    auto server = std::make_shared<morph::backend::RemoteServer>(exec);

    uint64_t const modelId = registerModel(server, exec);

    morph::testing::WaitReply replyA;
    morph::testing::WaitReply replyB;

    server->handle(morph::wire::encode(executeEnvelope(modelId, 1, 10)), std::ref(replyA));
    server->handle(morph::wire::encode(executeEnvelope(modelId, 2, 11)), std::ref(replyB));

    // Both `handle()` calls above only queued a task on `exec` -- nothing has
    // run yet. Hand-step one task at a time until both replies land. Because
    // both executes target the *same* model, RemoteServer's internal strand
    // (itself just another consumer of `exec`) guarantees A's dispatch fully
    // finishes -- including posting its reply -- before B's action ever runs,
    // even though every task, for every model, funnels through this one
    // single-stepped executor.
    for (int i = 0; i < 20 && (!replyA.ready.load() || !replyB.ready.load()); ++i) {
        if (!exec.runOne()) {
            break;
        }
    }

    REQUIRE(replyA.ready.load());
    REQUIRE(replyB.ready.load());
    REQUIRE(replyA.env.kind == "ok");
    REQUIRE(replyB.env.kind == "ok");
    REQUIRE(replyA.env.callId == 10U);
    REQUIRE(replyB.env.callId == 11U);
}

TEST_CASE("morph::testing::StepExecutor: two different models' work can be interleaved by the test, on demand",
          "[remote][step-executor]") {
    morph::testing::StepExecutor exec;
    auto server = std::make_shared<morph::backend::RemoteServer>(exec);

    uint64_t const modelA = registerModel(server, exec);
    uint64_t const modelB = registerModel(server, exec);
    REQUIRE(modelA != modelB);

    morph::testing::WaitReply replyA;
    morph::testing::WaitReply replyB;

    // Submit B's execute first, then A's -- the test decides the interleaving
    // by choosing which queued task to run next, not by submission order.
    server->handle(morph::wire::encode(executeEnvelope(modelB, 20, 21)), std::ref(replyB));
    server->handle(morph::wire::encode(executeEnvelope(modelA, 30, 31)), std::ref(replyA));

    // Drain everything queued so far -- both dispatch-into-strand steps and
    // both strand executions, for both independent models.
    exec.runAll();

    REQUIRE(replyA.ready.load());
    REQUIRE(replyB.ready.load());
    REQUIRE(replyA.env.callId == 31U);
    REQUIRE(replyB.env.callId == 21U);
}
