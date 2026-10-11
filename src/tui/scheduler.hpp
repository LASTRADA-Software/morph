// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <core/net/EventLoop.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/reactive/scheduler.hpp>

namespace morph::tui::detail {

/// reactive::Scheduler on a core::net::EventLoop's timers: each callback runs in a loop turn with the owner
/// executor current, so it may write the reactive state of a runtime that executor owns. Used on the loop's
/// thread only.
///
/// A periodic timer is re-armed one period from the loop clock's `now` at each firing, not from the deadline it
/// fired for: an owner blocked past several periods fires once, and the period drifts by each firing's dispatch
/// latency. Destroy it before its EventLoop: the destructor retires its armed timers on that loop.
class LoopScheduler final : public reactive::Scheduler {
public:
    LoopScheduler(::core::net::EventLoop& loop, exec::IExecutor& owner);
    /// @brief Cancels every armed timer, queued ones included: each names state this destructor frees.
    ~LoopScheduler() override;
    LoopScheduler(LoopScheduler const&) = delete;
    LoopScheduler& operator=(LoopScheduler const&) = delete;
    LoopScheduler(LoopScheduler&&) = delete;
    LoopScheduler& operator=(LoopScheduler&&) = delete;

    [[nodiscard]] reactive::TimerHandle after(std::chrono::milliseconds delay, std::function<void()> fn) override;
    [[nodiscard]] reactive::TimerHandle every(std::chrono::milliseconds period, std::function<void()> fn) override;
    /// How many timers are armed.
    [[nodiscard]] std::size_t pendingTimers() const noexcept;

private:
    struct State;
    /// Arms @p fn @p delay from now, then every @p period after each firing; a zero @p period fires once.
    [[nodiscard]] reactive::TimerHandle add(std::chrono::milliseconds delay, std::function<void()> fn,
                                            std::chrono::milliseconds period);

    /// Shared with each handle's cancel, which holds it weakly: a handle may outlive the scheduler.
    std::shared_ptr<State> _state;
};

}  // namespace morph::tui::detail
