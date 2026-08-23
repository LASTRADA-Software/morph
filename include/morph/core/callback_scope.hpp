// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace morph::async {

namespace detail {

/// Shared state behind one `CallbackScope` generation. Deliberately tiny: the
/// control block is what carries liveness, and the single atomic carries stop.
struct CallbackScopeState {
    std::atomic<bool> stopped{false};
};

}  // namespace detail

/// @brief Three-way answer to "may this callback still be delivered?".
///
/// Liveness and stop are *different* states and are kept distinguishable:
/// "the owner is gone" and "the owner is still here but cancelled this" have
/// the same effect on delivery but very different meanings for diagnostics,
/// logging and tests. `CallbackToken::status()` reports which one applies.
enum class CallbackStatus : std::uint8_t {
    /// The owning `CallbackScope` still exists and has not been stopped —
    /// callbacks gated on this token run.
    Active,
    /// The owning `CallbackScope` still exists but `requestStop()` was called
    /// on this generation — the owner is alive and no longer interested.
    Stopped,
    /// The owning `CallbackScope` was destroyed, or `reset()` retired the
    /// generation this token was issued from.
    Expired,
};

/// @brief Weak, copyable observer of a `CallbackScope` — the half a callback captures.
///
/// A token keeps nothing alive. It answers whether the scope that issued it is
/// still both present and interested, and it can wrap an arbitrary callable so
/// that the callable no-ops once the answer turns negative.
///
/// A default-constructed token is permanently `CallbackStatus::Expired`:
/// gating is fail-closed, so a token that was never bound to a scope suppresses
/// rather than admits. That trades a silently-dropped callback for never
/// dereferencing a freed receiver, which is the trade this whole type exists to
/// make.
///
/// @par Thread safety
/// Every member is safe to call from any thread, concurrently with the
/// scope's `requestStop()` / `reset()` / destruction. What the *result* is worth
/// across threads is a separate question — see `CallbackScope`'s
/// "Boundary of the guarantee".
class CallbackToken {
public:
    /// @brief Constructs an unbound token, permanently `CallbackStatus::Expired`.
    CallbackToken() = default;

    /// @brief Reports whether the issuing scope is alive, stopped, or gone.
    /// @return `Active`, `Stopped` or `Expired` — see `CallbackStatus`.
    [[nodiscard]] CallbackStatus status() const noexcept {
        auto const state = _state.lock();
        if (state == nullptr) {
            return CallbackStatus::Expired;
        }
        return state->stopped.load(std::memory_order_acquire) ? CallbackStatus::Stopped : CallbackStatus::Active;
    }

    /// @brief Whether a callback gated on this token may run right now.
    ///
    /// Advisory across threads: true here means "was active at the moment of
    /// the check". See `CallbackScope`'s "Boundary of the guarantee".
    /// @return `true` iff `status() == CallbackStatus::Active`.
    [[nodiscard]] bool active() const noexcept { return status() == CallbackStatus::Active; }

    /// @brief Whether the issuing scope generation is gone (destroyed or `reset()`).
    ///
    /// Cheaper than `status()` — no strong reference is taken — and unlike
    /// `active()` it does not conflate "gone" with "stopped".
    /// @return `true` iff the issuing generation no longer exists.
    [[nodiscard]] bool expired() const noexcept { return _state.expired(); }

    /// @brief Wraps @p fn so it runs only while this token is `Active`.
    ///
    /// The returned callable forwards every argument to @p fn and returns
    /// nothing. When the token is not `Active` at invocation time it does
    /// nothing at all — @p fn is not called. The wrapper holds a *copy of the
    /// token*, never a strong reference, so wrapping a callback cannot keep the
    /// receiver's scope alive.
    ///
    /// A suppressed callback is destroyed, not leaked: the wrapper (and with it
    /// @p fn and its captures) is released wherever the refused task is
    /// discarded — normally on the delivery executor's thread.
    ///
    /// @tparam F Callable type to wrap. Must return `void` for every argument
    ///           list it is invoked with; a value-returning callable has no
    ///           defensible answer for the suppressed case and is rejected at
    ///           compile time.
    /// @param fn Callable to gate. Moved into the returned wrapper.
    /// @return A callable with @p fn's argument list and a `void` return.
    template <typename F>
    [[nodiscard]] auto guard(F&& fn) const {
        return [token = *this, fn = std::forward<F>(fn)](auto&&... args) mutable -> void {
            static_assert(std::is_void_v<std::invoke_result_t<F&, decltype(args)...>>,
                          "CallbackToken::guard() only wraps void-returning callables: there is no correct value to "
                          "return when delivery is suppressed.");
            if (token.status() != CallbackStatus::Active) {
                return;
            }
            std::invoke(fn, std::forward<decltype(args)>(args)...);
        };
    }

private:
    friend class CallbackScope;

    explicit CallbackToken(std::weak_ptr<detail::CallbackScopeState> state) noexcept : _state{std::move(state)} {}

    std::weak_ptr<detail::CallbackScopeState> _state;
};

/// @brief Owned lifetime-and-stop gate for asynchronous callbacks — a data member, not a base class.
///
/// A receiver that attaches callbacks to a `Completion<T>`, a `BridgeHandler`
/// subscription, a timer tick or any other posted closure holds one of these as
/// an ordinary member and hands out `CallbackToken`s. A gated callback runs only
/// while the scope is **both alive and not stopped**.
///
/// Composition is the point. Requiring every consumer to derive from a framework
/// base class constrains hierarchies that are already `QObject`s, already have a
/// base, or are aggregates — for what is an implementation detail of one screen.
/// A member composes with all of them, and code that never opts in is unaffected.
///
/// @code
/// class BoardPresenter {
///     void load() {
///         _handler.execute(GetBoard{}).then(_callbacks, [this](GetBoardResult r) { render(r); });
///     }
///     void onUserNavigatedAway() { _callbacks.requestStop(); }   // alive, but no longer interested
///     void onNewQuery() { _callbacks.reset(); }                  // supersede: old replies dead, new ones live
///
///     morph::async::CallbackScope _callbacks;  // declared LAST — see below
/// };
/// @endcode
///
/// @par Three verbs, two states
/// - `requestStop()` — "I no longer care". The owner is still here; pending and
///   future callbacks on this generation are refused. Idempotent.
/// - `reset()` — "supersede". Every token issued so far goes permanently dead
///   and the scope becomes deliverable again under a fresh generation, so
///   "a new query cancels the old one" needs no heap-reallocated scope.
/// - destruction — the same refusal as `requestStop()`, plus the owner is gone.
///
/// Destruction and `requestStop()` are indistinguishable *to a callback*; they
/// differ only in whether the owner still exists, which `CallbackToken::status()`
/// reports as `Expired` versus `Stopped`.
///
/// @par Declared last, destroyed first
/// Put the member **after** (below) everything its callbacks touch. Members are
/// destroyed in reverse declaration order, so a last-declared scope is the first
/// thing to die and every gated callback is already refused before the fields it
/// would have touched are torn down. This replaces the per-class "token must
/// stay last-declared" convention that used to be re-derived and re-documented
/// in every class that hand-rolled the pattern.
///
/// @par Teardown that pumps
/// Members are destroyed *after* the destructor body runs. A destructor body
/// that can pump a nested event loop (a `sendSync`-style blocking call) can
/// therefore still deliver into a half-destroyed receiver. Such a destructor
/// must call `requestStop()` as its first statement.
///
/// @par Boundary of the guarantee
/// - **Executor-affine use gets the full guarantee.** When the scope is
///   destroyed / stopped / reset on the *same* thread the delivery executor runs
///   on — the normal case, where receiver and callbacks both live on the GUI
///   thread — check-then-run is atomic with respect to those operations, and a
///   gated callback never touches a dead or stopped receiver.
/// - **Cross-thread stop is advisory.** A scope torn down on a different thread
///   from the delivery executor can turn inactive between the token's check and
///   the callback body. That is exactly the boundary of the hand-rolled
///   `weak_ptr` idiom this type replaces — locking the token pins the *token*,
///   never the receiver — and external synchronisation there remains the
///   caller's job.
/// - **Deliberately no block-until-drained.** Neither `requestStop()` nor the
///   destructor waits for an in-flight callback to finish, `QObject::disconnect`
///   style. A GUI-thread destructor blocking on a pool-thread callback that is
///   itself blocked posting back to the GUI executor is a deadlock by
///   construction.
///
/// @par Thread safety
/// All members are safe to call concurrently from any thread. `requestStop()`
/// and `stopRequested()` are lock-free atomic operations; `reset()` publishes a
/// fresh generation and retires the previous one (stopping it first, so a token
/// holder that raced the swap and pinned the old state still observes refusal).
///
/// Identity, not a value: neither copyable nor movable. A moved-from scope would
/// have to either strand or silently retarget tokens already captured in flight;
/// `reset()` covers the one case (regeneration) that motivates a move at all.
class CallbackScope {
public:
    /// @brief Constructs a live, un-stopped scope.
    CallbackScope() : _state{std::make_shared<detail::CallbackScopeState>()} {}

    /// @brief Refuses every outstanding token, then releases the state.
    ///
    /// Equivalent, from a pending callback's point of view, to `requestStop()`.
    /// Does not wait for in-flight callbacks — see "Boundary of the guarantee".
    ~CallbackScope() { requestStop(); }

    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
    CallbackScope(CallbackScope&&) = delete;
    CallbackScope& operator=(CallbackScope&&) = delete;

    /// @brief Marks this generation stopped: gated callbacks stop being delivered.
    ///
    /// Idempotent and safe from any thread. The owner remains alive, so tokens
    /// report `CallbackStatus::Stopped` rather than `Expired`. Undone only by
    /// `reset()`, which starts a new generation.
    void requestStop() noexcept {
        if (_state != nullptr) {
            _state->stopped.store(true, std::memory_order_release);
        }
    }

    /// @brief Retires every token issued so far and starts a fresh, live generation.
    ///
    /// The supersede verb: an old in-flight reply can no longer be delivered
    /// (its token reports `Expired`), while callbacks attached *after* this call
    /// are deliverable again even if `requestStop()` had been called before it.
    /// The outgoing generation is stopped before it is released, so a token
    /// holder that pinned it while racing this call still observes refusal.
    void reset() {
        auto fresh = std::make_shared<detail::CallbackScopeState>();
        requestStop();
        _state = std::move(fresh);
    }

    /// @brief Whether this generation has been stopped.
    /// @return `true` after `requestStop()`, until the next `reset()`.
    [[nodiscard]] bool stopRequested() const noexcept {
        return _state != nullptr && _state->stopped.load(std::memory_order_acquire);
    }

    /// @brief Issues a weak token for the current generation.
    /// @return A `CallbackToken` observing this scope; keeps nothing alive.
    [[nodiscard]] CallbackToken token() const noexcept { return CallbackToken{_state}; }

    /// @brief Wraps @p fn so it runs only while this scope is alive and un-stopped.
    ///
    /// Forwards to `CallbackToken::guard()` on a token for the *current*
    /// generation, so a later `reset()` retires the wrapper. This is the
    /// general-purpose form for the callbacks that are not `Completion`
    /// attachments — timer ticks, `IExecutor::post` closures, poller dispatch,
    /// event sinks.
    ///
    /// @tparam F Callable type to wrap; must return `void` (see `CallbackToken::guard()`).
    /// @param fn Callable to gate. Moved into the returned wrapper.
    /// @return A callable with @p fn's argument list and a `void` return.
    template <typename F>
    [[nodiscard]] auto guard(F&& fn) const {
        return token().guard(std::forward<F>(fn));
    }

private:
    std::shared_ptr<detail::CallbackScopeState> _state;
};

}  // namespace morph::async
