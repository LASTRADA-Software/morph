// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "executor.hpp"
#include "logger.hpp"

namespace morph::async {

namespace detail {

// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
template <typename T>
struct CompletionState {
    std::mutex mtx;
    std::optional<T> value;
    std::exception_ptr error;
    bool ready = false;
    std::function<void(T)> onOk;
    std::function<void(std::exception_ptr)> onErr;
    bool onErrAttached = false;
    ::morph::exec::IExecutor* cbExec = nullptr;

    void setValue(T val) {
        std::function<void()> callback;
        {
            std::scoped_lock const lock{mtx};
            if (ready) {
                return;
            }
            value = std::move(val);
            ready = true;
            if (onOk) {
                auto savedFn = std::move(onOk);
                auto savedVal = std::move(*value);
                callback = [savedFn = std::move(savedFn), savedVal = std::move(savedVal)]() mutable {
                    savedFn(std::move(savedVal));
                };
            }
        }
        if (callback != nullptr && cbExec != nullptr) {
            cbExec->post(std::move(callback));
        }
    }
    void setException(const std::exception_ptr& exc) {
        std::function<void()> callback;
        {
            std::scoped_lock const lock{mtx};
            if (ready) {
                return;
            }
            error = exc;
            ready = true;
            if (onErr) {
                auto savedFn = std::move(onErr);
                auto savedErr = error;
                callback = [savedFn = std::move(savedFn), savedErr]() mutable { savedFn(savedErr); };
                onErrAttached = true;
            }
        }
        if (callback != nullptr && cbExec != nullptr) {
            cbExec->post(std::move(callback));
        }
    }
    void attachThen(std::function<void(T)> handler) {
        std::function<void()> fireNow;
        {
            std::scoped_lock const lock{mtx};
            if (ready && value) {
                auto savedVal = *value;
                fireNow = [handler = std::move(handler), savedVal]() mutable { handler(std::move(savedVal)); };
            } else if (!ready) {
                onOk = std::move(handler);
            }
        }
        if (fireNow != nullptr && cbExec != nullptr) {
            cbExec->post(std::move(fireNow));
        }
    }
    void attachOnError(std::function<void(std::exception_ptr)> handler) {
        std::function<void()> fireNow;
        {
            std::scoped_lock const lock{mtx};
            onErrAttached = true;
            if (ready && error) {
                auto savedErr = error;
                fireNow = [handler = std::move(handler), savedErr]() mutable { handler(savedErr); };
            } else if (!ready) {
                onErr = std::move(handler);
            }
        }
        if (fireNow != nullptr && cbExec != nullptr) {
            cbExec->post(std::move(fireNow));
        }
    }
    ~CompletionState() {
        if (!ready || !error || onErrAttached) {
            return;
        }
        // NOLINTBEGIN(bugprone-empty-catch) — logError may throw; we swallow to avoid noexcept-escape
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exc) {
            try {
                ::morph::log::logError("[orphan] unhandled exception: " + std::string{exc.what()});
            } catch (...) {
            }
        } catch (...) {
            try {
                ::morph::log::logError("[orphan] unhandled unknown exception");
            } catch (...) {
            }
        }
        // NOLINTEND(bugprone-empty-catch)
    }
};

// NOLINTEND(cppcoreguidelines-special-member-functions)

}  // namespace detail

/// @brief Move-only handle representing the eventual result of an asynchronous operation.
///
/// Callbacks are posted to the `IExecutor` supplied at construction time, so
/// they always run on the intended thread (e.g. the GUI thread).
///
/// @par Thread safety
/// `then()` and `onError()` may be called from any thread. The registered
/// callbacks are invoked via the executor, never directly from the producing thread.
///
/// @par Orphan detection
/// If a `Completion` is destroyed before an `onError()` handler is attached and
/// the operation has already failed, the exception is logged as an orphan error.
///
/// @tparam T Type of the success value.
template <typename T>
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class Completion {
public:
    /// @brief Constructs an empty (no-op) completion.
    Completion() = default;

    /// @brief Constructs a completion backed by @p statePtr, delivering callbacks via @p execPtr.
    /// @param statePtr Shared state produced by the backend.
    /// @param execPtr  Executor on which callbacks are posted. May be `nullptr` (direct call).
    Completion(std::shared_ptr<detail::CompletionState<T>> statePtr, ::morph::exec::IExecutor* execPtr)
        : _state{std::move(statePtr)} {
        if (_state != nullptr) {
            _state->cbExec = execPtr;
        }
    }

    /// @brief Move constructor — transfers ownership of the underlying state.
    Completion(Completion&&) noexcept = default;
    /// @brief Move assignment — transfers ownership of the underlying state.
    /// @return `*this`.
    Completion& operator=(Completion&&) noexcept = default;
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;

    /// @brief Registers a success callback.
    ///
    /// @p handler is posted to the executor with the result value when the
    /// operation completes successfully. If the operation has already completed,
    /// the callback is posted immediately.
    ///
    /// @param handler Callable receiving the result by value.
    /// @return `*this` for chaining.
    Completion& then(std::function<void(T)> handler) {
        if (_state != nullptr) {
            _state->attachThen(std::move(handler));
        }
        return *this;
    }

    /// @brief Registers an error callback.
    ///
    /// @p handler is posted to the executor with the `std::exception_ptr` when
    /// the operation fails. If the operation has already failed, the callback
    /// is posted immediately. Attaching this handler suppresses orphan logging.
    ///
    /// @param handler Callable receiving the exception pointer.
    /// @return `*this` for chaining.
    Completion& onError(std::function<void(std::exception_ptr)> handler) {
        if (_state != nullptr) {
            _state->attachOnError(std::move(handler));
        }
        return *this;
    }

    /// @brief Returns the underlying shared state (for advanced / internal use).
    /// @return Shared pointer to the completion state, or `nullptr` for empty completions.
    [[nodiscard]] std::shared_ptr<detail::CompletionState<T>> state() const { return _state; }

private:
    std::shared_ptr<detail::CompletionState<T>> _state;
};

}  // namespace morph::async
