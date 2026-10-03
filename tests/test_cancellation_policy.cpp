// SPDX-License-Identifier: Apache-2.0
//
// The cancellation policy in docs/spec/concurrency_and_lifetimes.md, held as
// tests: every cancel verb's G-level is asserted from below and, where a
// stronger level is reachable, from above, so a verb that gets weaker *or*
// silently stronger turns a case red.
//
// The scale, from the spec:
//   G0  nothing changes for callbacks on return; at most new work is refused
//   G1  no further callback will be scheduled; one already posted still runs
//   G2  G1, plus the terminal outcome is now scheduled (posted, not yet run)
//   G3  no further callback will start
//   G4  G3, plus none is still running
//
// "Work" is the model action or server handler the verb abandons. Most cases
// use a synchronous handler, which nothing can stop, so they measure what each
// verb does to the callbacks and whether it waits for that work. The verbs
// whose point is to stop the work -- a stopped co_await -- are measured
// against a Task handler, which can observe a stop; the other verbs' Task
// handler stops are measured in test_coroutine_model.cpp.

#include <any>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/coroutine.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/io_loop.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/timeout_scheduler.hpp>
#include <morph/core/wire.hpp>
#include <morph/offline/network_monitor.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

#include "test_support.hpp"

using namespace std::chrono_literals;

// ── A model whose one slow action blocks until the test lets it go ───────────

struct CPBlock {
    int tag = 0;
};
struct CPFast {
    int tag = 0;
};
/// Handled by a Task handler that sleeps until it is stopped.
struct CPSleep {
    int ms = 0;
};

namespace {

/// The slow action's progress, read by the test thread while the action runs
/// on a pool thread.
struct CPGate {
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<bool> released{false};

    void reset() {
        started = 0;
        finished = 0;
        released = false;
    }
};

CPGate& gate() {
    static CPGate instance;
    return instance;
}

/// The Task handler's progress, and the scheduler it sleeps on.
struct CPSleeper {
    std::atomic<int> started{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> finished{0};
    morph::async::detail::TimeoutScheduler* scheduler = nullptr;
};

CPSleeper& sleeper() {
    static CPSleeper instance;
    return instance;
}

/// Owns the scheduler `CPSleep` waits on for one test, and joins its thread
/// when the test ends rather than keeping it for the rest of the run.
class SleeperScope {
public:
    SleeperScope() {
        sleeper().started = 0;
        sleeper().cancelled = 0;
        sleeper().finished = 0;
        sleeper().scheduler = &_scheduler;
    }
    SleeperScope(const SleeperScope&) = delete;
    SleeperScope& operator=(const SleeperScope&) = delete;
    SleeperScope(SleeperScope&&) = delete;
    SleeperScope& operator=(SleeperScope&&) = delete;
    ~SleeperScope() { sleeper().scheduler = nullptr; }

private:
    morph::async::detail::TimeoutScheduler _scheduler;
};

template <typename Error>
[[nodiscard]] bool holds(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const Error&) {
        return true;
    } catch (...) {
        return false;
    }
}

/// Lets the slow action go after @p delay, from another thread: for a verb
/// that waits for the action, so the test thread can be the one waiting.
std::jthread releaseAfter(std::chrono::milliseconds delay) {
    return std::jthread{[delay] {
        std::this_thread::sleep_for(delay);
        gate().released = true;
    }};
}

}  // namespace

struct CPModel {
    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    int execute(CPBlock action) {
        gate().started.fetch_add(1);
        auto const deadline = std::chrono::steady_clock::now() + morph::testing::kDefaultWaitBudget;
        while (!gate().released.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        gate().finished.fetch_add(1);
        return action.tag;
    }
    int execute(CPFast action) { return action.tag; }
    core::async::Task<int> execute(CPSleep action) {
        sleeper().started.fetch_add(1);
        try {
            co_await morph::async::delay(*sleeper().scheduler, std::chrono::milliseconds{action.ms});
        } catch (const core::async::OperationCancelled&) {
            sleeper().cancelled.fetch_add(1);
            throw;
        }
        sleeper().finished.fetch_add(1);
        co_return action.ms;
    }
    // NOLINTEND(readability-convert-member-functions-to-static)
};

BRIDGE_REGISTER_MODEL(CPModel, "CP_Model")
BRIDGE_REGISTER_ACTION(CPModel, CPBlock, "CP_Block")
BRIDGE_REGISTER_ACTION(CPModel, CPFast, "CP_Fast")
BRIDGE_REGISTER_ACTION(CPModel, CPSleep, "CP_Sleep")

namespace {

/// What reached a call's continuations, on the owner.
struct Outcome {
    int ok = 0;
    int err = 0;
    std::exception_ptr error;

    template <typename T>
    void attach(morph::async::Completion<T>& completion) {
        completion.then([this](const T&) { ++ok; }).onError([this](const std::exception_ptr& exc) {
            ++err;
            error = exc;
        });
    }
};

/// Pumps @p owner until @p pred holds or the default budget runs out.
template <typename Pred>
bool pumpUntil(morph::exec::MainThreadExecutor& owner, Pred pred) {
    auto const deadline = std::chrono::steady_clock::now() + morph::testing::kDefaultWaitBudget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        owner.runFor(morph::testing::kDefaultWaitStep);
    }
    return true;
}

/// Starts a slow call on @p handler and returns once the action is running.
template <typename Handler>
void startBlockedCall(Handler& handler, morph::exec::MainThreadExecutor& owner, Outcome& outcome) {
    auto completion = handler.execute(CPBlock{.tag = 1});
    outcome.attach(completion);
    REQUIRE(pumpUntil(owner, [] { return gate().started.load() == 1; }));
}

morph::wire::Envelope executeEnvelope(std::uint64_t mid, std::string_view action, std::uint64_t callId) {
    morph::wire::Envelope env;
    env.kind = "execute";
    env.callId = callId;
    env.modelId = mid;
    env.modelType = "CP_Model";
    env.actionType = std::string{action};
    env.body = R"({"tag":1})";
    return env;
}

/// Collects every reply a server sends for one request, from any thread.
struct ReplyLog {
    std::mutex mtx;
    std::vector<morph::wire::Envelope> replies;

    std::function<void(std::string)> sink() {
        return [this](const std::string& raw) {
            std::scoped_lock const lock{mtx};
            replies.push_back(morph::wire::decode(raw));
        };
    }
    std::size_t count() {
        std::scoped_lock const lock{mtx};
        return replies.size();
    }
    morph::wire::Envelope first() {
        std::scoped_lock const lock{mtx};
        return replies.front();
    }
};

std::uint64_t registerOn(morph::backend::RemoteServer& server) {
    morph::testing::WaitReply reg;
    server.handle(morph::wire::encode(morph::wire::makeRegister("CP_Model")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");
    return reg.env.modelId;
}

}  // namespace

// ── IBackend::cancelPending ─────────────────────────────────────────────────

TEST_CASE("LocalBackend::cancelPending is G2: the error is posted, not run, and the work is not waited for",
          "[cancel-policy]") {
    gate().reset();
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto owned = std::make_unique<morph::backend::LocalBackend>(pool);
    auto* const backend = owned.get();
    morph::bridge::Bridge bridge{std::move(owned), owner};
    morph::bridge::BridgeHandler<CPModel> handler{bridge, &owner};
    Outcome outcome;
    startBlockedCall(handler, owner, outcome);

    backend->cancelPending(std::make_exception_ptr(morph::backend::DisconnectedError{}));

    // Not G3: the terminal callback has not run on return.
    CHECK(outcome.err == 0);
    // No join: the action it abandoned is still running.
    CHECK(gate().finished.load() == 0);
    // G2: the terminal outcome is scheduled -- it runs on the next pump.
    REQUIRE(pumpUntil(owner, [&] { return outcome.err == 1; }));
    CHECK(holds<morph::backend::DisconnectedError>(outcome.error));

    // G1: the action's own result, produced after the cancel, reaches nobody.
    gate().released = true;
    REQUIRE(morph::testing::waitUntil([] { return gate().finished.load() == 1; }));
    owner.runFor(30ms);
    CHECK(outcome.ok == 0);
    CHECK(outcome.err == 1);
}

TEST_CASE("SimulatedRemoteBackend::cancelPending is G2, and the server's work runs to completion", "[cancel-policy]") {
    gate().reset();
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto owned = std::make_unique<morph::backend::SimulatedRemoteBackend>(*server);
    auto* const backend = owned.get();
    morph::bridge::Bridge bridge{std::move(owned), owner};
    morph::bridge::BridgeHandler<CPModel> handler{bridge, &owner};
    REQUIRE(pumpUntil(owner, [&] { return handler.isBound(); }));
    Outcome outcome;
    startBlockedCall(handler, owner, outcome);

    backend->cancelPending(std::make_exception_ptr(morph::backend::DisconnectedError{}));

    CHECK(outcome.err == 0);
    CHECK(gate().finished.load() == 0);
    REQUIRE(pumpUntil(owner, [&] { return outcome.err == 1; }));

    // Nothing reached the server: its handler finishes, and its reply is dropped.
    gate().released = true;
    REQUIRE(morph::testing::waitUntil([] { return gate().finished.load() == 1; }));
    owner.runFor(30ms);
    CHECK(outcome.ok == 0);
    CHECK(outcome.err == 1);
}

namespace {

/// A `LocalBackend` that counts the cancellations that reach it.
class CountingLocalBackend : public morph::backend::LocalBackend {
public:
    using LocalBackend::LocalBackend;
    void cancelPending(const std::exception_ptr& exc) override {
        cancels.fetch_add(1);
        LocalBackend::cancelPending(exc);
    }
    std::atomic<int> cancels{0};
};

}  // namespace

TEST_CASE(
    "SynchronousBackendAdapter::cancelPending is G2 for its own binds and G0, on return, for the wrapped backend",
    "[cancel-policy]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::StepExecutor control;
    morph::exec::MainThreadExecutor owner;
    auto inner = std::make_shared<CountingLocalBackend>(pool);
    std::optional<morph::backend::SynchronousBackendAdapter> adapter{std::in_place, inner, control};

    std::atomic<int> created{0};
    auto bind = adapter->bindModel(
        morph::backend::detail::BindRequest{.typeId = "CP_Model",
                                            .factory =
                                                [&created] {
                                                    created.fetch_add(1);
                                                    return morph::model::detail::ModelFactory::create<CPModel>();
                                                },
                                            .contextKey = {},
                                            .primary = {}},
        owner);
    Outcome outcome;
    outcome.attach(bind);

    adapter->cancelPending(std::make_exception_ptr(morph::backend::BridgeDestroyedError{}));

    // Its own bind: rejected, the error posted and not yet run -- G2.
    CHECK(outcome.err == 0);
    REQUIRE(pumpUntil(owner, [&] { return outcome.err == 1; }));
    // The wrapped backend: on return the cancellation has only been queued on
    // the control strand. Nothing has changed there yet -- G0.
    CHECK(inner->cancels.load() == 0);

    control.runAll();
    CHECK(inner->cancels.load() == 1);
    // The bind queued before the cancel never reached the wrapped backend.
    CHECK(created.load() == 0);
    owner.runFor(10ms);
    CHECK(outcome.ok == 0);
    adapter.reset();
}

// ── Bridge ──────────────────────────────────────────────────────────────────

TEST_CASE("Bridge destructor is G2 for its calls, and waits for the local work it abandoned (G4 for the work)",
          "[cancel-policy]") {
    gate().reset();
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<morph::backend::LocalBackend>(pool), owner);
    auto handler = std::make_unique<morph::bridge::BridgeHandler<CPModel>>(*bridge, &owner);
    Outcome outcome;
    startBlockedCall(*handler, owner, outcome);

    handler.reset();
    {
        // The bridge's LocalBackend drains its strands as it is destroyed, so
        // the action is let go from another thread while this one waits.
        auto const releaser = releaseAfter(50ms);
        bridge.reset();
    }

    // G4 for the work: the destructor returned only once the action had ended.
    CHECK(gate().finished.load() == 1);
    // G2 for the call: BridgeDestroyedError is posted, not run.
    CHECK(outcome.err == 0);
    REQUIRE(pumpUntil(owner, [&] { return outcome.err == 1; }));
    CHECK(holds<morph::backend::BridgeDestroyedError>(outcome.error));
    owner.runFor(10ms);
    CHECK(outcome.ok == 0);
}

TEST_CASE("Bridge::switchBackend is G2 for the outgoing backend's calls", "[cancel-policy]") {
    gate().reset();
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<CPModel> handler{bridge, &owner};
    Outcome outcome;
    startBlockedCall(handler, owner, outcome);

    {
        auto const releaser = releaseAfter(50ms);
        bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool));
    }

    // The outgoing LocalBackend is destroyed inside the switch, and waits for
    // its running action like any other.
    CHECK(gate().finished.load() == 1);
    CHECK(outcome.err == 0);
    REQUIRE(pumpUntil(owner, [&] { return outcome.err == 1; }));
    CHECK(holds<morph::backend::BackendChangedError>(outcome.error));
    owner.runFor(10ms);
    CHECK(outcome.ok == 0);
}

TEST_CASE("Bridge::setExecuteDeadline is G0 for calls already made; a fired deadline settles its call as G2",
          "[cancel-policy]") {
    gate().reset();
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<CPModel> handler{bridge, &owner};

    Outcome before;
    startBlockedCall(handler, owner, before);
    bridge.setExecuteDeadline(20ms);
    owner.runFor(100ms);
    // G0: a call made before the deadline was set is not touched by it.
    CHECK(before.err == 0);
    gate().released = true;
    REQUIRE(pumpUntil(owner, [&] { return before.ok == 1; }));

    gate().reset();
    Outcome after;
    auto completion = handler.execute(CPBlock{.tag = 2});
    after.attach(completion);
    REQUIRE(pumpUntil(owner, [&] { return after.err == 1; }));
    CHECK(holds<morph::backend::ClientTimeoutError>(after.error));
    // The deadline settles the call; it does not end a synchronous action.
    CHECK(gate().finished.load() == 0);
    gate().released = true;
    REQUIRE(morph::testing::waitUntil([] { return gate().finished.load() == 1; }));
    owner.runFor(30ms);
    CHECK(after.ok == 0);
}

namespace {

/// The binding type `SubscriptionRegistry` matches on: only its instance id.
struct CPBinding {
    std::atomic<std::uint64_t> currentId{7};
};

}  // namespace

TEST_CASE("unsubscribe is G1: a delivery already posted still runs, no new one is scheduled", "[cancel-policy]") {
    morph::testing::StepExecutor gui;
    morph::bridge::detail::SubscriptionRegistry<CPBinding> registry;
    auto binding = std::make_shared<CPBinding>();
    int delivered = 0;
    registry.addSubscription(binding, std::type_index{typeid(int)}, [&](const std::any&) { ++delivered; }, &gui);

    registry.publishResult(morph::exec::detail::ModelId{7}, std::type_index{typeid(int)}, std::any{1});
    REQUIRE(gui.pending() == 1);

    registry.removeSubscription(binding, std::type_index{typeid(int)});

    // Not G3: the delivery posted before the unsubscribe still runs after it.
    gui.runAll();
    CHECK(delivered == 1);
    // G1: nothing new is scheduled.
    registry.publishResult(morph::exec::detail::ModelId{7}, std::type_index{typeid(int)}, std::any{2});
    CHECK(gui.pending() == 0);
    gui.runAll();
    CHECK(delivered == 1);
}

// ── CallbackScope ───────────────────────────────────────────────────────────

TEST_CASE("CallbackScope stop is G3 on the delivery executor, and never waits for a running callback",
          "[cancel-policy]") {
    SECTION("requestStop, reset and destruction refuse a delivery already posted") {
        morph::testing::StepExecutor owner;
        auto scope = std::make_optional<morph::async::CallbackScope>();
        auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&owner);
        int delivered = 0;
        completion.then(*scope, [&](const int&) { ++delivered; });
        promise.resolve(1);
        REQUIRE(owner.pending() == 1);

        SECTION("requestStop") { scope->requestStop(); }
        SECTION("reset") { scope->reset(); }
        SECTION("destruction") { scope.reset(); }

        // G3: posted before the stop, refused when it runs.
        owner.runAll();
        CHECK(delivered == 0);
    }

    SECTION("a stop from another thread returns while a gated callback is still running") {
        morph::exec::ThreadPoolExecutor pool{1};
        morph::async::CallbackScope const scope;
        std::atomic<bool> inBody{false};
        std::atomic<bool> letGo{false};
        std::atomic<bool> done{false};
        pool.post(scope.guard([&] {
            inBody = true;
            REQUIRE(morph::testing::waitUntil([&] { return letGo.load(); }));
            done = true;
        }));
        REQUIRE(morph::testing::waitUntil([&] { return inBody.load(); }));

        scope.requestStop();

        // Not G4: the stop does not wait for the callback inside its body.
        CHECK_FALSE(done.load());
        letGo = true;
        REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    }
}

// ── A stopped co_await ──────────────────────────────────────────────────────

namespace {

/// What reached a coroutine awaiting one call, on the owner.
struct AwaitSeen {
    /// Set in the step that suspends, just before the `co_await`.
    std::atomic<bool> suspending{false};
    std::atomic<bool> finished{false};
    bool cancelled = false;
    /// Anything other than the cancellation: a value, or another error.
    bool settled = false;
};

core::async::Task<void> awaitCall(morph::async::Completion<int> call, std::shared_ptr<AwaitSeen> seen) {
    try {
        seen->suspending = true;
        static_cast<void>(co_await std::move(call));
        seen->settled = true;
    } catch (const core::async::OperationCancelled&) {
        seen->cancelled = true;
    } catch (...) {
        seen->settled = true;
    }
    seen->finished = true;
}

}  // namespace

TEST_CASE("A stopped co_await is G2 for the await, and stops the Task handler it was waiting on", "[cancel-policy]") {
    SleeperScope const sleeping;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<CPModel> handler{bridge, &owner};

    auto seen = std::make_shared<AwaitSeen>();
    auto task = awaitCall(handler.execute(CPSleep{.ms = 60'000}), seen);
    // NOLINTNEXTLINE(misc-const-correctness): request_stop() is non-const
    core::async::StopSource stop;
    task.handle().promise().setStopToken(stop.get_token());
    morph::async::spawn(owner, std::move(task));
    REQUIRE(pumpUntil(owner, [&] { return seen->suspending.load() && sleeper().started.load() == 1; }));

    static_cast<void>(stop.request_stop());

    // Not G3: the coroutine has not resumed on return.
    CHECK_FALSE(seen->finished.load());
    // G2: its resumption with OperationCancelled is posted, and runs on a pump.
    REQUIRE(pumpUntil(owner, [&] { return seen->finished.load(); }));
    CHECK(seen->cancelled);
    // The work: the handler the await was waiting on is asked to stop.
    REQUIRE(pumpUntil(owner, [] { return sleeper().cancelled.load() == 1; }));
    CHECK(sleeper().finished.load() == 0);
    // G1: the call's own outcome, settled by the stopped handler, reaches
    // nothing.
    owner.runFor(30ms);
    CHECK_FALSE(seen->settled);
}

TEST_CASE("A co_await under a stop already requested stops the Task handler it would have waited on",
          "[cancel-policy]") {
    SleeperScope const sleeping;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<CPModel> handler{bridge, &owner};

    auto seen = std::make_shared<AwaitSeen>();
    auto task = awaitCall(handler.execute(CPSleep{.ms = 60'000}), seen);
    // NOLINTNEXTLINE(misc-const-correctness): request_stop() is non-const
    core::async::StopSource stop;
    static_cast<void>(stop.request_stop());
    task.handle().promise().setStopToken(stop.get_token());
    morph::async::spawn(owner, std::move(task));

    REQUIRE(pumpUntil(owner, [&] { return seen->finished.load(); }));
    CHECK(seen->cancelled);
    REQUIRE(pumpUntil(owner, [] { return sleeper().cancelled.load() == 1; }));
    CHECK(sleeper().finished.load() == 0);
}

// ── TimeoutScheduler ────────────────────────────────────────────────────────

TEST_CASE("TimeoutScheduler::cancel is G0 on return and G3 once the loop has run it; it never waits",
          "[cancel-policy]") {
    morph::exec::IoLoop loop;
    morph::async::detail::TimeoutScheduler scheduler{loop};

    SECTION("a callback not yet started") {
        std::atomic<bool> loopHeld{true};
        loop.post([&] {
            while (loopHeld.load()) {
                std::this_thread::sleep_for(1ms);
            }
        });
        auto capture = std::make_shared<int>(0);
        std::weak_ptr<int> const watch = capture;
        std::atomic<bool> fired{false};
        auto const handle = scheduler.schedule(30ms, [capture = std::move(capture), &fired] { fired = true; });
        scheduler.cancel(handle);

        // G0: on return nothing has happened yet -- the cancel is only posted,
        // so the callback still holds its captures.
        CHECK_FALSE(watch.expired());

        loopHeld = false;
        loop.runAndWait([] {});
        // G3 once the loop has run the cancel: released, and never fired.
        CHECK(watch.expired());
        std::this_thread::sleep_for(60ms);
        CHECK_FALSE(fired.load());
    }

    SECTION("a callback already running") {
        std::atomic<bool> running{false};
        std::atomic<bool> letGo{false};
        std::atomic<bool> done{false};
        auto const handle = scheduler.schedule(1ms, [&] {
            running = true;
            while (!letGo.load()) {
                std::this_thread::sleep_for(1ms);
            }
            done = true;
        });
        REQUIRE(morph::testing::waitUntil([&] { return running.load(); }));

        scheduler.cancel(handle);

        // Not G4: cancel returns while the callback runs.
        CHECK_FALSE(done.load());
        letGo = true;
        REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    }
}

TEST_CASE("TimeoutScheduler destructor is G4: it returns only once no callback is running", "[cancel-policy]") {
    morph::exec::IoLoop loop;
    auto scheduler = std::make_unique<morph::async::detail::TimeoutScheduler>(loop);
    std::atomic<bool> running{false};
    std::atomic<bool> letGo{false};
    std::atomic<bool> done{false};
    std::atomic<bool> laterFired{false};
    static_cast<void>(scheduler->schedule(1ms, [&] {
        running = true;
        while (!letGo.load()) {
            std::this_thread::sleep_for(1ms);
        }
        done = true;
    }));
    static_cast<void>(scheduler->schedule(20ms, [&] { laterFired = true; }));
    REQUIRE(morph::testing::waitUntil([&] { return running.load(); }));

    {
        std::jthread const releaser{[&] {
            std::this_thread::sleep_for(50ms);
            letGo = true;
        }};
        scheduler.reset();
    }

    CHECK(done.load());
    std::this_thread::sleep_for(40ms);
    CHECK_FALSE(laterFired.load());
}

// ── RemoteServer ────────────────────────────────────────────────────────────

TEST_CASE("RemoteServer's executeTimeout is G2 for the reply; the handler runs on and its reply is dropped",
          "[cancel-policy]") {
    gate().reset();
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::ServerConfig config;
    config.limits.executeTimeout = 30ms;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, config);
    auto const mid = registerOn(*server);

    ReplyLog log;
    server->handle(morph::wire::encode(executeEnvelope(mid, "CP_Block", 1)), log.sink());
    REQUIRE(morph::testing::waitUntil([&] { return log.count() == 1; }));
    CHECK(log.first().kind == "err");
    CHECK(log.first().message == "timeout");
    CHECK(gate().finished.load() == 0);

    gate().released = true;
    REQUIRE(morph::testing::waitUntil([] { return gate().finished.load() == 1; }));
    std::this_thread::sleep_for(50ms);
    CHECK(log.count() == 1);
}

TEST_CASE("RemoteServer::beginShutdown is G0: an execute in flight still answers, a new one is refused",
          "[cancel-policy]") {
    gate().reset();
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto const mid = registerOn(*server);

    ReplyLog inFlight;
    server->handle(morph::wire::encode(executeEnvelope(mid, "CP_Block", 1)), inFlight.sink());
    REQUIRE(morph::testing::waitUntil([] { return gate().started.load() == 1; }));

    server->beginShutdown();

    morph::testing::WaitReply refused;
    server->handle(morph::wire::encode(executeEnvelope(mid, "CP_Fast", 2)), std::ref(refused));
    REQUIRE(refused.await());
    CHECK(refused.env.kind == "err");
    CHECK(refused.env.message == "server shutting down");

    CHECK(inFlight.count() == 0);
    gate().released = true;
    REQUIRE(morph::testing::waitUntil([&] { return inFlight.count() == 1; }));
    CHECK(inFlight.first().kind == "ok");
}

TEST_CASE("RemoteServer::closeConnection is G0 for an execute in flight on that connection", "[cancel-policy]") {
    gate().reset();
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto const cid = server->openConnection();
    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CP_Model")), std::ref(reg), cid);
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");

    ReplyLog inFlight;
    server->handle(morph::wire::encode(executeEnvelope(reg.env.modelId, "CP_Block", 1)), inFlight.sink(), cid);
    REQUIRE(morph::testing::waitUntil([] { return gate().started.load() == 1; }));

    server->closeConnection(cid);

    // The instance is gone for new lookups...
    morph::testing::WaitReply after;
    server->handle(morph::wire::encode(executeEnvelope(reg.env.modelId, "CP_Fast", 2)), std::ref(after));
    REQUIRE(after.await());
    CHECK(after.env.kind == "err");
    // ...but the execute already running answers as if nothing happened.
    CHECK(inFlight.count() == 0);
    gate().released = true;
    REQUIRE(morph::testing::waitUntil([&] { return inFlight.count() == 1; }));
    CHECK(inFlight.first().kind == "ok");
}

// ── Offline ─────────────────────────────────────────────────────────────────

TEST_CASE("NetworkMonitor::stop from off the loop is G4: it returns once no probe is running", "[cancel-policy]") {
    morph::exec::IoLoop loop;
    std::atomic<bool> probing{false};
    std::atomic<bool> letGo{false};
    std::atomic<bool> probeDone{false};
    std::atomic<int> probes{0};
    morph::offline::NetworkMonitor monitor{loop,
                                           [&] {
                                               probes.fetch_add(1);
                                               probing = true;
                                               while (!letGo.load()) {
                                                   std::this_thread::sleep_for(1ms);
                                               }
                                               probeDone = true;
                                               return true;
                                           },
                                           [] {}, [] {}, morph::offline::NetworkMonitorConfig{.probeInterval = 5ms}};
    REQUIRE(morph::testing::waitUntil([&] { return probing.load(); }));

    {
        std::jthread const releaser{[&] {
            std::this_thread::sleep_for(50ms);
            letGo = true;
        }};
        monitor.stop();
    }

    CHECK(probeDone.load());
    int const atStop = probes.load();
    std::this_thread::sleep_for(30ms);
    CHECK(probes.load() == atStop);
}

TEST_CASE("SyncWorker::stop is G1: the item in flight completes, no further item is replayed", "[cancel-policy]") {
    morph::offline::InMemoryOfflineQueue queue{morph::testing::inlineOwner()};
    for (int idx = 0; idx < 3; ++idx) {
        static_cast<void>(queue.enqueue("item" + std::to_string(idx)));
    }
    morph::offline::SyncWorker* self = nullptr;
    int replayed = 0;
    morph::offline::SyncWorker worker{morph::testing::inlineOwner(), queue, [&](const std::string&) {
                                          ++replayed;
                                          self->stop();
                                          return true;
                                      }};
    self = &worker;

    auto const first = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return worker.run(reply); });

    // The replay that called stop() ran to its end and counted; the next did not start.
    CHECK(first.successful == 1);
    CHECK(replayed == 1);

    // The stop that landed during the run is still set: the next run drains nothing.
    auto const second = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return worker.run(reply); });
    CHECK(second.successful == 0);
    CHECK(replayed == 1);
}
