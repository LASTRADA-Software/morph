// SPDX-License-Identifier: Apache-2.0
//
// morph::async::delay (docs/spec/core/coroutines.md, "delay"): a stop-aware
// wait on a TimeoutScheduler entry that resumes in the awaiting context.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <memory>
#include <morph/core/coroutine.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/timeout_scheduler.hpp>
#include <thread>

#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;

template <typename Pred>
[[nodiscard]] bool pumpUntil(morph::exec::MainThreadExecutor& exec, Pred pred) {
    auto const deadline = std::chrono::steady_clock::now() + morph::testing::kDefaultWaitBudget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        exec.runFor(morph::testing::kDefaultWaitStep);
    }
    return true;
}

struct DelayObserved {
    std::atomic<bool> finished{false};
    bool cancelled = false;
    std::chrono::steady_clock::duration waited{};
    std::thread::id resumedOn;
    /// Set immediately before the `co_await`, in the same step that suspends.
    std::atomic<bool> reachedAwait{false};
};

core::async::Task<void> waitFor(morph::async::detail::TimeoutScheduler* scheduler, std::chrono::milliseconds duration,
                                std::shared_ptr<DelayObserved> seen) {
    auto const started = std::chrono::steady_clock::now();
    try {
        seen->reachedAwait = true;
        co_await morph::async::delay(*scheduler, duration);
    } catch (const core::async::OperationCancelled&) {
        seen->cancelled = true;
    }
    seen->waited = std::chrono::steady_clock::now() - started;
    seen->resumedOn = std::this_thread::get_id();
    seen->finished = true;
}

}  // namespace

TEST_CASE("delay resumes on the awaiting context's executor once the time has elapsed", "[coroutine][delay]") {
    morph::async::detail::TimeoutScheduler scheduler;
    morph::exec::MainThreadExecutor exec;
    auto seen = std::make_shared<DelayObserved>();

    morph::async::spawn(exec, waitFor(&scheduler, 30ms, seen));

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE_FALSE(seen->cancelled);
    REQUIRE(seen->waited >= 30ms);
    // Resumed through spawn's executor, not on the scheduler's loop thread.
    REQUIRE(seen->resumedOn == std::this_thread::get_id());
}

TEST_CASE("a stop request cancels a delay and resumes it with OperationCancelled", "[coroutine][delay]") {
    morph::async::detail::TimeoutScheduler scheduler;
    morph::exec::MainThreadExecutor exec;
    auto seen = std::make_shared<DelayObserved>();

    auto task = waitFor(&scheduler, 60s, seen);
    // Not const: request_stop() is a non-const member in the standard, and in
    // MSVC's library; libstdc++ declaring it const is what makes this look
    // const-able.
    // NOLINTNEXTLINE(misc-const-correctness)
    core::async::StopSource stop;
    task.handle().promise().setStopToken(stop.get_token());
    morph::async::spawn(exec, std::move(task));
    REQUIRE(pumpUntil(exec, [&] { return seen->reachedAwait.load(); }));
    REQUIRE_FALSE(seen->finished.load());

    stop.request_stop();

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE(seen->cancelled);
    REQUIRE(seen->waited < 10s);
    REQUIRE(seen->resumedOn == std::this_thread::get_id());
}

TEST_CASE("a delay awaited under a stop already requested does not suspend", "[coroutine][delay]") {
    morph::async::detail::TimeoutScheduler scheduler;
    morph::exec::MainThreadExecutor exec;
    auto seen = std::make_shared<DelayObserved>();

    auto task = waitFor(&scheduler, 60s, seen);
    // Not const: request_stop() is a non-const member in the standard.
    // NOLINTNEXTLINE(misc-const-correctness)
    core::async::StopSource stop;
    stop.request_stop();
    task.handle().promise().setStopToken(stop.get_token());
    morph::async::spawn(exec, std::move(task));

    REQUIRE(pumpUntil(exec, [&] { return seen->finished.load(); }));
    REQUIRE(seen->cancelled);
    REQUIRE(seen->waited < 10s);
}

TEST_CASE("a delay outside any resumption context resumes on the scheduler's thread", "[coroutine][delay]") {
    morph::async::detail::TimeoutScheduler scheduler;
    auto seen = std::make_shared<DelayObserved>();

    // Started by hand rather than spawned, so no resumption context is current.
    auto task = waitFor(&scheduler, 10ms, seen);
    task.handle().resume();
    REQUIRE(morph::testing::waitUntil([&] { return seen->finished.load(); }));
    // The loop runs one callback at a time: once this one has run, the one
    // that resumed the coroutine has returned, and its frame may be destroyed.
    std::atomic<bool> drained{false};
    static_cast<void>(scheduler.schedule(0ms, [&] { drained = true; }));
    REQUIRE(morph::testing::waitUntil([&] { return drained.load(); }));

    REQUIRE_FALSE(seen->cancelled);
    REQUIRE(seen->resumedOn != std::this_thread::get_id());
}

TEST_CASE("a delay whose coroutine is destroyed while it waits withdraws its timer", "[coroutine][delay]") {
    morph::async::detail::TimeoutScheduler scheduler;
    auto seen = std::make_shared<DelayObserved>();
    {
        auto task = waitFor(&scheduler, 20ms, seen);
        task.handle().resume();
        REQUIRE(seen->reachedAwait.load());
    }  // the frame, and the awaiter in it, die here while the timer is armed

    // Wait past the withdrawn deadline: had the timer survived, it would have
    // resumed the destroyed frame.
    std::atomic<bool> past{false};
    static_cast<void>(scheduler.schedule(60ms, [&] { past = true; }));
    REQUIRE(morph::testing::waitUntil([&] { return past.load(); }));
    REQUIRE_FALSE(seen->finished.load());
}
