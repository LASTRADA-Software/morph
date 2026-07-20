// SPDX-License-Identifier: Apache-2.0

// Coverage for morph::backend::RemoteServer::LimitPolicy — the opt-in,
// connection-agnostic resource limits (maxLiveModels, maxInFlightExecutes,
// executeTimeout). See docs/spec/core/backend.md.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <mutex>
#include <string>
#include <thread>

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

// ── maxInFlightExecutes ───────────────────────────────────────────────────────

namespace {
std::atomic<int> gLPSlowStarted{0};
}  // namespace

struct LPSlowAction {
    int ms = 0;
};
struct LPSlowModel {
    int execute(const LPSlowAction& act) {
        gLPSlowStarted.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(act.ms));
        return act.ms;
    }
};

BRIDGE_REGISTER_MODEL(LPSlowModel, "LP_SlowModel")
BRIDGE_REGISTER_ACTION(LPSlowModel, LPSlowAction, "LP_SlowAction")

TEST_CASE("LimitPolicy: maxInFlightExecutes rejects a second execute while the first is in flight",
          "[limits][limit-policy]") {
    gLPSlowStarted.store(0, std::memory_order_relaxed);
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::backend::LimitPolicy policy;
    policy.maxInFlightExecutes = 1;
    server->setLimitPolicy(policy);

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LP_SlowModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    auto mid = regReply.env.modelId;

    morph::wire::Envelope slow;
    slow.kind = "execute";
    slow.callId = 1;
    slow.modelId = mid;
    slow.modelType = "LP_SlowModel";
    slow.actionType = "LP_SlowAction";
    slow.body = R"({"ms":150})";

    morph::testing::WaitReply firstExec;
    server->handle(morph::wire::encode(slow), std::ref(firstExec));

    // Wait until the slow action has actually started running on its strand —
    // dispatchExecute increments the in-flight counter strictly before posting
    // to the strand, so by the time the model body runs the counter is
    // guaranteed to already reflect this call.
    REQUIRE(morph::testing::waitUntil([] { return gLPSlowStarted.load(std::memory_order_relaxed) >= 1; }));

    morph::wire::Envelope fast;
    fast.kind = "execute";
    fast.callId = 2;
    fast.modelId = mid;
    fast.modelType = "LP_SlowModel";
    fast.actionType = "LP_SlowAction";
    fast.body = R"({"ms":0})";

    morph::testing::WaitReply secondExec;
    server->handle(morph::wire::encode(fast), std::ref(secondExec));
    REQUIRE(secondExec.await());
    REQUIRE(secondExec.env.kind == "err");
    REQUIRE(secondExec.env.message == "server busy");

    // The first call still completes normally once its sleep elapses.
    REQUIRE(firstExec.await(2s));
    REQUIRE(firstExec.env.kind == "ok");

    // Now that the first has finished, a fresh call is admitted again.
    morph::wire::Envelope again = fast;
    again.callId = 3;
    morph::testing::WaitReply thirdExec;
    server->handle(morph::wire::encode(again), std::ref(thirdExec));
    REQUIRE(thirdExec.await());
    REQUIRE(thirdExec.env.kind == "ok");
}

// ── executeTimeout ────────────────────────────────────────────────────────────

TEST_CASE("LimitPolicy: executeTimeout replies err \"timeout\" and the late strand result is discarded",
          "[limits][limit-policy]") {
    gLPSlowStarted.store(0, std::memory_order_relaxed);
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::backend::LimitPolicy policy;
    policy.executeTimeout = 50ms;
    server->setLimitPolicy(policy);

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LP_SlowModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    auto mid = regReply.env.modelId;

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 1;
    req.modelId = mid;
    req.modelType = "LP_SlowModel";
    req.actionType = "LP_SlowAction";
    req.body = R"({"ms":300})";  // well past the 50ms executeTimeout

    std::mutex m;
    int replyCount = 0;
    std::string firstKind;
    std::string firstMessage;
    server->handle(morph::wire::encode(req), [&](const std::string& raw) {
        auto env = morph::wire::decode(raw);
        std::scoped_lock lock{m};
        if (replyCount == 0) {
            firstKind = env.kind;
            firstMessage = env.message;
        }
        ++replyCount;
    });

    REQUIRE(morph::testing::waitUntil([&] {
        std::scoped_lock lock{m};
        return replyCount >= 1;
    }));
    {
        std::scoped_lock lock{m};
        REQUIRE(firstKind == "err");
        REQUIRE(firstMessage == "timeout");
    }

    // The slow action keeps running to completion on its strand and eventually
    // replies too; give it time to do so, then confirm no *second* reply was
    // delivered (the once-flag guard discards it — reply-exactly-once holds).
    std::this_thread::sleep_for(400ms);
    {
        std::scoped_lock lock{m};
        REQUIRE(replyCount == 1);
    }
}

TEST_CASE("LimitPolicy: default executeTimeout (0) never times out a slow action (regression)",
          "[limits][limit-policy]") {
    gLPSlowStarted.store(0, std::memory_order_relaxed);
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    // No setLimitPolicy() call: executeTimeout defaults to 0 (disabled).

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("LP_SlowModel")), std::ref(regReply));
    REQUIRE(regReply.await());

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 1;
    req.modelId = regReply.env.modelId;
    req.modelType = "LP_SlowModel";
    req.actionType = "LP_SlowAction";
    req.body = R"({"ms":100})";

    morph::testing::WaitReply exec;
    server->handle(morph::wire::encode(req), std::ref(exec));
    REQUIRE(exec.await(2s));
    REQUIRE(exec.env.kind == "ok");
}

TEST_CASE("LimitPolicy: executeTimeout surfaces as backend::TimeoutError through SimulatedRemoteBackend",
          "[limits][limit-policy]") {
    gLPSlowStarted.store(0, std::memory_order_relaxed);
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::backend::LimitPolicy policy;
    policy.executeTimeout = 50ms;
    server->setLimitPolicy(policy);

    morph::backend::SimulatedRemoteBackend backend{*server};
    auto mid = backend.registerModelWithContext("LP_SlowModel", nullptr, {});

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "LP_SlowModel";
    call.actionTypeId = "LP_SlowAction";
    call.serializeAction = [] { return std::string{R"({"ms":300})"}; };
    call.deserializeResult = [](std::string_view json) -> std::shared_ptr<void> {
        auto result = std::make_shared<int>(0);
        (void)glz::read_json(*result, json);
        return result;
    };

    morph::exec::ThreadPoolExecutor cbPool{1};
    auto completion = backend.execute(mid, std::move(call), &cbPool);

    std::atomic<bool> gotTimeoutError{false};
    completion.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const morph::backend::TimeoutError&) {
            gotTimeoutError.store(true);
        } catch (...) {
        }
    });

    REQUIRE(morph::testing::waitUntil([&] { return gotTimeoutError.load(); }, 2s));
}
