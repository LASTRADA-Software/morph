// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace morph::async::detail {

/// @brief Shared state for a coroutine-based `Task<T>`.
///
/// Stores the result (or exception) and a single continuation that fires
/// when the coroutine completes. Used internally by `LocalBackend` to bridge
/// coroutine completion to `Completion<T>`.
template <typename T>
struct TaskState {
    std::mutex mtx;

    /// @brief Set when the coroutine returns a value.
    std::optional<T> value;

    /// @brief Set when the coroutine throws.
    std::exception_ptr error;

    bool ready = false;

    /// @brief Called once when the task becomes ready.
    std::function<void(TaskState&)> continuation;

    /// @brief Stores @p val as the result and fires the continuation if attached.
    /// @param val Result value from the coroutine.
    void setValue(T val) {
        std::function<void(TaskState&)> cont;
        {
            std::scoped_lock lock{mtx};
            value = std::move(val);
            ready = true;
            cont = std::move(continuation);
        }
        if (cont) {
            cont(*this);
        }
    }

    /// @brief Stores @p exc as the error and fires the continuation if attached.
    /// @param exc Exception pointer captured from the coroutine frame.
    void setException(const std::exception_ptr& exc) {
        std::function<void(TaskState&)> cont;
        {
            std::scoped_lock lock{mtx};
            error = exc;
            ready = true;
            cont = std::move(continuation);
        }
        if (cont) {
            cont(*this);
        }
    }

    /// @brief Attaches a continuation to be called when the task completes.
    ///
    /// If the task is already ready, @p cont is called immediately (inline).
    /// Otherwise it is stored and called by `setValue()` or `setException()`.
    /// @param cont Callable receiving `TaskState&` with the final result.
    void attach(std::function<void(TaskState&)> cont) {
        {
            std::scoped_lock lock{mtx};
            if (!ready) {
                continuation = std::move(cont);
                return;
            }
        }
        cont(*this);
    }
};

/// @brief Eagerly-started coroutine task that delivers its result via `TaskState<T>`.
///
/// Used internally by `LocalBackend::execute()` to wrap a synchronous model
/// operation in a coroutine frame so it can be dispatched onto a `StrandExecutor`.
///
/// @tparam T Return type of the coroutine.
template <typename T>
class Task {
public:
    // NOLINTBEGIN(readability-identifier-naming) — coroutine protocol requires these names
    struct promise_type {
        std::shared_ptr<TaskState<T>> state = std::make_shared<TaskState<T>>();

        /// @brief Returns the `Task` object that the caller receives.
        Task get_return_object() { return Task{state}; }

        /// @brief Runs eagerly — no initial suspension.
        std::suspend_never initial_suspend() noexcept { return {}; }

        /// @brief No suspension on exit.
        std::suspend_never final_suspend() noexcept { return {}; }

        /// @brief Stores the return value in the shared state.
        void return_value(T val) { state->setValue(std::move(val)); }

        /// @brief Stores the active exception in the shared state.
        void unhandled_exception() { state->setException(std::current_exception()); }
    };
    // NOLINTEND(readability-identifier-naming)

    /// @brief Returns the shared state that holds the eventual result.
    /// @return Shared pointer to the task state.
    [[nodiscard]] std::shared_ptr<TaskState<T>> state() const { return _state; }

private:
    explicit Task(std::shared_ptr<TaskState<T>> statePtr) : _state{std::move(statePtr)} {}
    std::shared_ptr<TaskState<T>> _state;
};

}  // namespace morph::async::detail
