// SPDX-License-Identifier: Apache-2.0

// Coverage for morph::backend::RemoteServer::beginShutdown() / drainedWithin()
// — the opt-in graceful-shutdown-and-drain sequence. See
// docs/spec/core/backend.md.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <string>
#include <thread>
#include <vector>

#include "test_support.hpp"

using namespace std::chrono_literals;

// ── Fixture models ────────────────────────────────────────────────────────────

// Must have external linkage so Glaze's reflection can mangle the type name.
struct GSEchoAction {
    std::string s;
};
struct GSEchoModel {
    std::string execute(const GSEchoAction& act) { return act.s; }
};

BRIDGE_REGISTER_MODEL(GSEchoModel, "GS_EchoModel")
BRIDGE_REGISTER_ACTION(GSEchoModel, GSEchoAction, "GS_EchoAction")

namespace {
std::atomic<int> gGSSlowStarted{0};
}  // namespace

struct GSSlowAction {
    int ms = 0;
};
struct GSSlowModel {
    int execute(const GSSlowAction& act) {
        gGSSlowStarted.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(act.ms));
        return act.ms;
    }
};

BRIDGE_REGISTER_MODEL(GSSlowModel, "GS_SlowModel")
BRIDGE_REGISTER_ACTION(GSSlowModel, GSSlowAction, "GS_SlowAction")

// ── beginShutdown() ───────────────────────────────────────────────────────────

TEST_CASE("RemoteServer: register/execute succeed normally before beginShutdown (regression)",
          "[shutdown][graceful]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("GS_EchoModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    REQUIRE(regReply.env.kind == "ok");
}

TEST_CASE("RemoteServer::beginShutdown rejects register with \"server shutting down\"", "[shutdown][graceful]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->beginShutdown();

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("GS_EchoModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    REQUIRE(regReply.env.kind == "err");
    REQUIRE(regReply.env.message == "server shutting down");
}

TEST_CASE("RemoteServer::beginShutdown rejects execute with \"server shutting down\"", "[shutdown][graceful]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("GS_EchoModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    auto mid = regReply.env.modelId;

    server->beginShutdown();

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 1;
    req.modelId = mid;
    req.modelType = "GS_EchoModel";
    req.actionType = "GS_EchoAction";
    req.body = R"({"s":"hi"})";

    morph::testing::WaitReply execReply;
    server->handle(morph::wire::encode(req), std::ref(execReply));
    REQUIRE(execReply.await());
    REQUIRE(execReply.env.kind == "err");
    REQUIRE(execReply.env.message == "server shutting down");
    REQUIRE(execReply.env.callId == 1U);
}

TEST_CASE("RemoteServer::beginShutdown still serves deregister", "[shutdown][graceful]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("GS_EchoModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    auto mid = regReply.env.modelId;

    server->beginShutdown();

    morph::testing::WaitReply deregReply;
    server->handle(morph::wire::encode(morph::wire::makeDeregister(mid)), std::ref(deregReply));
    REQUIRE(deregReply.await());
    REQUIRE(deregReply.env.kind == "ok");
}

TEST_CASE("RemoteServer::beginShutdown is idempotent", "[shutdown][graceful]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->beginShutdown();
    server->beginShutdown();  // must not throw or change behavior

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("GS_EchoModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    REQUIRE(regReply.env.kind == "err");
    REQUIRE(regReply.env.message == "server shutting down");
}

// ── beginShutdown() / health() wiring ─────────────────────────────────────────

TEST_CASE("RemoteServer::beginShutdown flips health().ready to false", "[shutdown][graceful][health]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    REQUIRE(server->health().ready);

    server->beginShutdown();

    REQUIRE_FALSE(server->health().ready);
}

TEST_CASE("RemoteServer::beginShutdown re-invokes the installed health handler with ready == false",
          "[shutdown][graceful][health]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    std::vector<bool> observedReady;
    server->setHealthHandler(
        [&](const morph::backend::HealthStatus& status) { observedReady.push_back(status.ready); });
    REQUIRE(observedReady == std::vector<bool>{true});  // fired immediately on install

    server->beginShutdown();

    REQUIRE(observedReady == (std::vector<bool>{true, false}));
}

// ── drainedWithin() ───────────────────────────────────────────────────────────

TEST_CASE("RemoteServer::drainedWithin returns true immediately when nothing is in flight",
          "[shutdown][graceful][drain]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    REQUIRE(server->drainedWithin(0ms));
}

TEST_CASE("RemoteServer::drainedWithin blocks until a slow in-flight execute delivers its reply",
          "[shutdown][graceful][drain]") {
    gGSSlowStarted.store(0, std::memory_order_relaxed);
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("GS_SlowModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    auto mid = regReply.env.modelId;

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 1;
    req.modelId = mid;
    req.modelType = "GS_SlowModel";
    req.actionType = "GS_SlowAction";
    req.body = R"({"ms":150})";

    morph::testing::WaitReply execReply;
    server->handle(morph::wire::encode(req), std::ref(execReply));

    // Wait until the model has actually started running on its strand, so
    // the in-flight counter is guaranteed to be non-zero for the assertion
    // below (dispatchExecute increments it strictly before posting to the
    // strand).
    REQUIRE(morph::testing::waitUntil([] { return gGSSlowStarted.load(std::memory_order_relaxed) >= 1; }));

    // Not drained yet — the slow action is still running.
    REQUIRE_FALSE(server->drainedWithin(10ms));

    // It does complete within a generous deadline, and the reply is delivered.
    REQUIRE(server->drainedWithin(2s));
    REQUIRE(execReply.await());
    REQUIRE(execReply.env.kind == "ok");
}

TEST_CASE("RemoteServer::drainedWithin times out while a slower-than-deadline execute is still running",
          "[shutdown][graceful][drain]") {
    gGSSlowStarted.store(0, std::memory_order_relaxed);
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("GS_SlowModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    auto mid = regReply.env.modelId;

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 1;
    req.modelId = mid;
    req.modelType = "GS_SlowModel";
    req.actionType = "GS_SlowAction";
    req.body = R"({"ms":300})";

    morph::testing::WaitReply execReply;
    server->handle(morph::wire::encode(req), std::ref(execReply));
    REQUIRE(morph::testing::waitUntil([] { return gGSSlowStarted.load(std::memory_order_relaxed) >= 1; }));

    REQUIRE_FALSE(server->drainedWithin(50ms));

    // Let the slow action actually finish so the pool/strand can tear down
    // cleanly at end of scope instead of racing the test fixture teardown.
    REQUIRE(execReply.await(2s));
}

TEST_CASE("RemoteServer: beginShutdown then drainedWithin is the standard stop sequence",
          "[shutdown][graceful][drain]") {
    gGSSlowStarted.store(0, std::memory_order_relaxed);
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    morph::testing::WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("GS_SlowModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    auto mid = regReply.env.modelId;

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 1;
    req.modelId = mid;
    req.modelType = "GS_SlowModel";
    req.actionType = "GS_SlowAction";
    req.body = R"({"ms":100})";

    morph::testing::WaitReply execReply;
    server->handle(morph::wire::encode(req), std::ref(execReply));
    REQUIRE(morph::testing::waitUntil([] { return gGSSlowStarted.load(std::memory_order_relaxed) >= 1; }));

    // The standard sequence: stop taking new work, then wait for old work.
    server->beginShutdown();

    morph::testing::WaitReply rejectedReg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("GS_SlowModel")), std::ref(rejectedReg));
    REQUIRE(rejectedReg.await());
    REQUIRE(rejectedReg.env.kind == "err");
    REQUIRE(rejectedReg.env.message == "server shutting down");

    REQUIRE(server->drainedWithin(2s));
    REQUIRE(execReply.await());
    REQUIRE(execReply.env.kind == "ok");
}
