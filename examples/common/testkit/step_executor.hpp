// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <morph/core/executor.hpp>
#include <mutex>
#include <stdexcept>

/// @file
/// The ladder testkit's worker-side executor double (examples/TESTING.md,
/// "Pumping discipline"): the substitute for a real `ThreadPoolExecutor`
/// underneath a submit-now/compute-later job, so a test asserts the exact
/// sequence submit -> still pending -> `runOne()` -> done, instead of polling
/// with a sleep and a retry cap.
///
/// This is a deliberate mirror of `morph::testing::StepExecutor`
/// (`tests/test_support.hpp`) under the ladder's own namespace, not a new
/// design: same name, same API, same rationale. It is duplicated rather than
/// shared for exactly the reason `strand_interleaver.hpp`'s
/// `DeterministicExecutor` is duplicated from that same header --
/// `tests/test_support.hpp` is a private header for `morph_tests`' own
/// translation units and has no reachable include path from `examples/`.
/// That reachability gap, not the absence of the semantics, is what left
/// every ladder async-job test spinning a real pool (morph#161).

namespace morph::ladder::testkit {

/// @brief An `IExecutor` that queues every posted task and runs them only when
///        the test explicitly asks, one at a time -- never on its own thread.
///
/// Where `DeterministicExecutor` (strand_interleaver.hpp) sits *underneath* a
/// `StrandExecutor` to control the delivery order of continuations, this sits
/// where a production `ThreadPoolExecutor` would: it is the worker. Injected
/// as the executor a model or App posts background work to, it turns
/// "eventually the job finishes" into a sequence of exact, assertable states:
///
/// ```cpp
/// StepExecutor worker;
/// const auto jobId = model.execute(SubmitReport{...});
/// CHECK(worker.pending() == 1);                       // queued, and not run
/// CHECK(status(jobId) == ReportStatus::Pending);      // asserted, not sampled
/// REQUIRE(worker.runOne());
/// CHECK(status(jobId) == ReportStatus::Done);         // no sleep, no cap
/// ```
///
/// The `pending()`/`runOne()` pair is what makes the *negative* half of that
/// assertable at all: "the worker has not run yet" is an ordinary `CHECK`
/// here, where against a real pool it can only be sampled and hoped for.
///
/// Single-threaded by construction: `post()` appends to a deque under a mutex
/// (posts can legitimately arrive from other threads -- code under test
/// posting a continuation from inside a running task -- so `post()` still
/// honours `IExecutor`'s thread-safe contract, `docs/spec/core/executor.md`),
/// but every task itself runs synchronously on whichever thread calls
/// `runOne()`/`runAll()`. Concurrent `runOne()`/`runAll()` calls are not
/// supported: a test reasoning about exact task ordering drives it from one
/// thread.
///
/// Unlike `ThreadPoolExecutor`, a task's exception is not caught and logged
/// here: it propagates straight out of `runOne()`/`runAll()` to the caller.
/// That is deliberate -- the caller is a test, and the exception is often a
/// `REQUIRE` failure the test needs to see rather than have swallowed.
class StepExecutor : public ::morph::exec::IExecutor {
public:
    /// @brief Enqueues @p task; does not run it.
    /// @param task Callable to run on a later `runOne()`/`runAll()` call.
    void post(std::function<void()> task) override {
        std::scoped_lock const lock{_mtx};
        _queue.push_back(std::move(task));
    }

    /// @brief Runs exactly one queued task, oldest first (FIFO).
    /// @return `true` if a task was run, `false` if the queue was empty --
    ///         returning rather than throwing so that "nothing more was
    ///         queued" is an ordinary `CHECK_FALSE`, which is half of what a
    ///         worker-side double is for.
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
    ///        itself posts -- so a chained job runs to completion rather than
    ///        stranding its own continuation.
    ///
    /// Bounded at @p maxSteps rather than looping until the queue is empty: a
    /// task that keeps re-posting work to this executor (a bug in the code
    /// under test, or a harness misuse) would otherwise be an undetectable
    /// infinite loop, hanging the test process with no assertion failure.
    /// @param maxSteps Upper bound on tasks run before giving up.
    /// @return Number of tasks run.
    /// @throws std::runtime_error if @p maxSteps is reached.
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

}  // namespace morph::ladder::testkit
