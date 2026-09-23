// SPDX-License-Identifier: Apache-2.0

// `Bridge::pendingCalls()` is the client-side quiescence signal.
// These tests exercise the in-flight
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
// A function-local static rather than a namespace-scope one, unlike its two
// neighbours above: `cppcoreguidelines-avoid-non-const-global-variables` is on
// for tests/ and fires on the latter. The two above predate the changed-lines
// clang-tidy gate and are not reported.
std::atomic<int>& pcSlowFinished() {
    static std::atomic<int> value{0};
    return value;
}
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
        pcSlowFinished().fetch_add(1, std::memory_order_relaxed);
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

// ── A throwing backend->execute() must not leak the slot ──
//
// executeVia() incremented `_pendingCalls` and armed the client deadline before
// calling `backend->execute(...)`, which was not wrapped in a try. That call is
// genuinely throwing code -- for QtWebSocketBackend it runs `serializeAction()`
// (user `toJson` and glaze) and `wire::encode(env)`. A throw escaped
// BridgeHandler::execute() with the counter permanently inflated, which breaks
// the quiescence gate this whole file exists to cover: pendingCalls() could
// never return to 0 again for that bridge.
namespace {
/// Wraps LocalBackend and throws from the dispatch, the way an encode failure does.
///
/// Overrides `executeInto`, not `execute`: `Bridge::executeVia` dispatches
/// through the former, and `LocalBackend::execute` is `final` precisely so a
/// double written the other way round is a compile error rather than a test
/// that quietly stops intercepting anything.
struct ThrowingExecuteBackend : morph::backend::LocalBackend {
    using morph::backend::LocalBackend::LocalBackend;

    void executeInto(morph::exec::detail::ModelId, morph::backend::detail::ActionCall, morph::exec::IExecutor*,
                     std::shared_ptr<morph::async::detail::ISettleSink>) override {
        throw std::runtime_error("serialize/encode failed");
    }
};
}  // namespace

TEST_CASE("Bridge: a throwing backend execute() leaves pendingCalls() at zero", "[bridge][pending-calls][morph502]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<ThrowingExecuteBackend>(pool)};
    morph::bridge::BridgeHandler<PCModel> handler{bridge, &cbExec};

    REQUIRE(bridge.pendingCalls() == 0);
    REQUIRE_THROWS_AS(handler.execute(PCFastAction{.value = 21}), std::runtime_error);
    // Before the fix this was 1, permanently, for the life of the bridge.
    CHECK(bridge.pendingCalls() == 0);

    // And the bridge is still usable as a quiescence gate afterwards.
    REQUIRE_THROWS_AS(handler.execute(PCFastAction{.value = 1}), std::runtime_error);
    CHECK(bridge.pendingCalls() == 0);
}

// ── cancelPending racing the real reply settles once ──
//
// Before Part B, "exactly one decrement per dispatch" was carried by the fact
// that `.then` and `.onError` are mutually exclusive on one `CompletionState`:
// `cancelPending` resolved the erased completion, the `.onError` forwarder
// fired, and the later `setValue` from the strand found an already-ready state
// and did nothing. There is no such guarantee on a sink -- `cancelPending`
// settles it from the caller's thread while the strand task is about to settle
// it from another -- so `BridgeSink` carries its own settle-once latch.
//
// Without that latch the second settle decrements `_pendingCalls` again, and
// the counter is a `std::size_t`: a second decrement from zero does not read as
// -1, it reads as 18446744073709551615, and `pendingCalls()` is documented as a
// quiescence gate that a caller waits on.
TEST_CASE("Bridge: cancelPending followed by the real reply decrements pendingCalls() once",
          "[bridge][pending-calls][morph572]") {
    gPendingCallsSlowStarted.store(0);
    gPendingCallsSlowRelease.store(false);
    pcSlowFinished().store(0);

    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    auto owned = std::make_unique<morph::backend::LocalBackend>(pool);
    auto* const backend = owned.get();
    morph::bridge::Bridge bridge{std::move(owned)};
    morph::bridge::BridgeHandler<PCModel> handler{bridge, &cbExec};

    std::atomic<int> errors{0};
    std::atomic<int> results{0};
    handler.execute(PCSlowAction{}).then([&](int) { results.fetch_add(1); }).onError([&](const std::exception_ptr&) {
        errors.fetch_add(1);
    });

    for (int idx = 0; idx < 400 && gPendingCallsSlowStarted.load() == 0; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gPendingCallsSlowStarted.load() == 1);
    REQUIRE(bridge.pendingCalls() == 1);

    // First settle: the backend cancels everything still in flight, exactly as
    // switchBackend() and ~Bridge() do.
    backend->cancelPending(std::make_exception_ptr(std::runtime_error("cancelled")));
    CHECK(bridge.pendingCalls() == 0);
    CHECK(errors.load() == 1);

    // Second settle: the model finishes and the strand task settles the same
    // sink with the real result. First result wins, so the caller still sees
    // only the cancellation -- and the counter must not move again.
    gPendingCallsSlowRelease.store(true);
    for (int idx = 0; idx < 400 && pcSlowFinished().load() == 0; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(pcSlowFinished().load() == 1);
    // The strand task settles after the model returns; give it room to land.
    for (int idx = 0; idx < 100 && bridge.pendingCalls() == 0 && results.load() == 0; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(bridge.pendingCalls() == 0);
    CHECK(errors.load() == 1);
    CHECK(results.load() == 0);
}
