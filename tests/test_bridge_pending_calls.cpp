// SPDX-License-Identifier: Apache-2.0

// Regression coverage for issue #45: Bridge has no pendingCalls() for
// client-side quiescence observability. These tests exercise the in-flight
// counter tracked by Bridge::executeVia() (dispatched via
// BridgeHandler::execute()), incremented on dispatch and decremented when the
// returned Completion resolves — on success, on error, and when the handler
// is not bound (immediate synchronous failure). See docs/spec/core/bridge.md.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <stdexcept>
#include <thread>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;
using namespace std::chrono_literals;

namespace {
std::atomic<int> gPendingCallsSlowStarted{0};
std::atomic<bool> gPendingCallsSlowRelease{false};
}  // namespace

struct PCFastAction {
    int value = 0;
};
struct PCFailAction {};
// A "slow" action that blocks until the test releases it, so the test can
// observe the counter while a call is genuinely still in flight.
struct PCSlowAction {};

struct PCModel {
    using PrimaryKey = int;  // Lets PCModel be used with BridgeHandler<PCModel, AllowShared> below.

    int execute(PCFastAction action) { return action.value * 2; }
    int execute(PCFailAction) { throw std::runtime_error("pending-calls test failure"); }
    int execute(PCSlowAction) {
        gPendingCallsSlowStarted.fetch_add(1, std::memory_order_relaxed);
        while (!gPendingCallsSlowRelease.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 1;
    }
};

BRIDGE_REGISTER_MODEL(PCModel, "Test_PCModel")
BRIDGE_REGISTER_ACTION(PCModel, PCFastAction, "Test_PCFastAction")
BRIDGE_REGISTER_ACTION(PCModel, PCFailAction, "Test_PCFailAction")
BRIDGE_REGISTER_ACTION(PCModel, PCSlowAction, "Test_PCSlowAction")

TEST_CASE("Bridge: pendingCalls() is zero before any dispatch", "[bridge][pending-calls]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<PCModel> handler{bridge, &cbExec};

    REQUIRE(bridge.pendingCalls() == 0);
}

TEST_CASE("Bridge: pendingCalls() increments on dispatch and decrements on success", "[bridge][pending-calls]") {
    gPendingCallsSlowStarted.store(0);
    gPendingCallsSlowRelease.store(false);

    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<PCModel> handler{bridge, &cbExec};

    std::atomic<bool> done{false};
    handler.execute(PCSlowAction{}).then([&](int) { done.store(true); }).onError([](const std::exception_ptr&) {});

    // Wait until the model actually started executing, so the call is
    // genuinely in flight (not just queued).
    for (int idx = 0; idx < 200 && gPendingCallsSlowStarted.load() == 0; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gPendingCallsSlowStarted.load() == 1);
    REQUIRE(bridge.pendingCalls() == 1);

    gPendingCallsSlowRelease.store(true);

    for (int idx = 0; idx < 200 && !done.load(); ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(done.load());
    REQUIRE(bridge.pendingCalls() == 0);
}

TEST_CASE("Bridge: pendingCalls() decrements on error resolution too", "[bridge][pending-calls]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<PCModel> handler{bridge, &cbExec};

    std::atomic<bool> errored{false};
    handler.execute(PCFailAction{}).then([](int) {}).onError([&](const std::exception_ptr&) { errored.store(true); });

    for (int idx = 0; idx < 200 && !errored.load(); ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(errored.load());
    REQUIRE(bridge.pendingCalls() == 0);
}

TEST_CASE("Bridge: pendingCalls() reflects multiple concurrent in-flight calls", "[bridge][pending-calls]") {
    // LocalBackend serialises actions per model instance (each instance gets
    // its own strand), so genuine concurrency here needs distinct handlers
    // (distinct instances), not repeated dispatch on one handler.
    gPendingCallsSlowStarted.store(0);
    gPendingCallsSlowRelease.store(false);

    morph::exec::ThreadPoolExecutor pool{4};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<PCModel> handler1{bridge, &cbExec};
    morph::bridge::BridgeHandler<PCModel> handler2{bridge, &cbExec};
    morph::bridge::BridgeHandler<PCModel> handler3{bridge, &cbExec};

    std::atomic<int> doneCount{0};
    constexpr int numCalls = 3;
    auto onDone = [&](int) { doneCount.fetch_add(1); };
    auto onErr = [](const std::exception_ptr&) {};
    handler1.execute(PCSlowAction{}).then(onDone).onError(onErr);
    handler2.execute(PCSlowAction{}).then(onDone).onError(onErr);
    handler3.execute(PCSlowAction{}).then(onDone).onError(onErr);

    for (int idx = 0; idx < 200 && gPendingCallsSlowStarted.load() < numCalls; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gPendingCallsSlowStarted.load() == numCalls);
    REQUIRE(bridge.pendingCalls() == numCalls);

    gPendingCallsSlowRelease.store(true);

    for (int idx = 0; idx < 200 && doneCount.load() < numCalls; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(doneCount.load() == numCalls);
    REQUIRE(bridge.pendingCalls() == 0);
}

TEST_CASE("Bridge: pendingCalls() does not increment for a synchronously-failed unbound handler",
          "[bridge][pending-calls]") {
    // A shared handler with no primary yet is unbound: executeVia's
    // "handler not bound" branch resolves the Completion immediately,
    // synchronously, before any backend dispatch. This must not leave the
    // counter incremented (nor decrement below zero).
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<PCModel, morph::bridge::AllowShared> handler{bridge, &cbExec};

    bool errorFired = false;
    handler.execute(PCFastAction{5}).then([](int) {}).onError([&](const std::exception_ptr&) { errorFired = true; });

    REQUIRE(errorFired);
    REQUIRE(bridge.pendingCalls() == 0);
}
