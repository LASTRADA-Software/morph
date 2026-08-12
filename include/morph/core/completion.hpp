// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
    // Every handler attached while the state is not yet ready is kept (not
    // overwritten): a second/third attach composes with earlier ones instead of
    // silently discarding them. Dispatch invokes all of them, in attachment
    // order, from a single posted closure. See docs/spec/core/completion.md,
    // "Failure modes" / fan-out.
    std::vector<std::function<void(T)>> onOk;
    std::vector<std::function<void(std::exception_ptr)>> onErr;
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
            if (!onOk.empty()) {
                auto savedFns = std::move(onOk);
                auto savedVal = std::move(*value);
                callback = [savedFns = std::move(savedFns), savedVal = std::move(savedVal)]() mutable {
                    // Every handler but the last sees a copy (the value is only
                    // moved into the final invocation), so an earlier handler
                    // cannot leave the value moved-from for a later one. Each
                    // handler is isolated in its own try/catch so one throwing
                    // handler cannot prevent its siblings from running --
                    // fan-out means every attached handler gets its turn,
                    // independent of whether an earlier one misbehaves. An
                    // escaping exception here would otherwise unwind the whole
                    // posted closure and silently skip every handler after the
                    // one that threw.
                    for (std::size_t i = 0; i + 1 < savedFns.size(); ++i) {
                        try {
                            savedFns[i](savedVal);
                        } catch (...) {
                            ::morph::log::logError("[completion] then handler threw; continuing with next handler");
                        }
                    }
                    try {
                        savedFns.back()(std::move(savedVal));
                    } catch (...) {
                        ::morph::log::logError("[completion] then handler threw; continuing with next handler");
                    }
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
            if (!onErr.empty()) {
                auto savedFns = std::move(onErr);
                auto savedErr = error;
                callback = [savedFns = std::move(savedFns), savedErr]() mutable {
                    // Isolate each handler so one throwing onError handler
                    // cannot suppress its siblings -- see the matching comment
                    // in setValue's callback above.
                    for (auto& fn : savedFns) {
                        try {
                            fn(savedErr);
                        } catch (...) {
                            ::morph::log::logError(
                                "[completion] onError handler threw; continuing with next handler");
                        }
                    }
                };
                // Only mark the error handled (suppressing the destructor's orphan
                // log) if we actually have an executor to deliver on. With a null
                // executor the callback below is never posted, so the error must
                // still reach the orphan logger rather than vanish silently.
                onErrAttached = (cbExec != nullptr);
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
                onOk.push_back(std::move(handler));
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
            // See setException: only suppress orphan logging when an executor
            // exists to actually deliver the handler; a null executor otherwise
            // drops the error and silences the orphan logger both at once.
            onErrAttached = (cbExec != nullptr);
            if (ready && error) {
                auto savedErr = error;
                fireNow = [handler = std::move(handler), savedErr]() mutable { handler(savedErr); };
            } else if (!ready) {
                onErr.push_back(std::move(handler));
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
    /// @param execPtr  Executor on which callbacks are posted. If `nullptr`, callbacks are
    ///                 never delivered — they are silently dropped (see `setValue`/`setException`,
    ///                 which post only when `cbExec != nullptr`).
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
