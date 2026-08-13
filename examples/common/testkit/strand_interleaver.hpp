// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/executor.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <vector>

/// @file
/// The strand interleaver's companion harness to the fault proxy
/// (examples/TESTING.md): without it, strand-ordering bugs (kanban's
/// MoveTaskPosition centerpiece) are probabilistic stress runs rather than
/// reproducible interleavings. Sits underneath a StrandExecutor as its `base`
/// IExecutor so a test controls exactly which posted task runs next.

namespace morph::ladder::testkit {

/// @brief An `IExecutor` that queues every posted task and runs them only
///        when explicitly stepped — never on its own thread.
///
/// Single-threaded by construction: `post()` just appends to a deque under a
/// mutex (posts can legitimately arrive from other threads — e.g. a
/// `StrandExecutor` posting a same-key continuation from inside a running
/// task — but every task itself runs synchronously on whichever thread calls
/// `step()`/`runSchedule()`).
///
/// Unlike `ThreadPoolExecutor`/`StrandExecutor`, a task's exception is not
/// caught and logged here: it propagates straight out of `step()`/
/// `runSchedule()` to the caller. That is deliberate — the caller is a test,
/// and the exception is often a `REQUIRE` failure the test needs to see
/// rather than have silently swallowed.
class DeterministicExecutor : public ::morph::exec::IExecutor {
  public:
    void post(std::function<void()> task) override {
        std::lock_guard lock{_mtx};
        _queue.push_back(std::move(task));
    }

    /// @return The number of tasks currently queued and not yet run.
    [[nodiscard]] std::size_t pending() const {
        std::lock_guard lock{_mtx};
        return _queue.size();
    }

    /// @brief Runs the oldest-queued task. Throws if the queue is empty.
    void step() {
        std::function<void()> task;
        {
            std::lock_guard lock{_mtx};
            if (_queue.empty()) {
                throw std::runtime_error("DeterministicExecutor::step: queue is empty");
            }
            task = std::move(_queue.front());
            _queue.pop_front();
        }
        task();
    }

    /// @brief Runs tasks in the exact order given, by *current* queue
    ///        position at the moment each entry is consumed — so a task that
    ///        posts new work mid-schedule is reflected in later indices.
    ///        `order` must name every index that will exist by the time it's
    ///        reached; the simplest correct schedule is just `{0, 1, ..., n-1}`
    ///        run one at a time via repeated `step()` calls when a test only
    ///        wants strict FIFO — `runSchedule` exists for tests that
    ///        deliberately want a *non*-FIFO interleaving across two strands'
    ///        queues merged into one DeterministicExecutor.
    /// @param order The queue indices to run, in caller-chosen order, each
    ///              read against the queue's *current* contents at the
    ///              moment it is consumed (see above).
    void runSchedule(const std::vector<std::size_t>& order) {
        for (auto index : order) {
            std::function<void()> task;
            {
                std::lock_guard lock{_mtx};
                if (index >= _queue.size()) {
                    throw std::runtime_error("DeterministicExecutor::runSchedule: index beyond current queue size");
                }
                task = std::move(_queue[index]);
                _queue.erase(_queue.begin() + static_cast<std::ptrdiff_t>(index));
            }
            task();
        }
    }

  private:
    mutable std::mutex _mtx;
    std::deque<std::function<void()>> _queue;
};

}  // namespace morph::ladder::testkit
