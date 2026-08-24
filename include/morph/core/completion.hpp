// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "callback_scope.hpp"
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
        // No local guard around the logging calls: `morph::log`'s helpers are
        // `noexcept` (docs/spec/core/logger.md, "Failure modes"), which is
        // what makes them safe to call from a destructor at all. The variadic
        // overload is deliberate — it formats *inside* that guarantee, where
        // building the message by concatenation out here would allocate
        // outside it and could still escape.
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exc) {
            ::morph::log::logError("[orphan] unhandled exception: {}", exc.what());
        } catch (...) {
            ::morph::log::logError("[orphan] unhandled unknown exception");
        }
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
/// @par Lifetime and stop gating
/// Because delivery always goes through an executor, the receiver can be
/// destroyed — or simply lose interest — before the handler runs. Pass a
/// `morph::async::CallbackScope` (or one of its `CallbackToken`s) as the first
/// argument to `then()` / `onError()` and the handler is refused unless the
/// scope is still alive and un-stopped at delivery time. `thenDetached()` /
/// `onErrorDetached()` are the same ungated attachments as `then(fn)` /
/// `onError(fn)`, spelled so a deliberately unmanaged callback says so.
/// See `callback_scope.hpp` and docs/spec/core/callback_scope.md.
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

    /// @brief Registers a success callback gated on @p scope's lifetime and stop state.
    ///
    /// Identical to `then(handler)` except that @p handler is wrapped in a
    /// `CallbackToken` gate at attach time: when the result is delivered, the
    /// handler runs only if @p scope is still alive and has not been stopped or
    /// `reset()`. Nothing else changes — the gate lives inside the stored
    /// handler, so fan-out, the attach-after-ready fire-now path and executor
    /// marshalling all behave exactly as they do for the ungated form.
    ///
    /// The scope is observed weakly; attaching does **not** extend its lifetime.
    ///
    /// @param scope   Receiver-owned gate. Only a token for its *current*
    ///                generation is captured, so a later `reset()` retires this
    ///                attachment.
    /// @param handler Callable receiving the result by value.
    /// @return `*this` for chaining.
    Completion& then(const CallbackScope& scope, std::function<void(T)> handler) {
        return then(scope.token(), std::move(handler));
    }

    /// @brief Registers a success callback gated on an already-issued @p token.
    ///
    /// The token-taking form of `then(const CallbackScope&, handler)`, for
    /// callers that hold a token rather than the scope itself (a helper that
    /// was handed one, or code that captured a token before dispatching).
    ///
    /// @param token   Gate observing some receiver's `CallbackScope`. A
    ///                default-constructed token suppresses unconditionally.
    /// @param handler Callable receiving the result by value.
    /// @return `*this` for chaining.
    Completion& then(CallbackToken token, std::function<void(T)> handler) {
        return then(std::function<void(T)>{token.guard(std::move(handler))});
    }

    /// @brief Registers an error callback gated on @p scope's lifetime and stop state.
    ///
    /// Identical to `onError(handler)` except that @p handler is wrapped in a
    /// `CallbackToken` gate at attach time.
    ///
    /// Orphan logging is unaffected: attaching a gated handler suppresses it
    /// exactly as the ungated form does, and an error whose delivery the scope
    /// then refuses still counts as **handled**. Suppression is a deliberate act
    /// by the receiver, not a dropped error nobody asked about.
    ///
    /// @param scope   Receiver-owned gate; observed weakly.
    /// @param handler Callable receiving the exception pointer.
    /// @return `*this` for chaining.
    Completion& onError(const CallbackScope& scope, std::function<void(std::exception_ptr)> handler) {
        return onError(scope.token(), std::move(handler));
    }

    /// @brief Registers an error callback gated on an already-issued @p token.
    ///
    /// The token-taking form of `onError(const CallbackScope&, handler)`.
    ///
    /// @param token   Gate observing some receiver's `CallbackScope`. A
    ///                default-constructed token suppresses unconditionally.
    /// @param handler Callable receiving the exception pointer.
    /// @return `*this` for chaining.
    Completion& onError(CallbackToken token, std::function<void(std::exception_ptr)> handler) {
        return onError(std::function<void(std::exception_ptr)>{token.guard(std::move(handler))});
    }

    /// @brief Registers a success callback whose lifetime is deliberately unmanaged.
    ///
    /// Exactly `then(handler)`, spelled so that "no scope gates this" is a
    /// statement the author made on purpose and a reviewer can grep for. Use it
    /// when the handler genuinely owns everything it touches — it captures only
    /// values, or a `shared_ptr` it keeps alive itself.
    ///
    /// @param handler Callable receiving the result by value.
    /// @return `*this` for chaining.
    Completion& thenDetached(std::function<void(T)> handler) { return then(std::move(handler)); }

    /// @brief Registers an error callback whose lifetime is deliberately unmanaged.
    ///
    /// The `onError` counterpart of `thenDetached()`.
    ///
    /// @param handler Callable receiving the exception pointer.
    /// @return `*this` for chaining.
    Completion& onErrorDetached(std::function<void(std::exception_ptr)> handler) {
        return onError(std::move(handler));
    }

    /// @brief Returns the underlying shared state (for advanced / internal use).
    /// @return Shared pointer to the completion state, or `nullptr` for empty completions.
    [[nodiscard]] std::shared_ptr<detail::CompletionState<T>> state() const { return _state; }

    /// @brief Producer-side handle paired with a `Completion<T>` by `makeSettleable()`.
    ///
    /// `Promise<T>` is the public settling counterpart to `Completion<T>`: it
    /// exposes exactly `resolve()`/`reject()` against the same shared state a
    /// paired `Completion<T>` observes via `then()`/`onError()`, without ever
    /// naming `morph::async::detail::CompletionState<T>`. Intended for test code
    /// that needs to construct a `Completion<T>` it can settle on demand — e.g.
    /// standing in for a `Bridge`/`IBackend` round trip — instead of reaching
    /// into `detail::CompletionState<T>` directly (see docs/spec/core/completion.md,
    /// "Settleable promise seam").
    ///
    /// Move-only, mirroring `Completion<T>`: exactly one producer settles a
    /// given operation.
    class Promise {
    public:
        /// @brief Move constructor — transfers ownership of the shared state.
        Promise(Promise&&) noexcept = default;
        /// @brief Move assignment — transfers ownership of the shared state.
        /// @return `*this`.
        Promise& operator=(Promise&&) noexcept = default;
        Promise(const Promise&) = delete;
        Promise& operator=(const Promise&) = delete;
        ~Promise() = default;

        /// @brief Resolves the paired `Completion<T>` with @p val.
        ///
        /// No-op if the state is already settled (first result wins — see
        /// `detail::CompletionState<T>::setValue`), or if this `Promise` was
        /// moved from (mirrors `Completion<T>::then()`'s null-state no-op).
        /// Safe to call from any thread.
        /// @param val Success value delivered to every attached `then()` handler.
        void resolve(T val) {
            if (_state != nullptr) {
                _state->setValue(std::move(val));
            }
        }

        /// @brief Rejects the paired `Completion<T>` with @p exc.
        ///
        /// No-op if the state is already settled (first result wins — see
        /// `detail::CompletionState<T>::setException`), or if this `Promise` was
        /// moved from (mirrors `Completion<T>::onError()`'s null-state no-op).
        /// Safe to call from any thread.
        /// @param exc Error delivered to every attached `onError()` handler, or
        ///            logged as an orphan if none is ever attached.
        void reject(std::exception_ptr exc) {
            if (_state != nullptr) {
                _state->setException(exc);
            }
        }

    private:
        friend class Completion<T>;
        explicit Promise(std::shared_ptr<detail::CompletionState<T>> state) : _state{std::move(state)} {}

        std::shared_ptr<detail::CompletionState<T>> _state;
    };

    /// @brief Constructs a `Completion<T>`/`Promise<T>` pair sharing one settleable state.
    ///
    /// The public "settleable promise" seam (issue #55): lets a caller — typically
    /// test code — construct a `Completion<T>` it can resolve or reject on demand,
    /// without a full `Bridge`/`IBackend` round trip and without reaching into
    /// `morph::async::detail::CompletionState<T>`. Everything `Completion(state,
    /// executor)` already provided by hand is available through this factory
    /// instead: the returned `Completion<T>` is exactly what `then()`/`onError()`
    /// observe; the returned `Promise` is exactly what settles it.
    /// @param execPtr Executor callbacks are posted on; `nullptr` for a
    ///                 write-only completion (see the two-argument constructor).
    /// @return A `{Completion<T>, Promise}` pair sharing one `CompletionState<T>`.
    [[nodiscard]] static std::pair<Completion<T>, Promise> makeSettleable(::morph::exec::IExecutor* execPtr) {
        auto state = std::make_shared<detail::CompletionState<T>>();
        Completion<T> completion{state, execPtr};
        Promise promise{state};
        return {std::move(completion), std::move(promise)};
    }

private:
    std::shared_ptr<detail::CompletionState<T>> _state;
};

}  // namespace morph::async
