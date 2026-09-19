// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "callback_scope.hpp"
#include "executor.hpp"
#include "logger.hpp"

namespace morph::async {

namespace detail {

// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
//
// `enable_shared_from_this` is load-bearing, not decoration: the dispatch
// closures below capture a `shared_ptr` to this state and read the settled
// value *in place* instead of carrying a copy of it. That is what makes the
// copy budget independent of how many handlers are attached, and what lets `T`
// be move-only -- the closure stores a refcounted handle, so it stays
// copy-constructible and `IExecutor::post`'s `std::function<void()>` is
// untouched. Every `CompletionState<T>` in the tree is created by
// `std::make_shared` (`Completion<T>::makeSettleable`, `Bridge`, the backends),
// which is the precondition `shared_from_this()` needs.
//
// Reading `value` outside `mtx` from those closures is safe because `value` is
// write-once: both `setValue` and `setException` return early when `ready`, and
// nothing else ever assigns it. The store happens under the lock before the
// closure is handed to the executor, and the executor's own queue provides the
// happens-before edge to the thread that runs it.
template <typename T>
struct CompletionState : std::enable_shared_from_this<CompletionState<T>> {
    static_assert(std::move_constructible<T>,
                  "morph::async::Completion<T> requires only that T be move-constructible. Copyability is a "
                  "per-handler obligation: a handler taking `const T&` imposes nothing, a handler taking `T` by "
                  "value requires T to be copyable and is diagnosed where that handler is written.");

    std::mutex mtx;
    std::optional<T> value;
    std::exception_ptr error;
    bool ready = false;
    // Every handler attached while the state is not yet ready is kept (not
    // overwritten): a second/third attach composes with earlier ones instead of
    // silently discarding them. Dispatch invokes all of them, in attachment
    // order, from a single posted closure. See docs/spec/core/completion.md,
    // "Failure modes" / fan-out.
    //
    // Erased as `void(const T&)`, not `void(T)`. One erased type accepts every
    // handler spelling a caller already writes -- `[](const T&)`, `[](T)`, a
    // `std::function<void(T)>` object -- because each is invocable with
    // `const T&`. A handler that wants its own value gets exactly one copy, at
    // its own parameter binding, where the reader of that call site can see it;
    // a handler that only observes pays nothing. Erasing as `void(T)` charged
    // every handler a copy whether or not it wanted one (morph#553).
    std::vector<std::function<void(const T&)>> onOk;
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
            // Store first, drain `onOk` last. `value` is this state's own
            // store and is never moved out of: a `then()` attached *after*
            // this point (attachThen's `ready && value` branch) reads it
            // again, and moving out of it left it engaged but moved-from so a
            // later attacher silently observed a husk (morph#520). The value
            // is observed, never consumed -- structurally, now that no
            // dispatch path can take it.
            //
            // The ordering is what gives this block the strong exception
            // guarantee for a `T` whose *move* constructor can throw: nothing
            // has changed when the store below runs, and `optional::emplace`
            // leaves the optional disengaged if the construction throws, so an
            // escape leaves `onOk` holding every handler and the state unready
            // rather than a state that already looks settled with its handlers
            // already lost. Draining first (as an earlier revision did) meant
            // a throwing store unwound with `savedFns` -- a local -- carrying
            // every handler to its destructor: permanently unsettled, no
            // handlers, and silent, because the destructor's orphan logger
            // only fires when `error` is set. `std::move` on a vector is
            // noexcept, so the ordering costs nothing.
            //
            // `emplace`, not `value = std::move(val)`: assigning through
            // `std::optional` requires `T` to be move-*assignable* as well as
            // move-constructible, which would quietly make the `static_assert`
            // above a lie for a `T` with a deleted assignment operator.
            // `value` is guaranteed disengaged here -- it is written only on
            // this line, and the `ready` guard above makes this line run once.
            value.emplace(std::move(val));
            ready = true;
            if (!onOk.empty()) {
                auto savedFns = std::move(onOk);
                callback = [self = this->shared_from_this(), savedFns = std::move(savedFns)]() {
                    // Every handler reads the one stored value in place; none
                    // can move out of it, so no handler can leave a husk for
                    // its siblings or for a later attacher, and the count of
                    // handlers costs nothing in copies. Each handler is
                    // isolated in its own try/catch so one throwing handler
                    // cannot prevent its siblings from running -- fan-out means
                    // every attached handler gets its turn, independent of
                    // whether an earlier one misbehaves. An escaping exception
                    // here would otherwise unwind the whole posted closure and
                    // silently skip every handler after the one that threw.
                    for (const auto& fn : savedFns) {
                        try {
                            fn(*self->value);
                        } catch (...) {
                            ::morph::log::logError("[completion] then handler threw; continuing with next handler");
                        }
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
            // A null `exc` must never reach `error`. Storing one would set
            // `ready` with `error` still falsy — a state no attach can act on,
            // because `attachOnError` tests `ready && error` and `attachThen`
            // tests `ready && value`, so both branches fall through and the
            // handler is neither fired nor queued. The completion would then
            // be dead in both directions for every later caller, and silently:
            // `attachOnError` sets `onErrAttached` on entry, so even the
            // destructor's orphan logger is suppressed. It would also hand any
            // *already*-attached handler a null `exception_ptr`, which is UB to
            // `std::rethrow_exception` — the idiomatic handler body, this
            // file's own orphan logger included.
            //
            // Substituting here rather than at any one producer is deliberate:
            // `reject(nullptr)` is reachable through the public `Promise<T>`
            // seam, and around ten sites in the tree forward an
            // `exception_ptr` straight through (`.onError([state](auto e) {
            // state->setException(e); })`) without inspecting it, so a guard
            // at one producer would leave every other one able to reintroduce
            // the same wedge. See issue #347.
            error = exc ? exc
                        : std::make_exception_ptr(
                              std::runtime_error{"completion rejected with no exception (null exception_ptr)"});
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
                            ::morph::log::logError("[completion] onError handler threw; continuing with next handler");
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
    void attachThen(std::function<void(const T&)> handler) {
        std::function<void()> fireNow;
        {
            std::scoped_lock const lock{mtx};
            if (ready && value) {
                // Keep the state alive and read `value` in place rather than
                // snapshotting it. The old shape copied `*value` into
                // `savedVal` and then captured `savedVal` *by copy* before
                // moving it into the handler -- two copies where the handler
                // asked for at most one, and the reason a late attacher cost
                // 2 copies rather than 1 (morph#553).
                fireNow = [self = this->shared_from_this(), handler = std::move(handler)]() { handler(*self->value); };
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
/// @par Value-handling contract
/// `T` need only be **move-constructible**; that is the whole type requirement.
/// Copyability is a *per-handler* obligation, reported where the handler is
/// written rather than imposed on the whole instantiation:
/// - a handler taking `const T&` costs **zero** copies;
/// - a handler taking `T` by value costs **exactly one**, at its own parameter
///   binding, and requires `T` to be copyable.
///
/// The budget does not depend on how many handlers are attached, nor on whether
/// they attached before or after the completion settled. The value is
/// **observed, never consumed**: no handler can move out of the stored value, so
/// a `then()` attached after settling still sees the genuine result. A handler
/// that wants to consume takes `T` by value and moves out of its own copy.
/// See docs/spec/core/completion.md, "Value-handling contract".
///
/// @tparam T Type of the success value. Must be move-constructible.
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
    /// @param handler Callable receiving the result. Take `const T&` to observe it for free;
    ///                take `T` by value to get your own copy, at the cost of exactly one copy.
    /// @return `*this` for chaining — a reference into this `Completion`, valid
    ///         only for as long as it is.
    Completion& then(std::function<void(const T&)> handler) MORPH_LIFETIMEBOUND {
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
    /// @return `*this` for chaining — a reference into this `Completion`, valid
    ///         only for as long as it is.
    Completion& onError(std::function<void(std::exception_ptr)> handler) MORPH_LIFETIMEBOUND {
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
    /// @param handler Callable receiving the result. Take `const T&` to observe it for free;
    ///                take `T` by value to get your own copy, at the cost of exactly one copy.
    /// @return `*this` for chaining — a reference into this `Completion`, valid
    ///         only for as long as it is.
    Completion& then(const CallbackScope& scope, std::function<void(const T&)> handler) MORPH_LIFETIMEBOUND {
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
    /// @param handler Callable receiving the result. Take `const T&` to observe it for free;
    ///                take `T` by value to get your own copy, at the cost of exactly one copy.
    /// @return `*this` for chaining — a reference into this `Completion`, valid
    ///         only for as long as it is.
    Completion& then(CallbackToken token, std::function<void(const T&)> handler) MORPH_LIFETIMEBOUND {
        return then(std::function<void(const T&)>{token.guard(std::move(handler))});
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
    /// @return `*this` for chaining — a reference into this `Completion`, valid
    ///         only for as long as it is.
    Completion& onError(const CallbackScope& scope,
                        std::function<void(std::exception_ptr)> handler) MORPH_LIFETIMEBOUND {
        return onError(scope.token(), std::move(handler));
    }

    /// @brief Registers an error callback gated on an already-issued @p token.
    ///
    /// The token-taking form of `onError(const CallbackScope&, handler)`.
    ///
    /// @param token   Gate observing some receiver's `CallbackScope`. A
    ///                default-constructed token suppresses unconditionally.
    /// @param handler Callable receiving the exception pointer.
    /// @return `*this` for chaining — a reference into this `Completion`, valid
    ///         only for as long as it is.
    Completion& onError(CallbackToken token, std::function<void(std::exception_ptr)> handler) MORPH_LIFETIMEBOUND {
        return onError(std::function<void(std::exception_ptr)>{token.guard(std::move(handler))});
    }

    /// @brief Registers a success callback whose lifetime is deliberately unmanaged.
    ///
    /// Exactly `then(handler)`, spelled so that "no scope gates this" is a
    /// statement the author made on purpose and a reviewer can grep for. Use it
    /// when the handler genuinely owns everything it touches — it captures only
    /// values, or a `shared_ptr` it keeps alive itself.
    ///
    /// @param handler Callable receiving the result. Take `const T&` to observe it for free;
    ///                take `T` by value to get your own copy, at the cost of exactly one copy.
    /// @return `*this` for chaining — a reference into this `Completion`, valid
    ///         only for as long as it is.
    Completion& thenDetached(std::function<void(const T&)> handler) MORPH_LIFETIMEBOUND {
        return then(std::move(handler));
    }

    /// @brief Registers an error callback whose lifetime is deliberately unmanaged.
    ///
    /// The `onError` counterpart of `thenDetached()`.
    ///
    /// @param handler Callable receiving the exception pointer.
    /// @return `*this` for chaining — a reference into this `Completion`, valid
    ///         only for as long as it is.
    Completion& onErrorDetached(std::function<void(std::exception_ptr)> handler) MORPH_LIFETIMEBOUND {
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
