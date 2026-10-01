// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <concepts>
#include <core/async/ExecutorContext.hpp>
#include <core/async/StopToken.hpp>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "callback_scope.hpp"
#include "detail/completion_awaiter.hpp"
#include "detail/owner_probe.hpp"
#include "executor.hpp"
#include "logger.hpp"

namespace morph::async {

namespace detail {

// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
//
// One state per operation, shared by the producer that settles it and the
// consumer that attaches to it, with no lock between them:
//
// - **Settling** is claimed once with an atomic exchange (`settled`), so a late
//   settle -- a reply after `cancelPending` -- returns before it allocates
//   anything. The claimant stores `value` or `error` and publishes it with a
//   release store of `ready`; both are write-once, so any thread that sees
//   `ready` may read them. It then posts `deliver()` to `cbExec`.
// - **Everything else is the owner's**: `onOk`, `onErr`, `delivered`,
//   `stopLinks` are touched only on `cbExec` -- by `deliver()`, and by the
//   consumer's attaches, which run there (`onOwner`). The executor's queue is
//   the happens-before edge between the settle and the handlers.
//
// `enable_shared_from_this` is load-bearing: `deliver()` and a late attach's
// fire-now closure capture a `shared_ptr` to this state and read the settled
// value *in place*. That is what makes the copy budget independent of how many
// handlers are attached, what lets `T` be move-only, and what keeps the state
// -- and so its orphan logger -- alive until delivery has run. Every
// `CompletionState<T>` in the tree is created by `std::make_shared`.
template <typename T>
struct CompletionState : std::enable_shared_from_this<CompletionState<T>> {
    static_assert(std::move_constructible<T>,
                  "morph::async::Completion<T> requires only that T be move-constructible. Copyability is a "
                  "per-handler obligation: a handler taking `const T&` imposes nothing, a handler taking `T` by "
                  "value requires T to be copyable and is diagnosed where that handler is written.");

    std::optional<T> value;
    std::exception_ptr error;
    // Claimed by the first settle; every later one returns at once.
    std::atomic<bool> settled{false};
    // Published by the settle that claimed `settled`, after `value` or `error`
    // is stored: `true` means exactly one of them is engaged and final.
    std::atomic<bool> ready{false};
    // Every handler attached while the state is not yet delivered is kept (not
    // overwritten): a second/third attach composes with earlier ones instead of
    // silently discarding them. `deliver()` invokes all of them, in attachment
    // order. See docs/spec/core/completion.md, "Failure modes" / fan-out.
    //
    // Erased as `void(const T&)`, not `void(T)`. One erased type accepts every
    // handler spelling a caller already writes -- `[](const T&)`, `[](T)`, a
    // `std::function<void(T)>` object -- because each is invocable with
    // `const T&`. A handler that wants its own value gets exactly one copy, at
    // its own parameter binding, where the reader of that call site can see it;
    // a handler that only observes pays nothing. Erasing as `void(T)` would
    // charge every handler a copy whether or not it wanted one.
    std::vector<std::function<void(const T&)>> onOk;
    std::vector<std::function<void(std::exception_ptr)>> onErr;
    // Whether the error is someone's to handle, which silences the orphan
    // logger. Atomic because `bridge::detail::takeSettled` takes an outcome
    // without attaching, on whichever thread holds the completion.
    std::atomic<bool> onErrAttached{false};
    // Set by `deliver()`, on the owner. A handler attached after it is posted on
    // its own; one attached before is in the lists `deliver()` drains.
    std::atomic<bool> delivered{false};
    // The thread an attach outside every executor's task presumed to be the
    // owner's own, and the thread `deliver()` ran on: compared, in both
    // orders, so that presumption is checked rather than trusted.
    std::atomic<std::thread::id> presumedOwnerThread{};
    std::atomic<std::thread::id> deliveredOn{};
    ::morph::exec::IExecutor* cbExec = nullptr;
    // The stop source of the call this state reports on, or null for a call
    // that cannot be stopped. Set by the producer before the state is handed
    // out, and read only on the owner.
    std::shared_ptr<::core::async::StopSource> stopSource;
    // One `StopCallback` per `CallbackScope` a gated handler was attached
    // through, relaying that scope's stop to `stopSource`. Released when the
    // state is delivered: a settled call has nothing left to stop.
    std::vector<std::shared_ptr<void>> stopLinks;

    // Relays a scope's stop request to the call's own stop source. Holds the
    // source weakly, so a link outliving its call keeps nothing alive.
    struct StopRelay {
        std::weak_ptr<::core::async::StopSource> source;
        void operator()() const noexcept {
            if (auto const strong = source.lock()) {
                static_cast<void>(strong->request_stop());
            }
        }
    };

    /// Whether the calling thread may touch the owner's half of this state,
    /// reporting it (assert, or the test probe) when it may not.
    ///
    /// The owner is `cbExec`. Inside one of its tasks is the owner. Outside
    /// every executor's task is presumed to be the owner's own thread running
    /// code outside its tasks -- a Qt slot, a test body -- and recorded, so
    /// `deliver()` can check the presumption against where it actually runs.
    /// `inlineExecutor()` delivers wherever the settle happens: its consumer
    /// attaches before handing the state to the producer, or after it settled,
    /// and nothing here can tell those apart from a race.
    void checkOwner(char const* site) noexcept {
        if (::morph::exec::runningOn(*cbExec) || cbExec == &::morph::exec::detail::inlineExecutor()) {
            return;
        }
        if (::core::async::ExecutorScope::innermost() != nullptr) {
            ::morph::exec::detail::noteOwner(site, cbExec->coreExecutor(), false);
            return;
        }
        auto const self = std::this_thread::get_id();
        presumedOwnerThread.store(self, std::memory_order_relaxed);
        if (delivered.load(std::memory_order_acquire) && deliveredOn.load(std::memory_order_relaxed) != self) {
            ::morph::exec::detail::noteOwner(site, cbExec->coreExecutor(), false);
        }
    }

    // Links the stop of the scope @p scopeStop belongs to with this call's own
    // stop source, until the state is delivered. A no-op for a call with no
    // stop source, a scope that can no longer be stopped, a state already
    // delivered, or one with no executor to deliver on. A scope already stopped
    // stops the call here. Runs on the owner.
    void linkStop(::core::async::StopToken scopeStop) {
        if (cbExec == nullptr || stopSource == nullptr || delivered.load(std::memory_order_relaxed) ||
            !scopeStop.stop_possible()) {
            return;
        }
        checkOwner("Completion::linkStop");
        stopLinks.push_back(std::make_shared<::core::async::StopCallback<StopRelay>>(
            std::move(scopeStop), StopRelay{std::weak_ptr<::core::async::StopSource>{stopSource}}));
    }

    void setValue(T val) {
        if (settled.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        // `emplace`, not `value = std::move(val)`: assigning through
        // `std::optional` requires `T` to be move-*assignable* as well as
        // move-constructible, which would quietly make the `static_assert`
        // above a lie for a `T` with a deleted assignment operator. A store
        // whose move throws leaves `value` disengaged and releases the claim,
        // so the state is still unsettled with every handler kept.
        try {
            value.emplace(std::move(val));
        } catch (...) {
            settled.store(false, std::memory_order_release);
            throw;
        }
        publish();
    }

    void setException(const std::exception_ptr& exc) {
        if (settled.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        // A null `exc` must never reach `error`. Storing one would publish
        // `ready` with `error` still falsy -- a state no attach can act on,
        // because `deliver()` and a late `attachOnError` both test `error`, and
        // `attachThen` tests `value`, so a handler is neither fired nor kept.
        // It would also hand an attached handler a null `exception_ptr`, which
        // is UB to `std::rethrow_exception` -- the idiomatic handler body, this
        // file's own orphan logger included.
        //
        // Substituting here rather than at any one producer is deliberate:
        // `reject(nullptr)` is reachable through the public `Promise<T>` seam,
        // and many sites forward an `exception_ptr` straight through
        // (`.onError([state](auto e) { state->setException(e); })`) without
        // inspecting it.
        error = exc ? exc
                    : std::make_exception_ptr(
                          std::runtime_error{"completion rejected with no exception (null exception_ptr)"});
        publish();
    }

    void attachThen(std::function<void(const T&)> handler) {
        if (cbExec == nullptr) {
            return;  // Nothing could ever deliver it.
        }
        checkOwner("Completion::attach");
        if (!delivered.load(std::memory_order_relaxed)) {
            onOk.push_back(std::move(handler));
            return;
        }
        if (!value) {
            return;  // Settled with an error: a `then` has nothing to observe.
        }
        // Posted, not run here: a handler attached later must not overtake
        // one attached earlier, and a handler never runs inside the call that
        // attached it. The closure reads `value` in place.
        cbExec->post([self = this->shared_from_this(), handler = std::move(handler)]() {
            try {
                handler(*self->value);
            } catch (...) {
                ::morph::log::logError("[completion] then handler threw; continuing with next handler");
            }
        });
    }

    void attachOnError(std::function<void(std::exception_ptr)> handler) {
        if (cbExec == nullptr) {
            // Nothing could ever deliver it, so the error stays the orphan
            // logger's rather than being dropped and silenced at once.
            return;
        }
        checkOwner("Completion::attach");
        onErrAttached.store(true, std::memory_order_relaxed);
        if (!delivered.load(std::memory_order_relaxed)) {
            onErr.push_back(std::move(handler));
            return;
        }
        if (!error) {
            return;  // Settled with a value.
        }
        cbExec->post([handler = std::move(handler), savedErr = error]() mutable {
            try {
                handler(savedErr);
            } catch (...) {
                ::morph::log::logError("[completion] onError handler threw; continuing with next handler");
            }
        });
    }

    ~CompletionState() {
        if (!ready.load(std::memory_order_acquire) || !error || onErrAttached.load(std::memory_order_relaxed)) {
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

private:
    /// Publishes the stored outcome and hands its delivery to the owner.
    void publish() {
        ready.store(true, std::memory_order_release);
        if (cbExec != nullptr) {
            cbExec->post([self = this->shared_from_this()] { self->deliver(); });
        }
    }

    /// Runs on `cbExec`: every handler attached so far, in attachment order,
    /// each isolated so one that throws cannot cost its siblings their turn.
    void deliver() {
        auto const here = std::this_thread::get_id();
        deliveredOn.store(here, std::memory_order_relaxed);
        delivered.store(true, std::memory_order_release);
        // An attach outside every task presumed this thread was the owner's;
        // delivery running elsewhere refutes it -- that attach raced this.
        if (auto const presumed = presumedOwnerThread.load(std::memory_order_relaxed);
            presumed != std::thread::id{} && presumed != here) {
            ::morph::exec::detail::noteOwner("Completion::deliver", cbExec->coreExecutor(), false);
        }
        // Released here, on the owner, where they were added.
        auto const links = std::move(stopLinks);
        if (value) {
            auto const handlers = std::move(onOk);
            for (const auto& handler : handlers) {
                try {
                    handler(*value);
                } catch (...) {
                    ::morph::log::logError("[completion] then handler threw; continuing with next handler");
                }
            }
            return;
        }
        auto const handlers = std::move(onErr);
        for (const auto& handler : handlers) {
            try {
                handler(error);
            } catch (...) {
                ::morph::log::logError("[completion] onError handler threw; continuing with next handler");
            }
        }
    }
};

// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief A place a backend settles one dispatch, without naming its type.
///
/// The narrow half of `CompletionState<std::shared_ptr<void>>`: the two calls a
/// backend actually makes on the completion it produced. Splitting them out is
/// what lets `Bridge` hand the backend a sink that *is* the caller's typed
/// completion state, instead of a second, erased completion whose only job is
/// to be forwarded into the first.
///
/// Such forwarding costs six heap allocations per dispatch — the erased state,
/// the `.then` and `.onError` closures, their two handler vectors, and one of
/// the two posted settle tasks — against the 14.06 a local round trip takes.
/// Through a sink it is one.
///
/// @par Contract for implementers
/// - **Settle once.** `settleValue` and `settleException` are mutually
///   exclusive and each may be called at most once *in effect*; an
///   implementation must tolerate extra calls and ignore them, because
///   `IBackend::cancelPending` settles a sink that a reply may be racing to
///   settle at the same moment. `CompletionState` is already first-result-wins,
///   but anything an implementation does *besides* forwarding — decrementing a
///   counter, cancelling a timer — needs its own latch.
/// - **Do not let an exception escape** where one can be avoided. A sink is
///   settled from a strand task or a transport thread whose only handler is the
///   one the backend wrote around the call. The methods are deliberately *not*
///   `noexcept`: `CompletionState::setValue` is not either (it builds a
///   `std::function` for the posted callback), and promising more here would
///   turn a `bad_alloc` into a `std::terminate` that today it is not.
class ISettleSink {
public:
    ISettleSink() = default;
    ISettleSink(const ISettleSink&) = delete;
    ISettleSink(ISettleSink&&) = delete;
    ISettleSink& operator=(const ISettleSink&) = delete;
    ISettleSink& operator=(ISettleSink&&) = delete;
    virtual ~ISettleSink() = default;

    /// @brief Settles the dispatch with its opaque result.
    /// @param value The result, as the backend produced it.
    virtual void settleValue(std::shared_ptr<void> value) = 0;

    /// @brief Settles the dispatch with a failure.
    /// @param exc The exception to deliver.
    virtual void settleException(const std::exception_ptr& exc) = 0;
};

/// @brief The `ISettleSink` that simply is a `CompletionState<std::shared_ptr<void>>`.
///
/// The adapter for every caller that wants the old shape: `IBackend::execute`
/// returns a `Completion<std::shared_ptr<void>>`, so a backend implementing
/// `executeInto` can serve `execute` by wrapping its own state in one of these.
/// Nothing but forwarding happens here, so it needs no settle-once latch of its
/// own — `CompletionState` already has one.
class CompletionSettleSink final : public ISettleSink {
public:
    /// @brief Binds the sink to @p state.
    /// @param state The completion state to settle. Must not be null.
    explicit CompletionSettleSink(std::shared_ptr<CompletionState<std::shared_ptr<void>>> state)
        : _state{std::move(state)} {}

    /// @brief Forwards to `CompletionState::setValue`.
    /// @param value The result to store.
    void settleValue(std::shared_ptr<void> value) override { _state->setValue(std::move(value)); }

    /// @brief Forwards to `CompletionState::setException`.
    /// @param exc The exception to store.
    void settleException(const std::exception_ptr& exc) override { _state->setException(exc); }

private:
    std::shared_ptr<CompletionState<std::shared_ptr<void>>> _state;
};

}  // namespace detail

/// @brief Move-only handle representing the eventual result of an asynchronous operation.
///
/// Callbacks are posted to the `IExecutor` supplied at construction time, so
/// they always run on the intended thread (e.g. the GUI thread).
///
/// @par Thread safety
/// A completion belongs to the executor supplied at construction, its owner.
/// It may be settled from any thread: the first settle wins, and its delivery
/// is posted to the owner, where every registered callback runs. `then()` and
/// `onError()` must be called on the owner -- inside one of its tasks, or on
/// its own thread outside them (a Qt slot, a test body). Calling them from
/// another executor's task is a contract violation, asserted in a debug
/// build. `co_await` may be used from any coroutine: it posts its attach to
/// the owner when it is not running there.
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
    /// @param execPtr  The completion's owner: settling posts the delivery there, and
    ///                 callbacks are attached and run there. If `nullptr`, callbacks are
    ///                 never delivered — attaching one is a no-op.
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
    /// @p handler runs on the executor with the result value when the
    /// operation completes successfully. If the result has already been
    /// delivered, the callback is posted on its own. Call on the owner (see
    /// the class's thread-safety note).
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
    /// @p handler runs on the executor with the `std::exception_ptr` when
    /// the operation fails. If the failure has already been delivered, the
    /// callback is posted on its own. Attaching this handler suppresses orphan
    /// logging. Call on the owner (see the class's thread-safety note).
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
    /// Attaching also makes the call this completion reports on one the scope
    /// owns: when the call carries a stop source (a `Bridge` call whose handler
    /// returns a `Task`), stopping, resetting or destroying the scope before it
    /// settles requests stop on it.
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
        if (_state != nullptr) {
            _state->linkStop(token.stopToken());
        }
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
        if (_state != nullptr) {
            _state->linkStop(token.stopToken());
        }
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

    /// @brief Awaits this completion from a coroutine, consuming it.
    ///
    /// `co_await std::move(completion)` yields a copy of the settled value or
    /// rethrows the stored exception. The coroutine resumes in the resumption
    /// context it suspended in -- a `spawn`ed task's executor, a Task handler's
    /// strand -- or, with none, on this completion's executor, where `then()`
    /// handlers run. A stop requested on the awaiting coroutine's token
    /// withdraws the await and resumes it with `core::async::OperationCancelled`.
    /// Rvalue only: awaiting moves the state out, so an lvalue `co_await` would
    /// hide that the completion is empty afterwards. See
    /// `docs/spec/core/coroutines.md`.
    /// @return The awaiter; not for direct use.
    [[nodiscard]] detail::CompletionAwaiter<T> operator co_await() && {
        static_assert(std::copy_constructible<T>,
                      "co_await on a morph::async::Completion<T> copies the settled value out of the shared state, "
                      "which other handlers may still read, so T must be copy-constructible.");
        return detail::CompletionAwaiter<T>{std::move(_state)};
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
        /// Safe to call from any thread; the delivery is posted to the
        /// completion's executor.
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
        /// Safe to call from any thread; the delivery is posted to the
        /// completion's executor.
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
    /// The public "settleable promise" seam: lets a caller — typically
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
