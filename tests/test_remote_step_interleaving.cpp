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
#include <functional>
#include <memory>
#include <string>

#include "test_support.hpp"

// Named StepILOrder* (not the more obvious OrderAction/OrderModel) because
// test_conflict_resolution.cpp already declares its own, unrelated OrderModel
// at external linkage -- two same-named external-linkage types with different
// definitions is an ODR violation the linker does not diagnose, and which
// definition the linker keeps is compiler/link-order dependent (confirmed:
// this silently made OrderModel not backend-change-aware in some builds,
// since the type actually linked into some translation units was this file's
// bare struct, not test_conflict_resolution.cpp's onBackendChanged()-bearing
// one -- see that file's own history for the resulting flaky-then-hard
// failure this caused before the collision was found and fixed).
struct StepILOrderAction {
    int tag = 0;
};
struct StepILOrderModel {
    int execute(const StepILOrderAction& action) { return action.tag; }
};

BRIDGE_REGISTER_MODEL(StepILOrderModel, "StepIL_OrderModel")
BRIDGE_REGISTER_ACTION(StepILOrderModel, StepILOrderAction, "StepIL_OrderAction")

namespace {

/// @brief Registers a fresh `StepILOrderModel` instance and returns the
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

TEST_CASE("morph::testing::StepExecutor: runAll() throws instead of hanging on a task that reposts indefinitely",
          "[step-executor]") {
    // A task that always re-posts itself never leaves the queue empty, so an
    // unbounded `while (runOne())` would spin forever with no assertion
    // failure and no compile-time signal -- exactly the failure mode a
    // hand-stepping harness must not have. runAll()'s maxSteps bound turns
    // that into a loud, immediate exception instead.
    morph::testing::StepExecutor exec;
    std::function<void()> reArm = [&exec, &reArm] { exec.post(reArm); };
    exec.post(reArm);

    REQUIRE_THROWS_AS(exec.runAll(/*maxSteps=*/50), std::runtime_error);
}
