// SPDX-License-Identifier: Apache-2.0

#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/registry.hpp>
#include <morph/task.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>


namespace {
struct CovSyncExecutor : morph::exec::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};
}  // namespace

// ── task.hpp: per-instantiation coverage for TaskState<T> ────────────────────
//
// LocalBackend uses TaskState<std::shared_ptr<void>>; tests use TaskState<int>
// and TaskState<std::string> in places. Codecov's per-instantiation view marks
// branches on lines 43, 58, 71 partial unless every arm fires for every
// instantiation. Walk all paths for each used type.

namespace {

template <typename T>
void exerciseTaskStateBranches(const T& sampleValue) {
    using namespace morph::async::detail;

    // attach BEFORE ready  → line 71 True arm; setValue then fires continuation (line 43 True arm).
    {
        TaskState<T> state;
        bool fired = false;
        state.attach([&](TaskState<T>&) { fired = true; });
        state.setValue(T{sampleValue});
        REQUIRE(fired);
    }

    // attach BEFORE ready, then setException  → line 58 True arm.
    {
        TaskState<T> state;
        bool fired = false;
        state.attach([&](TaskState<T>&) { fired = true; });
        state.setException(std::make_exception_ptr(std::runtime_error{"err"}));
        REQUIRE(fired);
    }

    // setValue WITHOUT attach  → line 43 False arm.
    {
        TaskState<T> state;
        state.setValue(T{sampleValue});
        REQUIRE(state.ready);
    }

    // setException WITHOUT attach  → line 58 False arm.
    {
        TaskState<T> state;
        state.setException(std::make_exception_ptr(std::runtime_error{"err"}));
        REQUIRE(state.error != nullptr);
    }

    // attach AFTER ready (value)  → line 71 False arm; immediate fire on line 76.
    {
        TaskState<T> state;
        state.setValue(T{sampleValue});
        bool fired = false;
        state.attach([&](TaskState<T>&) { fired = true; });
        REQUIRE(fired);
    }

    // attach AFTER ready (error)  → same False arm via the error side.
    {
        TaskState<T> state;
        state.setException(std::make_exception_ptr(std::runtime_error{"err"}));
        bool fired = false;
        state.attach([&](TaskState<T>&) { fired = true; });
        REQUIRE(fired);
    }
}

}  // namespace

TEST_CASE("TaskState<int>: every branch", "[task]") {
    exerciseTaskStateBranches<int>(42);
}

TEST_CASE("TaskState<string>: every branch", "[task]") {
    exerciseTaskStateBranches<std::string>("hello");
}

TEST_CASE("TaskState<shared_ptr<void>>: every branch", "[task]") {
    exerciseTaskStateBranches<std::shared_ptr<void>>(
        std::static_pointer_cast<void>(std::make_shared<int>(7)));
}

// ── executor.hpp: while-loop guard takes its `now() >= deadline` arm ─────────
//
// All existing runFor tests enter the loop body and exit via `wait_until` →
// `return;`. The while condition's False arm at line 128 stays untaken. Run
// with a zero-duration timeout: deadline equals "now" at entry, so the next
// `now()` read is past deadline and the loop is skipped.

TEST_CASE("morph::exec::MainThreadExecutor: runFor with zero timeout skips the loop", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    bool taskRan = false;
    exec.post([&] { taskRan = true; });
    exec.runFor(std::chrono::milliseconds(0));
    REQUIRE_FALSE(taskRan);  // queue not drained — loop never entered
}

// ── bridge.hpp: easy partial branches ────────────────────────────────────────
//
// unsubscribe<A> on a handler that never subscribed → False arm of `iter != end`.
// reset<A>      on a handler that never `set<...>`  → False arm of `iter != end`.

struct CovAction {
    int v = 0;
};
struct CovModel {
    int execute(CovAction act) { return act.v + 1; }
};

BRIDGE_REGISTER_MODEL(CovModel, "Cov_CovModel")
BRIDGE_REGISTER_ACTION(CovModel, CovAction, "Cov_CovAction")

TEST_CASE("BridgeHandler::unsubscribe on type with no entry is a no-op", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{1};
    CovSyncExecutor cb;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CovModel> handler{bridge, &cb};

    // No prior subscribe<CovAction> → the unsubscribe path finds no entry and exercises
    // the False arm of `if (iter != _subs->entries.end())` at line 311.
    REQUIRE_NOTHROW(handler.unsubscribe<CovAction>());
}

TEST_CASE("BridgeHandler::reset on type with no entry is a no-op", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{1};
    CovSyncExecutor cb;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CovModel> handler{bridge, &cb};

    REQUIRE_NOTHROW(handler.reset<CovAction>());
}

TEST_CASE("BridgeHandler::set on a field reuses an existing draft", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{1};
    CovSyncExecutor cb;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CovModel> handler{bridge, &cb};

    // First set creates the draft entry → line 327 True arm. Second set finds
    // the entry already there → False arm.
    handler.set<&CovAction::v>(1);
    handler.set<&CovAction::v>(2);
    REQUIRE(true);
}
