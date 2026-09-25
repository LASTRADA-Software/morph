// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <concepts>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "../strand.hpp"

/// @file
/// @brief What drives a model's Task handler: the driver coroutine that starts
///        and finishes it, the per-instance gate that keeps actions from
///        overlapping, and the trait that tells a Task handler from an ordinary
///        one. The resumer its resumptions go through is `strand.hpp`'s.
///
/// Specified in `docs/spec/core/coroutines.md`, "Model side".

namespace morph::model {

/// @brief The result an action handler's return type stands for: `R` for a
///        `core::async::Task<R>` handler, the type itself for any other.
/// @tparam T The handler's return type.
template <typename T>
struct HandlerResult {
    /// @brief The action's result type.
    using type = T;
    /// @brief Whether the handler is a coroutine returning `core::async::Task`.
    static constexpr bool isTask = false;
};

/// @brief `HandlerResult` for a Task handler.
/// @tparam R The value the Task produces.
template <typename R>
struct HandlerResult<::core::async::Task<R>> {
    /// @brief The action's result type: what the Task produces.
    using type = R;
    /// @brief Whether the handler is a coroutine returning `core::async::Task`.
    static constexpr bool isTask = true;
};

/// @brief The action result a handler returning @p T produces.
/// @tparam T The handler's return type.
template <typename T>
using HandlerResultT = HandlerResult<T>::type;

/// @brief Whether a handler returning @p T is a Task handler.
/// @tparam T The handler's return type.
template <typename T>
inline constexpr bool isTaskHandler = HandlerResult<T>::isTask;

namespace detail {

/// @brief Keeps a model instance's actions from overlapping, across a Task
///        handler's suspensions.
///
/// A strand serialises the tasks posted to it, and a suspended Task handler is
/// not one: while it waits the strand is free. Every action therefore enters
/// this gate on the strand before it runs and leaves it when it is finished --
/// for a Task handler, when the Task completes. An action that finds the gate
/// held is queued, in arrival order, and started by the `leave()` that frees
/// it. Touched only on the model's strand, so it takes no lock.
class ActionGate {
public:
    /// @brief Starts @p start now, or queues it behind the action holding the gate.
    ///
    /// The started action holds the gate until it calls `leave()`.
    ///
    /// A template rather than a `std::function` parameter: an action that
    /// starts at once is called as it is, and only one that has to wait is
    /// type-erased into the queue. libstdc++'s `std::function` heap-allocates
    /// any callable that is not trivially copyable, a lambda holding a
    /// `shared_ptr` included, and an action on a free gate need not pay that.
    /// @param start The action; must not throw. Copyable, as the queue's
    ///              `std::function` requires.
    template <std::invocable Start>
    void enter(Start&& start) {
        Occupancy const occupancy{*this};
        if (_held || (_waiting != nullptr && !_waiting->empty())) {
            if (_waiting == nullptr) {
                _waiting = std::make_unique<std::deque<std::function<void()>>>();
            }
            _waiting->emplace_back(std::forward<Start>(start));
            return;
        }
        _held = true;
        start();
    }

    /// @brief Takes the gate if it is free, for an action its caller then runs
    ///        itself; otherwise leaves it to `enter` to queue the action.
    ///
    /// For a caller whose action stays owned where it is while it runs: a
    /// backend's strand task, which holds an ordinary run by value, so an
    /// action on a free gate costs no allocation beyond the post.
    /// @return True if taken: the caller runs its action now, and the action
    ///         holds the gate until it calls `leave()`. False if the action has
    ///         to wait, through `enter`.
    [[nodiscard]] bool tryEnter() {
        Occupancy const occupancy{*this};
        if (_held || (_waiting != nullptr && !_waiting->empty())) {
            return false;
        }
        _held = true;
        return true;
    }

    /// @brief Releases the gate and starts the actions queued behind it, oldest
    ///        first, for as long as each one finishes without suspending.
    ///
    /// A loop rather than a recursion: a queue of ordinary handlers would
    /// otherwise nest one stack frame per queued action.
    void leave() {
        Occupancy const occupancy{*this};
        _held = false;
        if (_draining) {
            return;
        }
        _draining = true;
        while (!_held && _waiting != nullptr && !_waiting->empty()) {
            auto next = std::move(_waiting->front());
            _waiting->pop_front();
            _held = true;
            next();
        }
        _draining = false;
    }

    /// @brief How many times, process-wide, two threads have been inside
    ///        `enter` or `leave` of one gate at once.
    ///
    /// Always zero when the gate is used as specified: a gate is touched only
    /// on its model's strand. A diagnostic for tests, which assert that it
    /// does not change; the check that counts is one atomic compare-exchange
    /// per call.
    /// @return The count so far.
    [[nodiscard]] static std::size_t overlapsObserved() noexcept { return overlapCounter().load(); }

private:
    static std::atomic<std::size_t>& overlapCounter() noexcept {
        static std::atomic<std::size_t> counter{0};
        return counter;
    }

    /// Marks the calling thread as inside the gate for the scope's lifetime,
    /// and counts it if another thread already is. Reentry on the same thread,
    /// which `leave()` starting the next action does, is not an overlap.
    class Occupancy {
    public:
        explicit Occupancy(ActionGate& gate) noexcept : _gate{&gate} {
            auto expected = std::thread::id{};
            auto const self = std::this_thread::get_id();
            _owner = _gate->_occupant.compare_exchange_strong(expected, self);
            if (!_owner && expected != self) {
                overlapCounter().fetch_add(1);
            }
        }
        Occupancy(const Occupancy&) = delete;
        Occupancy& operator=(const Occupancy&) = delete;
        Occupancy(Occupancy&&) = delete;
        Occupancy& operator=(Occupancy&&) = delete;
        ~Occupancy() {
            if (_owner) {
                _gate->_occupant.store(std::thread::id{});
            }
        }

    private:
        ActionGate* _gate;
        bool _owner = false;
    };

    std::atomic<std::thread::id> _occupant;
    bool _held = false;
    bool _draining = false;
    /// Allocated the first time an action has to wait, which only a suspended
    /// Task handler causes: every model instance carries a gate, and one that
    /// never queues should cost a model holder nothing but a pointer.
    std::unique_ptr<std::deque<std::function<void()>>> _waiting;
};

/// @brief The coroutine that runs one Task handler to completion.
///
/// Created suspended so its promise's stop token can be set before the first
/// step; `startTaskHandler` resumes it. It frees itself at the end
/// (`final_suspend` never suspends). Its promise answers `stopToken()`, which is
/// how the token reaches the handler: `Task`'s awaiter copies the awaiting
/// promise's token into the handler's.
struct TaskHandlerDriver {
    // The compiler calls every member of a promise through the object, so none
    // is made static.
    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    struct promise_type {
        ::core::async::StopToken token;

        TaskHandlerDriver get_return_object() noexcept {
            return TaskHandlerDriver{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        [[nodiscard]] std::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        // Every exception is caught in the driver's body, which settles the call
        // with it; one escaping here would mean the settle itself threw.
        [[noreturn]] void unhandled_exception() const noexcept { std::terminate(); }
        [[nodiscard]] ::core::async::StopToken stopToken() const noexcept { return token; }
    };
    // NOLINTEND(readability-convert-member-functions-to-static)

    std::coroutine_handle<promise_type> handle;
};

/// @brief The driver body: awaits @p task and hands its outcome to @p done.
/// @tparam R The handler's result type.
/// @param executor The handler's resumer. Held in the frame, so it lives until
///        the handler has finished whatever holds it besides.
/// @param task     The handler's Task, not yet started.
/// @param done     Called once, on the strand, with the result or the exception.
/// @return The suspended driver.
template <typename R>
TaskHandlerDriver driveTaskHandler(std::shared_ptr<::morph::exec::detail::TaskResumer> executor,
                                   ::core::async::Task<R> task,
                                   std::function<void(std::optional<R>, std::exception_ptr)> done) {
    static_cast<void>(executor);
    std::optional<R> result;
    std::exception_ptr error;
    try {
        // The await is its own statement: MSVC cannot tail-call a call that
        // shares a full-expression with a `co_await` (C4737).
        R value = co_await std::move(task);
        result.emplace(std::move(value));
    } catch (...) {
        error = std::current_exception();
    }
    done(std::move(result), error);
}

/// @brief Starts a Task handler on the model's strand, in the calling strand task.
///
/// The handler's first step runs here, inside @p executor -- the current
/// executor, with the action's session -- and with the stop token set; every
/// later step is resumed through @p executor, on the same strand.
/// @tparam R The handler's result type.
/// @param executor The handler's resumer.
/// @param task     The handler's Task, as the handler call returned it.
/// @param token    The stop token the handler observes.
/// @param done     Called once, on the strand, with the result or the exception.
template <typename R>
void startTaskHandler(const std::shared_ptr<::morph::exec::detail::TaskResumer>& executor, ::core::async::Task<R> task,
                      ::core::async::StopToken token, std::function<void(std::optional<R>, std::exception_ptr)> done) {
    auto driver = driveTaskHandler<R>(executor, std::move(task), std::move(done));
    driver.handle.promise().token = std::move(token);
    executor->resumeHere(driver.handle);
}

}  // namespace detail

}  // namespace morph::model
