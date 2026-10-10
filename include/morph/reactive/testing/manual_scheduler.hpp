// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../scheduler.hpp"

/// @file
/// @brief `morph::reactive::testing::ManualScheduler`: a scheduler whose time moves only when a test says so.

namespace morph::reactive::testing {

/// @brief A deterministic `Scheduler`: its clock moves only in `advance()`, which fires what falls due on the
///        calling thread.
///
/// `advance(delta)` models time passing while the owner loop runs and is never blocked: the clock stops at each
/// deadline in turn, so `every()` fires once per elapsed period, and a timer a callback creates fires in the same
/// advance when its deadline falls within it. Timers due at the same instant fire in the order they were created
/// (a periodic timer keeps the place its first scheduling gave it). One exception keeps a self-re-arming
/// zero-delay timer from looping: a timer a callback creates with a non-positive delay is due at the next
/// `advance()`, not at the current instant.
///
/// It keeps the `Scheduler` contract: a timer cancelled by an earlier callback does not fire, a callback may
/// cancel any timer, its own included, and handles may outlive the scheduler. Not thread-safe.
class ManualScheduler final : public Scheduler {
public:
    ManualScheduler() = default;
    ~ManualScheduler() override = default;
    ManualScheduler(ManualScheduler const&) = delete;
    ManualScheduler& operator=(ManualScheduler const&) = delete;
    ManualScheduler(ManualScheduler&&) = delete;
    ManualScheduler& operator=(ManualScheduler&&) = delete;

    /// @brief Runs @p fn once, @p delay after the current time.
    /// @param delay How long to wait; a negative delay counts as zero, which is due at the next `advance()`,
    ///        `advance(0ms)` included.
    /// @param fn The callback.
    /// @return The handle; destroying it before the deadline cancels the call.
    [[nodiscard]] TimerHandle after(std::chrono::milliseconds delay, std::function<void()> fn) override {
        return add(std::max(delay, std::chrono::milliseconds{0}), std::chrono::milliseconds{0}, std::move(fn));
    }

    /// @brief Runs @p fn every @p period, starting one period after the current time.
    /// @param period The interval; must be positive.
    /// @param fn The callback.
    /// @return The handle; destroying it stops the repetition.
    /// @throws std::invalid_argument when @p period is not positive.
    [[nodiscard]] TimerHandle every(std::chrono::milliseconds period, std::function<void()> fn) override {
        if (period.count() <= 0) {
            throw std::invalid_argument{"ManualScheduler::every: the period must be positive"};
        }
        return add(period, period, std::move(fn));
    }

    /// @brief Moves the clock forward by @p delta, stopping at each deadline on the way to fire what is due
    ///        there.
    ///
    /// A callback sees the clock at its own deadline. A periodic timer is next due one period after the deadline
    /// it fired for. A callback that throws ends the advance: the exception propagates, the clock stays at that
    /// deadline, the throwing timer is rescheduled (periodic) or gone (one-shot) as if it had returned, and the
    /// rest fire at the next advance.
    /// @param delta How far to move; must not be negative.
    /// @throws std::invalid_argument when @p delta is negative.
    /// @throws std::logic_error when called from a timer callback.
    void advance(std::chrono::milliseconds delta) {
        if (delta.count() < 0) {
            throw std::invalid_argument{"ManualScheduler::advance: the delta must not be negative"};
        }
        if (_state->advancing) {
            throw std::logic_error{"ManualScheduler::advance: called from a timer callback"};
        }
        // What callbacks deferred during an earlier advance is due now, at the current time rather than at the
        // earlier deadline it was created at, so the clock never runs backwards.
        for (Timer& timer : _state->timers) {
            if (timer.deferred) {
                timer.deadline = _state->now;
                timer.deferred = false;
            }
        }
        _state->advancing = true;
        try {
            fireUntil(_state->now + delta);
        } catch (...) {
            _state->advancing = false;
            throw;
        }
        _state->advancing = false;
    }

    /// @brief The scheduler's clock: the sum of every `advance()` so far, or, inside a callback, that callback's
    ///        deadline. It never decreases.
    /// @return The current time.
    [[nodiscard]] std::chrono::milliseconds now() const { return _state->now; }

    /// @brief How many timers are scheduled: periodic ones until cancelled, one-shots until they fire or are
    ///        cancelled.
    /// @return The count.
    [[nodiscard]] std::size_t pendingTimers() const { return _state->timers.size(); }

private:
    struct Timer {
        std::uint64_t id;
        std::chrono::milliseconds deadline;
        std::chrono::milliseconds period;  // zero for a one-shot
        std::shared_ptr<std::function<void()> const> fn;
        // Created by a callback with a deadline at the current instant: waits for the next advance.
        bool deferred;
    };

    // Shared with every handle's cancel function, weakly, so a handle that outlives the scheduler finds it
    // gone instead of touching freed memory.
    struct State {
        std::chrono::milliseconds now{0};
        std::uint64_t nextId = 0;
        bool advancing = false;
        std::vector<Timer> timers;
    };

    void fireUntil(std::chrono::milliseconds target) {
        auto& timers = _state->timers;
        for (;;) {
            // Ids grow with creation order, which breaks deadline ties.
            auto due = timers.end();
            for (auto timer = timers.begin(); timer != timers.end(); ++timer) {
                if (!timer->deferred && timer->deadline <= target &&
                    (due == timers.end() ||
                     std::pair{timer->deadline, timer->id} < std::pair{due->deadline, due->id})) {
                    due = timer;
                }
            }
            if (due == timers.end()) {
                break;
            }
            _state->now = due->deadline;
            // Held here: the callback may cancel its own timer, which destroys the stored one.
            std::shared_ptr<std::function<void()> const> const fire = due->fn;
            if (due->period.count() > 0) {
                due->deadline += due->period;
            } else {
                timers.erase(due);
            }
            (*fire)();
        }
        _state->now = target;
    }

    TimerHandle add(std::chrono::milliseconds delay, std::chrono::milliseconds period, std::function<void()> fn) {
        auto const timerId = ++_state->nextId;
        _state->timers.push_back(Timer{.id = timerId,
                                       .deadline = _state->now + delay,
                                       .period = period,
                                       .fn = std::make_shared<std::function<void()> const>(std::move(fn)),
                                       .deferred = _state->advancing && delay.count() <= 0});
        auto const isTimer = [timerId](Timer const& timer) { return timer.id == timerId; };
        return TimerHandle{[weak = std::weak_ptr<State>{_state}, isTimer] {
                               if (auto const state = weak.lock()) {
                                   std::erase_if(state->timers, isTimer);
                               }
                           },
                           [weak = std::weak_ptr<State>{_state}, isTimer] {
                               auto const state = weak.lock();
                               return state != nullptr && std::ranges::any_of(state->timers, isTimer);
                           }};
    }

    std::shared_ptr<State> _state = std::make_shared<State>();
};

}  // namespace morph::reactive::testing
