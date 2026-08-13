// SPDX-License-Identifier: Apache-2.0

#pragma once

// Common helpers used across the morph test suite. Each helper here was being
// re-declared (often with slightly different names — SyncExec / SyncExecutor /
// InlineExec) inside ~15 individual test translation units; consolidating them
// keeps the test code consistent and lets the production API not have to expose
// test-only utilities.

#include <morph/core/executor.hpp>
#include <morph/core/wire.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace morph::testing {

/// @brief `IExecutor` that runs every posted task synchronously on the caller's thread.
///
/// Replaces ad-hoc `SyncExec` / `SyncExecutor` / `InlineExec` definitions that
/// were sprinkled across the test suite. Catch2 assertions executed inside the
/// posted lambda surface on the same thread as the test body, so failures keep
/// pointing at the right source location.
struct InlineExecutor : ::morph::exec::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

/// @brief `IExecutor` that queues every posted task and runs them only when the
///        test explicitly asks, one at a time.
///
/// The public interleaving-test harness for issue #55's use case 2: any server
/// component built on `morph::exec::IExecutor` — `RemoteServer` included — can
/// be driven with fully deterministic, hand-stepped task ordering by
/// constructing it against a `StepExecutor` instead of a `ThreadPoolExecutor`.
/// `RemoteServer` posts every dispatch (both the top-level `handle()` post and
/// the per-model strand dispatch its internal `StrandExecutor` performs) onto
/// whichever `IExecutor` it was constructed with, so controlling that one
/// executor is enough to control ordering end-to-end — no need to name
/// `morph::exec::detail::StrandExecutor` or `morph::exec::detail::ModelId` to
/// get there. A test picks which of several pending tasks (e.g. two different
/// models' queued work) to run next via `runOne()`, observing `RemoteServer`'s
/// real per-model serialisation (a strand never posts its next task until the
/// previous one has run) while still controlling the order two *different*
/// models' work interleaves in.
///
/// Not thread-safe against concurrent `runOne()`/`runAll()` calls — intended
/// for single-threaded, single-stepping test code, mirroring `MainThreadExecutor`'s
/// "owning thread" contract but without its wall-clock `runFor()` drain.
class StepExecutor : public ::morph::exec::IExecutor {
public:
    /// @brief Enqueues @p task; does not run it.
    /// @param task Callable to run on a later `runOne()`/`runAll()` call.
    void post(std::function<void()> task) override {
        std::scoped_lock const lock{_mtx};
        _queue.push_back(std::move(task));
    }

    /// @brief Runs exactly one queued task, oldest first (FIFO).
    /// @return `true` if a task was run, `false` if the queue was empty.
    bool runOne() {
        std::function<void()> task;
        {
            std::scoped_lock const lock{_mtx};
            if (_queue.empty()) {
                return false;
            }
            task = std::move(_queue.front());
            _queue.pop_front();
        }
        task();
        return true;
    }

    /// @brief Runs every task currently queued, including ones a running task
    ///        itself posts (e.g. a strand re-arming for its next queued item).
    ///
    /// Bounded at @p maxSteps rather than looping until the queue is empty: a
    /// task that keeps re-posting more work to this executor (a bug in the
    /// code under test, or a harness misuse) would otherwise turn this into an
    /// undetectable infinite loop, hanging the test process with no assertion
    /// failure and no compile-time signal. Real drains in this suite finish
    /// within a handful of steps, so the default is generous headroom, not a
    /// tight bound callers need to reason about.
    /// @param maxSteps Upper bound on tasks run before giving up.
    /// @return Number of tasks run.
    std::size_t runAll(std::size_t maxSteps = 10'000) {
        std::size_t ran = 0;
        while (ran < maxSteps && runOne()) {
            ++ran;
        }
        if (ran == maxSteps) {
            throw std::runtime_error(
                "StepExecutor::runAll: exceeded maxSteps -- a task is likely re-posting "
                "indefinitely; use runOne() to step through and find it");
        }
        return ran;
    }

    /// @brief Number of tasks currently queued, awaiting a `runOne()`/`runAll()`.
    /// @return Queue depth.
    [[nodiscard]] std::size_t pending() const {
        std::scoped_lock const lock{_mtx};
        return _queue.size();
    }

private:
    mutable std::mutex _mtx;
    std::deque<std::function<void()>> _queue;
};

/// @brief Default polling budget for `waitUntil`. Picked to cover the slowest
///        TSan/Valgrind runs without making green tests visibly slow.
inline constexpr std::chrono::milliseconds kDefaultWaitBudget{2000};

/// @brief Default polling step for `waitUntil`.
inline constexpr std::chrono::milliseconds kDefaultWaitStep{5};

/// @brief Polls @p pred until it returns `true` or @p budget elapses.
///
/// Returns `true` if the predicate eventually became `true`, `false` if the
/// budget expired first. Sleeps for @p step between polls so we don't burn the
/// CPU. Sized for asynchronous test fixtures: most callers should just write
/// `REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));`
template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds budget = kDefaultWaitBudget,
               std::chrono::milliseconds step = kDefaultWaitStep) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(step);
    }
    return true;
}

/// @brief Collects a single `RemoteServer` reply and decodes it.
///
/// Designed to be passed as the reply callback to `RemoteServer::handle()`:
///
/// @code
/// morph::testing::WaitReply waiter;
/// server->handle(envelopeJson, std::ref(waiter));
/// REQUIRE(waiter.await());
/// REQUIRE(waiter.env.kind == "ok");
/// @endcode
///
/// The raw reply string is kept available for tests that need to inspect
/// malformed responses (when `env` may have failed to decode).
struct WaitReply {
    std::atomic<bool> ready{false};
    std::string raw;
    ::morph::wire::Envelope env;

    /// @brief Reply-callback entry point.
    void operator()(const std::string& msg) {
        raw = msg;
        try {
            env = ::morph::wire::decode(msg);
        } catch (...) {
            // Leave `env` default-initialised; tests inspecting `raw` can still assert.
        }
        ready.store(true);
    }

    /// @brief Blocks (polling) until the reply arrives or @p budget elapses.
    /// @return `true` if a reply arrived within the budget.
    bool await(std::chrono::milliseconds budget = kDefaultWaitBudget) {
        return waitUntil([this] { return ready.load(); }, budget);
    }
};

}  // namespace morph::testing
