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
/// A scheduler hands it two functions: one that cancels the timer and one that says whether the timer is still
/// scheduled. So `active()` turns false once a one-shot timer has fired, and cancelling a fired timer does nothing.
class TimerHandle {
public:
    /// @brief A handle that owns no timer.
    TimerHandle() = default;

    /// @brief Takes ownership of a timer through the function that cancels it.
    /// @param cancel Cancels the timer; called at most once. It must tolerate a timer that has already
    ///        fired or whose scheduler is gone.
    /// @param isScheduled Whether the timer is still scheduled: false once a one-shot timer has fired or its
    ///        scheduler is gone. Empty means the handle cannot tell, and `active()` stays true until `cancel()`.
    explicit TimerHandle(std::function<void()> cancel, std::function<bool()> isScheduled = {})
        : _cancel{std::move(cancel)}, _isScheduled{std::move(isScheduled)} {}

    /// @brief Cancels the timer, if this handle still owns one.
    ~TimerHandle() { cancel(); }

    TimerHandle(TimerHandle const&) = delete;
    TimerHandle& operator=(TimerHandle const&) = delete;

    /// @brief Takes over @p other's timer; @p other then owns none.
    /// @param other The handle to move from.
    TimerHandle(TimerHandle&& other) noexcept
        : _cancel{std::exchange(other._cancel, {})}, _isScheduled{std::exchange(other._isScheduled, {})} {}

    /// @brief Cancels this handle's timer, then takes over @p other's.
    /// @param other The handle to move from.
    /// @return `*this`.
    TimerHandle& operator=(TimerHandle&& other) noexcept {
        if (this != &other) {
            cancel();
            _cancel = std::exchange(other._cancel, {});
            _isScheduled = std::exchange(other._isScheduled, {});
        }
        return *this;
    }

    /// @brief Cancels the timer, if this handle still owns one.
    void cancel() noexcept {
        _isScheduled = {};
        if (auto const stop = std::exchange(_cancel, {})) {
            // Runs in the destructor and in move assignment, which cannot report a failure: a cancel function
            // that breaks its no-throw contract leaves its timer to the scheduler.
            try {
                stop();
            } catch (...) {  // NOLINT(bugprone-empty-catch): nothing to report to, as said above
            }
        }
    }

    /// @brief Whether this handle still owns a scheduled timer.
    /// @return False after `cancel()`, after a move from it, for a default-constructed handle, and once a one-shot
    ///         timer has fired or its scheduler is gone (when the scheduler can say so).
    [[nodiscard]] bool active() const noexcept {
        if (!_cancel) {
            return false;
        }
        if (!_isScheduled) {
            return true;
        }
        // A probe that breaks its no-throw contract cannot say the timer is still there.
        try {
            return _isScheduled();
        } catch (...) {
            return false;
        }
    }

private:
    std::function<void()> _cancel;
    std::function<bool()> _isScheduled;
};

/// @brief Runs callbacks after a delay or on a period, on the runtime's owner executor.
///
/// Every frontend provides one, on its own event loop's timers; a test uses `testing::ManualScheduler`. Every
/// implementation guarantees:
///
/// - Callbacks run on the owner executor of the `Runtime` the timers serve.
/// - A one-shot timer's handle reports `active()` false once the timer has fired.
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
