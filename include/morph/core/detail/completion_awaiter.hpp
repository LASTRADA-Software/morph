// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/StopToken.hpp>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "../callback_scope.hpp"
#include "../executor.hpp"

/// @file
/// @brief The awaiter behind `Completion<T>::operator co_await`, and the
///        resumption context every morph awaiter resumes through.
///
/// A fragment of `completion.hpp` rather than of `coroutine.hpp`, so a
/// `Completion<T>` is awaitable wherever it is visible. Specified in
/// `docs/spec/core/coroutines.md`.

namespace morph::async::detail {

template <typename T>
struct CompletionState;

/// @brief The calling thread's resumption context: the executor a coroutine
///        suspending here is resumed through, or null for none.
///
/// A thread-local, installed by `ScopedResumeContext` for the duration of each
/// resumption by the executors that resume coroutines (`spawn`'s adapter and
/// `StrandCoroExecutor`), and read by every morph awaiter in `await_suspend`.
/// `core::async::Task` carries a stop token from awaiter to awaitee but no
/// executor, so this is where "the context a coroutine suspended in" lives.
/// @return A reference to the calling thread's slot.
[[nodiscard]] inline ::core::async::IExecutor*& resumeContextSlot() noexcept {
    thread_local ::core::async::IExecutor* slot = nullptr;
    return slot;
}

/// @brief The calling thread's resumption context.
/// @return The executor installed by the innermost `ScopedResumeContext`, or null.
[[nodiscard]] inline ::core::async::IExecutor* currentResumeContext() noexcept { return resumeContextSlot(); }

/// @brief Installs a resumption context for its own lifetime, restoring the
///        previous one after.
class ScopedResumeContext {
public:
    /// @param context The executor coroutines suspending in this scope resume through.
    explicit ScopedResumeContext(::core::async::IExecutor* context) noexcept
        : _previous{std::exchange(resumeContextSlot(), context)} {}
    ~ScopedResumeContext() { resumeContextSlot() = _previous; }

    ScopedResumeContext(const ScopedResumeContext&) = delete;
    ScopedResumeContext& operator=(const ScopedResumeContext&) = delete;
    ScopedResumeContext(ScopedResumeContext&&) = delete;
    ScopedResumeContext& operator=(ScopedResumeContext&&) = delete;

private:
    ::core::async::IExecutor* _previous;
};

/// @brief The awaiter `Completion<T>::operator co_await() &&` returns.
///
/// The await is one more `then`/`onError` pair on the completion, holding only
/// a heap `Shared` -- never the coroutine frame -- and gated by a
/// `CallbackToken`. With a stoppable token on the awaiting promise it also
/// registers a stop callback. Exactly one of *the completion's handler* and
/// *the stop callback* moves `Shared::outcome` off `Pending`, and only that one
/// resumes the coroutine.
///
/// `await_suspend` touches only local copies of the shared pointers once it has
/// published anything another thread could act on: from then on the coroutine
/// may already have been resumed elsewhere, and the awaiter with its frame
/// destroyed.
/// @tparam T The completion's value type; copied out of the settled state.
template <typename T>
class CompletionAwaiter {
    enum class Outcome : std::uint8_t { Pending, Settled, Cancelled };

    struct Shared;

    /// The stop callback's callable: forwards to `Shared::onStop`.
    struct OnStop {
        Shared* shared;
        void operator()() const noexcept { shared->onStop(); }
    };

    struct Shared {
        std::atomic<Outcome> outcome{Outcome::Pending};
        /// Set once `await_suspend` has finished registering; before that a
        /// stop only records itself and `await_suspend` answers it.
        std::atomic<bool> armed{false};
        /// Taken by whichever of `await_suspend` and `onStop` answers a stop.
        std::atomic<bool> claimed{false};
        std::optional<T> value;
        std::exception_ptr error;
        std::coroutine_handle<> continuation;
        ::core::async::IExecutor* context = nullptr;
        ::morph::exec::IExecutor* fallback = nullptr;
        CallbackScope scope;
        std::optional<::core::async::StopCallback<OnStop>> stopCallback;

        // seq_cst, as `armed` is: whoever decides and then reads `armed` must
        // not miss the other side's store (a store-buffering pattern).
        bool decide(Outcome outcome_) noexcept {
            auto expected = Outcome::Pending;
            return outcome.compare_exchange_strong(expected, outcome_, std::memory_order_seq_cst);
        }

        /// A settled completion, on the completion's executor: resumes in the
        /// suspending context, or right here.
        void resumeSettled() {
            if (context != nullptr) {
                context->submit(continuation);
            } else {
                continuation.resume();
            }
        }

        /// A stop, on whichever thread requested it: resumes through the
        /// suspending context, or on the completion's executor. Not inline
        /// here, because the requesting thread is not the coroutine's; the
        /// context itself may resume inline (a `StrandCoroExecutor` whose
        /// backend has closed its strand does).
        void resumeCancelled() {
            if (context != nullptr) {
                context->submit(continuation);
            } else {
                fallback->post([handle = continuation] { handle.resume(); });
            }
        }

        void onStop() noexcept {
            if (!decide(Outcome::Cancelled)) {
                return;
            }
            scope.requestStop();
            if (armed.load(std::memory_order_seq_cst) && !claimed.exchange(true, std::memory_order_acq_rel)) {
                try {
                    resumeCancelled();
                } catch (...) {  // NOLINT(bugprone-empty-catch): a stop callback must not throw
                    // A queue that cannot accept one more closure leaves nothing to
                    // resume through; the coroutine stays suspended until its
                    // owner destroys it.
                }
            }
        }
    };

public:
    /// @param state The completion's state; consumed. Null for an empty completion.
    explicit CompletionAwaiter(std::shared_ptr<CompletionState<T>> state)
        : _state{std::move(state)}, _shared{std::make_shared<Shared>()} {}

    /// Withdraws the await if the frame is destroyed while suspended, so neither
    /// a later settlement nor a later stop resumes a frame that is gone.
    ~CompletionAwaiter() {
        _shared->stopCallback.reset();
        _shared->scope.requestStop();
    }

    CompletionAwaiter(const CompletionAwaiter&) = delete;
    CompletionAwaiter& operator=(const CompletionAwaiter&) = delete;
    CompletionAwaiter(CompletionAwaiter&&) = delete;
    CompletionAwaiter& operator=(CompletionAwaiter&&) = delete;

    /// @return Always false: every decision is `await_suspend`'s, so this stays
    ///         a constant (see core-cpp's `awaitReadyIsConstantFalse`).
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return False to continue at once (an unusable completion, or a stop
    ///         already requested); true once the await is attached.
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> awaiting) {
        auto shared = _shared;
        auto state = _state;
        if (state == nullptr || state->cbExec == nullptr) {
            shared->error = std::make_exception_ptr(std::logic_error{
                state == nullptr ? "co_await on an empty morph::async::Completion (default-constructed or moved-from)"
                                 : "co_await on a morph::async::Completion with no callback executor to resume on"});
            shared->outcome.store(Outcome::Settled);
            return false;
        }
        shared->continuation = awaiting;
        shared->context = currentResumeContext();
        shared->fallback = state->cbExec;

        if constexpr (::core::async::HasStopToken<Promise>) {
            ::core::async::StopToken const token = awaiting.promise().stopToken();
            if (token.stop_requested()) {
                shared->outcome.store(Outcome::Cancelled);
                return false;
            }
            if (token.stop_possible()) {
                shared->stopCallback.emplace(token, OnStop{shared.get()});
                shared->armed.store(true, std::memory_order_seq_cst);
                if (shared->outcome.load(std::memory_order_seq_cst) == Outcome::Cancelled) {
                    // A stop landed while registering. Whoever claims answers
                    // it; if onStop claimed first it is resuming us already.
                    return shared->claimed.exchange(true, std::memory_order_acq_rel);
                }
            }
        }

        auto const token = shared->scope.token();
        try {
            state->attachThen([token, shared](const T& settled) {
                if (token.active() && shared->decide(Outcome::Settled)) {
                    shared->value.emplace(settled);
                    shared->resumeSettled();
                }
            });
            state->attachOnError([token, shared](std::exception_ptr error) {
                if (token.active() && shared->decide(Outcome::Settled)) {
                    shared->error = std::move(error);
                    shared->resumeSettled();
                }
            });
        } catch (...) {
            // The throw resumes the coroutine, so nothing else may. If a stop
            // has claimed the await it is resuming the coroutine already, and
            // the frame is no longer this call's to throw into: that stop's
            // OperationCancelled is what the coroutine sees.
            if (shared->claimed.exchange(true, std::memory_order_acq_rel)) {
                shared->scope.requestStop();
                return true;
            }
            // Likewise if the handler attached before the throw has already
            // settled the await: it resumed the coroutine.
            if (!shared->decide(Outcome::Settled) && shared->outcome.load() == Outcome::Settled) {
                shared->scope.requestStop();
                return true;
            }
            // Still suspended, and decided: withdraw the stop callback (waiting
            // out one that is running) and the handler attached before the throw.
            shared->stopCallback.reset();
            shared->scope.requestStop();
            throw;
        }
        return true;
    }

    /// @return The settled value.
    /// @throws core::async::OperationCancelled if a stop withdrew the await.
    /// @throws Whatever the completion was rejected with.
    /// @throws std::logic_error for an empty completion or one with no executor.
    T await_resume() {
        _shared->stopCallback.reset();
        if (_shared->outcome.load() == Outcome::Cancelled) {
            throw ::core::async::OperationCancelled{};
        }
        if (_shared->error) {
            std::rethrow_exception(_shared->error);
        }
        return std::move(*_shared->value);
    }

private:
    std::shared_ptr<CompletionState<T>> _state;
    std::shared_ptr<Shared> _shared;
};

}  // namespace morph::async::detail
