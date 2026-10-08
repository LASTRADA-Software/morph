// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "../core/executor.hpp"

/// @file
/// @brief `morph::testing::StepExecutor`: an executor whose posted tasks run only when a test steps it.

namespace morph::testing {

/// @brief `IExecutor` that queues every posted task and runs them only when the
///        test explicitly asks, one at a time.
///
/// The public interleaving-test harness: any server
/// component built on `morph::exec::IExecutor` — `RemoteServer` included — can
/// be driven with fully deterministic, hand-stepped task ordering by
/// constructing it against a `StepExecutor` instead of a `ThreadPoolExecutor`.
/// `RemoteServer` posts every dispatch (both the top-level `handle()` post and
/// the per-model strand dispatch its internal `ModelStrands` performs) onto
/// whichever `IExecutor` it was constructed with, so controlling that one
/// executor is enough to control ordering end-to-end — no need to name
/// `morph::exec::detail::ModelStrands` or `morph::exec::detail::ModelId` to
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

}  // namespace morph::testing
