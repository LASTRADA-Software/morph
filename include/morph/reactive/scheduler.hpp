// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <functional>
#include <utility>

/// @file
/// @brief `morph::reactive::Scheduler`: timers a frontend runs on the runtime's owner executor.
///
/// Specified in `docs/spec/reactive/control.md`, "Timed refresh".

namespace morph::reactive {

/// @brief Owns one scheduled timer and cancels it when destroyed. Move-only.
///
/// The handle knows only how to cancel: a one-shot timer that has fired leaves its handle `active()`, and
/// cancelling it then does nothing.
class TimerHandle {
public:
    /// @brief A handle that owns no timer.
    TimerHandle() = default;

    /// @brief Takes ownership of a timer through the function that cancels it.
    /// @param cancel Cancels the timer; called at most once. It must tolerate a timer that has already
    ///        fired or whose scheduler is gone.
    explicit TimerHandle(std::function<void()> cancel) : _cancel{std::move(cancel)} {}

    /// @brief Cancels the timer, if this handle still owns one.
    ~TimerHandle() { cancel(); }

    TimerHandle(TimerHandle const&) = delete;
    TimerHandle& operator=(TimerHandle const&) = delete;

    /// @brief Takes over @p other's timer; @p other then owns none.
    /// @param other The handle to move from.
    TimerHandle(TimerHandle&& other) noexcept : _cancel{std::exchange(other._cancel, {})} {}

    /// @brief Cancels this handle's timer, then takes over @p other's.
    /// @param other The handle to move from.
    /// @return `*this`.
    TimerHandle& operator=(TimerHandle&& other) noexcept {
        if (this != &other) {
            cancel();
            _cancel = std::exchange(other._cancel, {});
        }
        return *this;
    }

    /// @brief Cancels the timer, if this handle still owns one.
    void cancel() noexcept {
        if (auto const stop = std::exchange(_cancel, {})) {
            // Runs in the destructor and in move assignment, which cannot report a failure: a cancel function
            // that breaks its no-throw contract leaves its timer to the scheduler.
            try {
                stop();
            } catch (...) {  // NOLINT(bugprone-empty-catch): nothing to report to, as said above
            }
        }
    }

    /// @brief Whether this handle still owns a timer to cancel.
    /// @return False after `cancel()`, after a move from it, and for a default-constructed handle.
    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(_cancel); }

private:
    std::function<void()> _cancel;
};

/// @brief Runs callbacks after a delay or on a period, on the runtime's owner executor.
///
/// Every frontend provides one, on its own event loop's timers; a test uses `testing::ManualScheduler`. Every
/// implementation guarantees:
///
/// - Once a handle's `cancel()` returns on the owner, its callback never runs, even when its deadline has passed
///   and a call is already queued.
/// - `cancel()` may be called from inside a callback, its own timer's included.
/// - A `TimerHandle` may outlive its `Scheduler`; cancelling it then does nothing.
/// - `every()` fires once per period while the owner runs. If the owner is blocked past several periods, a
///   frontend fires once and counts the next deadline from that firing: no burst of missed firings.
class Scheduler {
public:
    Scheduler() = default;
    virtual ~Scheduler() = default;
    Scheduler(Scheduler const&) = delete;
    Scheduler& operator=(Scheduler const&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    /// @brief Runs @p fn once, @p delay from now.
    /// @param delay How long to wait; a non-positive delay means as soon as possible.
    /// @param fn The callback.
    /// @return The handle; destroying it before the deadline cancels the call.
    [[nodiscard]] virtual TimerHandle after(std::chrono::milliseconds delay, std::function<void()> fn) = 0;

    /// @brief Runs @p fn every @p period, starting one period from now.
    /// @param period The interval; must be positive.
    /// @param fn The callback.
    /// @return The handle; destroying it stops the repetition.
    /// @throws std::invalid_argument when @p period is not positive.
    [[nodiscard]] virtual TimerHandle every(std::chrono::milliseconds period, std::function<void()> fn) = 0;
};

}  // namespace morph::reactive
