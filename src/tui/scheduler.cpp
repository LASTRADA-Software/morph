// SPDX-License-Identifier: Apache-2.0

#include "tui/scheduler.hpp"

#include <core/async/ExecutorContext.hpp>
#include <cstdint>
#include <exception>
#include <morph/core/logger.hpp>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace morph::tui::detail {

struct LoopScheduler::State {
    /// One armed timer. Held by unique_ptr, so its address — the loop timer's state — never moves.
    struct Timer {
        State* state = nullptr;
        std::uint64_t id = 0;
        ::core::net::TimerId timer{};
        std::chrono::milliseconds period{0};
        std::function<void()> fn;
    };

    State(::core::net::EventLoop& eventLoop, exec::IExecutor& executor) : loop{&eventLoop}, owner{&executor} {}

    void arm(Timer& timer, std::chrono::milliseconds delay) const {
        timer.timer = loop->addTimer(loop->clock().now() + delay, &State::fire, &timer);
    }

    /// cancelTimer also retires a timer the loop has found due and queued but not yet run, which is what keeps a
    /// sibling cancelled earlier in the same turn from running.
    void cancel(std::uint64_t timerId) {
        auto const found = timers.find(timerId);
        if (found == timers.end()) {
            return;
        }
        static_cast<void>(loop->cancelTimer(found->second->timer));
        timers.erase(found);
    }

    void cancelAll() {
        for (auto const& entry : timers) {
            static_cast<void>(loop->cancelTimer(entry.second->timer));
        }
        timers.clear();
    }

    /// The loop's timer callback. The callback is copied out before it runs, because it may cancel its own timer
    /// or destroy the scheduler; nothing here touches the state after it returns. A periodic timer is re-armed
    /// from now, not from its missed deadline, so an owner blocked past several periods fires once.
    static void fire(void* armed) {
        auto* const timer = static_cast<Timer*>(armed);
        State& state = *timer->state;
        std::function<void()> const fn = timer->fn;
        if (timer->period.count() > 0) {
            state.arm(*timer, timer->period);
        } else {
            state.timers.erase(timer->id);
        }
        ::core::async::ExecutorScope const scope{state.owner->coreExecutor()};
        try {
            fn();
        } catch (std::exception const& failure) {
            ::morph::log::logError("[tui] timer callback threw: {}", failure.what());
        } catch (...) {
            ::morph::log::logError("[tui] timer callback threw an unknown exception");
        }
    }

    ::core::net::EventLoop* loop;
    exec::IExecutor* owner;
    std::uint64_t nextId = 0;
    std::unordered_map<std::uint64_t, std::unique_ptr<Timer>> timers;
};

LoopScheduler::LoopScheduler(::core::net::EventLoop& loop, exec::IExecutor& owner)
    : _state{std::make_shared<State>(loop, owner)} {}

LoopScheduler::~LoopScheduler() { _state->cancelAll(); }

reactive::TimerHandle LoopScheduler::after(std::chrono::milliseconds delay, std::function<void()> fn) {
    return add(delay, std::move(fn), std::chrono::milliseconds{0});
}

reactive::TimerHandle LoopScheduler::every(std::chrono::milliseconds period, std::function<void()> fn) {
    if (period.count() <= 0) {
        throw std::invalid_argument{"morph::tui: Scheduler::every needs a positive period"};
    }
    return add(period, std::move(fn), period);
}

std::size_t LoopScheduler::pendingTimers() const noexcept { return _state->timers.size(); }

reactive::TimerHandle LoopScheduler::add(std::chrono::milliseconds delay, std::function<void()> fn,
                                         std::chrono::milliseconds period) {
    auto const timerId = ++_state->nextId;
    auto timer = std::make_unique<State::Timer>(
        State::Timer{.state = _state.get(), .id = timerId, .timer = {}, .period = period, .fn = std::move(fn)});
    auto& armed = *timer;
    _state->timers.emplace(timerId, std::move(timer));
    _state->arm(armed, delay);
    return reactive::TimerHandle{[weak = std::weak_ptr<State>{_state}, timerId] {
        if (auto const state = weak.lock()) {
            state->cancel(timerId);
        }
    }};
}

}  // namespace morph::tui::detail
