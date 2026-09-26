// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <chrono>
#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include "completion.hpp"
#include "detail/completion_awaiter.hpp"
#include "detail/task_handler.hpp"
#include "executor.hpp"
#include "logger.hpp"
#include "timeout_scheduler.hpp"

/// @file
/// @brief Coroutines on `core::async::Task`: awaiting a `Completion`, starting
///        a detached flow on a morph executor, and waiting on a timer.
///
/// `Completion<T>::operator co_await` itself lives in `completion.hpp`, so a
/// completion is awaitable wherever it is visible; this header adds `spawn` and
/// `delay`, and brings in what drives a Task-returning model handler. Specified
/// in `docs/spec/core/coroutines.md`.

namespace morph::async {

namespace detail {

/// @brief Resumes coroutines through a `morph::exec::IExecutor`, as the
///        current executor of each resumption.
class ExecutorResumer final : public ::core::async::IExecutor, public std::enable_shared_from_this<ExecutorResumer> {
public:
    /// @param executor Where every resumption is posted; must outlive them all.
    explicit ExecutorResumer(::morph::exec::IExecutor& executor MORPH_LIFETIMEBOUND) : _executor{executor} {}

    using ::core::async::IExecutor::submit;

    /// @brief Posts @p handle's resumption to the executor.
    /// @param handle The coroutine to resume; borrowed.
    void submit(std::coroutine_handle<> handle) override {
        _executor.post([self = shared_from_this(), handle] {
            std::shared_ptr<void> const keep = self;
            ::core::async::ExecutorScope const scope{*self, &keep, nullptr};
            handle.resume();
        });
    }

    /// @brief Posts @p work's resumption to the executor.
    /// @param work The coroutine to resume. The spawned task's driver owns its
    ///        frame, so the abandon claim is not taken.
    void submit(::core::async::ParkedWork work) override { submit(work.resume); }

private:
    ::morph::exec::IExecutor& _executor;
};

/// @brief Suspends and resumes through @p target: how a spawned task's first
///        step reaches its executor.
struct HopTo {
    ::core::async::IExecutor* target;

    // Called through the awaiter by the compiler, so not static.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> awaiting) const { target->submit(awaiting); }
    void await_resume() const noexcept {}
};

/// @brief The detached coroutine `spawn` starts: hops onto the executor, runs
///        the task there and logs what it lets escape.
struct SpawnedTask {
    // The compiler calls every member of a promise through the object, so none
    // is made static.
    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    struct promise_type {
        [[nodiscard]] SpawnedTask get_return_object() const noexcept { return {}; }
        [[nodiscard]] std::suspend_never initial_suspend() const noexcept { return {}; }
        [[nodiscard]] std::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        // Only the hop can throw past the body's catch: an executor that cannot
        // accept one closure, before anything has run.
        [[noreturn]] void unhandled_exception() const noexcept { std::terminate(); }
    };
    // NOLINTEND(readability-convert-member-functions-to-static)
};

/// @brief The body of `spawn`.
/// @param resumer The executor adapter; owned here for the task's lifetime.
/// @param task    The task to run.
/// @return Nothing to hold: the coroutine frees itself when it finishes.
inline SpawnedTask runSpawned(std::shared_ptr<ExecutorResumer> resumer, ::core::async::Task<void> task) {
    co_await HopTo{resumer.get()};
    try {
        co_await std::move(task);
    } catch (const std::exception& exc) {
        ::morph::log::logError("[spawn] task threw: {}", exc.what());
    } catch (...) {
        ::morph::log::logError("[spawn] task threw unknown exception");
    }
}

}  // namespace detail

/// @brief Starts @p task detached, with every resumption -- its first step
///        included -- posted to @p executor.
///
/// The entry point from code that is not itself a coroutine: with a
/// `QtExecutor`, a GUI flow written as a coroutine runs every step on the GUI
/// thread. Awaits inside @p task resume on @p executor, whichever thread the
/// awaited operation completed on. An exception @p task lets escape is logged
/// through `morph::log` and swallowed. Its stop token is never stopped.
/// @param executor Where the task runs; must outlive it.
/// @param task     The task to run.
inline void spawn(::morph::exec::IExecutor& executor, ::core::async::Task<void> task) {
    detail::runSpawned(std::make_shared<detail::ExecutorResumer>(executor), std::move(task));
}

/// @brief The awaiter `delay()` returns: suspends until a `TimeoutScheduler`
///        entry fires, or until a stop withdraws it.
///
/// Resumes on the executor the awaiting coroutine was running on (core-cpp's
/// current executor), or on the scheduler's thread if there was none. With a
/// stoppable token on the awaiting promise, a stop cancels the scheduler entry
/// -- releasing its capture at once -- and resumes the coroutine with
/// `core::async::OperationCancelled` on the same executor, or, with none,
/// inline on the thread that requested the stop. Exactly one of the timer and the stop resumes it.
class DelayAwaiter {
    enum class Outcome : std::uint8_t { Pending, Fired, Cancelled };

    struct Shared;

    struct OnStop {
        Shared* shared;
        void operator()() const noexcept { shared->onStop(); }
    };

    struct Shared : std::enable_shared_from_this<Shared> {
        std::atomic<Outcome> outcome{Outcome::Pending};
        std::atomic<bool> armed{false};
        std::atomic<bool> claimed{false};
        std::atomic<detail::TimeoutScheduler::Handle> timer{0};
        std::coroutine_handle<> continuation;
        ::core::async::ResumeTarget context;
        detail::TimeoutScheduler* scheduler = nullptr;
        std::optional<::core::async::StopCallback<OnStop>> stopCallback;

        // seq_cst, as `armed` is: whoever decides and then reads `armed` must
        // not miss the other side's store (a store-buffering pattern).
        bool decide(Outcome outcome_) noexcept {
            auto expected = Outcome::Pending;
            return outcome.compare_exchange_strong(expected, outcome_, std::memory_order_seq_cst);
        }

        void resume() const {
            if (context) {
                context.submit(::core::async::ParkedWork{.resume = continuation});
            } else {
                continuation.resume();
            }
        }

        void onStop() noexcept {
            if (!decide(Outcome::Cancelled)) {
                return;
            }
            try {
                // The coroutine may be resumed below and free the awaiter;
                // this keeps the state alive until the callback has returned.
                auto const keep = shared_from_this();
                scheduler->cancel(timer.load());
                if (armed.load(std::memory_order_seq_cst) && !claimed.exchange(true, std::memory_order_acq_rel)) {
                    resume();
                }
            } catch (...) {  // NOLINT(bugprone-empty-catch): a stop callback must not throw
                // No executor to resume through: the coroutine stays
                // suspended until its owner destroys it.
            }
        }
    };

public:
    /// @param scheduler The scheduler whose entry times the wait; must outlive it.
    /// @param duration  How long to wait at least.
    DelayAwaiter(detail::TimeoutScheduler& scheduler MORPH_LIFETIMEBOUND, std::chrono::milliseconds duration)
        : _shared{std::make_shared<Shared>()}, _duration{duration} {
        _shared->scheduler = &scheduler;
    }

    /// @brief Withdraws the wait if the frame is destroyed while suspended.
    ~DelayAwaiter() {
        _shared->stopCallback.reset();
        if (_shared->decide(Outcome::Cancelled)) {
            _shared->scheduler->cancel(_shared->timer.load());
        }
    }

    DelayAwaiter(const DelayAwaiter&) = delete;
    DelayAwaiter& operator=(const DelayAwaiter&) = delete;
    DelayAwaiter(DelayAwaiter&&) = delete;
    DelayAwaiter& operator=(DelayAwaiter&&) = delete;

    /// @return Always false: every decision is `await_suspend`'s.
    // Called through the awaiter by the compiler, so not static.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return False to continue at once, when a stop was already requested;
    ///         true once the timer is armed.
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> awaiting) {
        // Copied before `armed` is published: from then on a stop may resume
        // the coroutine and free this awaiter with its frame.
        auto shared = _shared;
        auto const duration = _duration;
        shared->continuation = awaiting;
        shared->context = ::core::async::ResumeTarget::current();
        if constexpr (::core::async::HasStopToken<Promise>) {
            ::core::async::StopToken const token = awaiting.promise().stopToken();
            if (token.stop_requested()) {
                shared->outcome.store(Outcome::Cancelled);
                return false;
            }
            if (token.stop_possible()) {
                shared->stopCallback.emplace(token, OnStop{shared.get()});
            }
        }
        shared->armed.store(true, std::memory_order_seq_cst);
        if (shared->outcome.load(std::memory_order_seq_cst) == Outcome::Cancelled) {
            return shared->claimed.exchange(true, std::memory_order_acq_rel);
        }
        auto const handle = shared->scheduler->schedule(duration, [shared] {
            if (shared->decide(Outcome::Fired)) {
                shared->resume();
            }
        });
        shared->timer.store(handle);
        // A stop that won between arming and here found no timer to cancel.
        if (shared->outcome.load() == Outcome::Cancelled) {
            shared->scheduler->cancel(handle);
        }
        return true;
    }

    /// @throws core::async::OperationCancelled if a stop withdrew the wait.
    void await_resume() {
        _shared->stopCallback.reset();
        if (_shared->outcome.load() == Outcome::Cancelled) {
            throw ::core::async::OperationCancelled{};
        }
    }

private:
    std::shared_ptr<Shared> _shared;
    std::chrono::milliseconds _duration;
};

/// @brief Suspends the awaiting coroutine for at least @p duration.
///
/// `co_await morph::async::delay(scheduler, 50ms)`. Stop-aware; see
/// `DelayAwaiter`.
/// @param scheduler The scheduler whose entry times the wait; must outlive it.
/// @param duration  How long to wait at least.
/// @return The awaiter.
[[nodiscard]] inline DelayAwaiter delay(detail::TimeoutScheduler& scheduler MORPH_LIFETIMEBOUND,
                                        std::chrono::milliseconds duration) {
    return DelayAwaiter{scheduler, duration};
}

}  // namespace morph::async
