# `CallbackScope` / `CallbackToken` — design

Design spec for `morph::async::CallbackScope`, `morph::async::CallbackToken` and
`morph::async::CallbackStatus` (`include/morph/core/callback_scope.hpp`): the
lifetime-and-stop gate a receiver holds so that an asynchronous callback it
attached is *not delivered* once the receiver is gone — or once the receiver is
still there but no longer interested.

Read this before attaching any callback that captures `this`, and before
inventing another per-class liveness token.

## Contents

- [The problem](#the-problem)
- [Shape: a member, not a base class](#shape-a-member-not-a-base-class)
- [Two states, three verbs](#two-states-three-verbs)
- [API surface](#api-surface)
- [Where the gate lives](#where-the-gate-lives)
- [Thread safety and the boundary of the guarantee](#thread-safety-and-the-boundary-of-the-guarantee)
- [Declared last, destroyed first](#declared-last-destroyed-first)
- [Failure modes](#failure-modes)
- [Design decisions](#design-decisions)
- [Out of scope](#out-of-scope)
- [Cross-references](#cross-references)

## The problem

A `Completion<T>` always resolves **through an executor** — even a local
backend's immediate resolution is posted, never delivered inline
(`CompletionState<T>::attachThen`, [completion.md](completion.md)). The
receiver can therefore always be destroyed, or lose interest, before the handler
runs, and the natural spelling is silently wrong:

```cpp
completion.then([this](GetBoardResult r) { /* `this` may be long gone */ });
```

Correctness used to depend on every author independently remembering a
three-part incantation: declare a token member (last!), capture its weak form,
re-check it before touching `this`. Forgetting compiles fine — which is the
hazard. Issue #137 was a real `stack-use-after-scope` write produced exactly
this way, invisible in every unsanitised build.

Liveness is only half of it. Two different situations must both suppress
delivery:

1. **The receiver no longer exists.** Destroyed while a reply was in flight.
2. **The receiver no longer cares.** The user navigated away, cancelled the
   dialog, or typed a new query superseding the in-flight one. The object is
   perfectly alive and the callback must still not run.

(2) is the common case in a GUI — a stale search result overwriting a newer one
is a bug users actually see — and a bare `weak_ptr` cannot express it at all.

## Shape: a member, not a base class

`CallbackScope` is a plain data member of the receiver. It is deliberately **not**
a base class, and deriving from it is not a supported use.

Requiring every consumer of `BridgeHandler` to derive from a framework base
constrains a hierarchy for what should be an implementation detail of one
screen, and penalises types that already have a base, are `QObject`s, or are
aggregates. A member composes with all of them. The decisive property: **code
that does not opt in is unaffected.** Every pre-existing `then(fn)` /
`onError(fn)` / `subscribe<R>(cb)` call site keeps its exact previous behaviour;
the gated forms are additional overloads, not replacements.

```cpp
class BoardPresenter {
    void load() {
        _handler.execute(GetBoard{}).then(_callbacks, [this](GetBoardResult r) { render(r); });
    }
    void onUserNavigatedAway() { _callbacks.requestStop(); }
    void onNewQuery(QString q) { _callbacks.reset(); /* ...issue the new request... */ }

    morph::async::CallbackScope _callbacks;   // plain member; declared LAST
};
```

## Two states, three verbs

| Verb | Effect on pending callbacks | Owner still exists? | Token reports |
|---|---|---|---|
| `requestStop()` | refused | yes | `CallbackStatus::Stopped` |
| `reset()` | refused (permanently, for tokens issued before the call) | yes | `CallbackStatus::Expired` |
| `~CallbackScope()` | refused | no | `CallbackStatus::Expired` |

**Liveness and stop are kept distinguishable.** They have the same effect on
delivery, so a callback never needs to tell them apart — but a caller,
a log line and a test do. `CallbackToken::status()` is the three-way answer;
`CallbackToken::active()` is the one-bit form the gate itself uses, and
`CallbackToken::expired()` answers the liveness half alone without conflating it
with stop.

`reset()` is the **supersede** verb, and the reason the type is not just a
`weak_ptr` plus a `bool`. It retires every token issued so far *and* makes the
scope deliverable again under a fresh generation, so "a new query cancels the
old one" has a spelling that does not require heap-reallocating the scope per
request. Note the consequence for anything already installed: a callback
attached before `reset()` holds a token for the retired generation and stays
dead. Re-arming an existing subscription means re-subscribing under the new
generation — `reset()` does not revive it.

## API surface

### `morph::async::CallbackStatus`

`enum class CallbackStatus : std::uint8_t { Active, Stopped, Expired }`.

### `morph::async::CallbackScope`

| Member | Description |
|---|---|
| `CallbackScope()` | Constructs a live, un-stopped scope. |
| `~CallbackScope()` | Stops the current generation, then releases it. Does not wait for in-flight callbacks. |
| `requestStop() noexcept` | Marks this generation stopped. Idempotent. |
| `reset()` | Retires every outstanding token and starts a fresh, live generation. |
| `stopRequested() const noexcept` | Whether this generation is stopped. |
| `token() const noexcept` | Issues a weak `CallbackToken` for the current generation. |
| `guard(F&& fn) const` | Wraps `fn` so it no-ops unless this scope is alive and un-stopped. |

Neither copyable nor movable: it is an identity, not a value. A moved-from scope
would have to either strand or silently retarget tokens already captured in
flight, and `reset()` already covers the one case (regeneration) that motivates
a move.

### `morph::async::CallbackToken`

| Member | Description |
|---|---|
| `CallbackToken()` | Unbound token; permanently `Expired`. |
| `status() const noexcept` | `Active` / `Stopped` / `Expired`. |
| `active() const noexcept` | `status() == Active`. |
| `expired() const noexcept` | Whether the issuing generation is gone; does not take a strong reference. |
| `guard(F&& fn) const` | The gate itself — see below. |

Copyable and weak: a token keeps nothing alive, including the scope.

### `guard()`

`guard()` matters as much as the `Completion` overloads. A large share of the
callbacks at risk are not `Completion` attachments at all — `QTimer` ticks,
`IExecutor::post` closures, poller dispatch, event sinks. `guard()` is the
general-purpose form for those:

```cpp
QTimer::singleShot(0, _callbacks.guard([this] { tick(); }));
```

The returned callable forwards every argument and returns `void`. A
value-returning callable is rejected at compile time: there is no defensible
value to return when delivery is suppressed.

### Gated overloads elsewhere

| Surface | Gated form |
|---|---|
| `Completion<T>::then` | `then(scope, fn)`, `then(token, fn)` |
| `Completion<T>::onError` | `onError(scope, fn)`, `onError(token, fn)` |
| `BridgeHandler<M>::subscribe<R>` | `subscribe<R>(scope, cb)`, `subscribe<R>(token, cb)` |

`Completion<T>` additionally gains `thenDetached(fn)` / `onErrorDetached(fn)`:
byte-for-byte the ungated `then(fn)` / `onError(fn)`, spelled so that a
deliberately unmanaged callback says so and stays greppable in review. Both
spellings remain available; the ungated forms are **not** deprecated.

## Where the gate lives

Mechanically, the gate lives **inside the stored handler** — the callback is
wrapped at attach time by `CallbackToken::guard()`. Nothing in
`CompletionState<T>` changes, so gating composes with handler fan-out, with the
attach-after-ready fire-now path, and with executor marshalling exactly as they
already behave.

Two consequences worth stating:

- **A suppressed callback is destroyed, not leaked.** The wrapper (and with it
  the closure and its captures) is released wherever the refused task is
  discarded — normally on the delivery executor's thread, as the posted closure
  is dropped after running.
- **A scope-suppressed error still counts as handled.** Attaching
  `onError(scope, fn)` sets `onErrAttached` exactly as the ungated form does, so
  an error whose delivery the scope then refuses does **not** re-arm
  `~CompletionState`'s orphan logging. Suppression is a deliberate act by the
  receiver, not an error nobody asked about.

## Thread safety and the boundary of the guarantee

This section is the part an API of this shape is silently assumed to promise and
cannot. It is normative.

- **Every member is safe to call from any thread**, concurrently with any other.
  `requestStop()` / `stopRequested()` are single atomic
  operations (`release` store / `acquire` load); `token()` and `status()` go
  through the generation's `shared_ptr`/`weak_ptr` control block, whose reference
  counting is itself atomic.
- **Executor-affine use gets the full guarantee.** When the scope's destruction,
  `requestStop()` or `reset()` happen on the delivery executor's thread — the
  normal case, where receiver and callbacks both live on the GUI thread —
  check-then-run is atomic with respect to those operations, and a gated
  callback never touches a dead or stopped receiver.
- **Cross-thread stop is advisory.** A scope destroyed or stopped on a different
  thread from the delivery executor can turn inactive between the token check
  and the handler body. That is exactly the boundary of the hand-rolled
  `weak_ptr` idiom this type replaces — locking the token pins the *token*,
  never the receiver — and it carries over unchanged: external synchronisation
  there remains the caller's job.
- **The gate is monotone within a generation.** Once a token has been observed
  non-`Active`, it never returns to `Active`. Only `reset()` (which issues a
  *different* generation's tokens) produces a live token again.
- **`reset()` stops the outgoing generation before releasing it**, so a token
  holder that pinned the old state while racing the swap still observes refusal
  rather than a stale `Active`. `~CallbackScope` does the same.
- **Deliberately no block-until-drained.** Neither `requestStop()` nor the
  destructor waits for in-flight callbacks, `QObject::disconnect`-style. A
  GUI-thread destructor blocking on a pool-thread callback that is itself
  blocked posting back to the GUI executor is a deadlock by construction — the
  same self-join family
  [concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md) already warns
  about. A stronger cross-thread guarantee, if ever wanted, can arrive later as
  a separate opt-in without breaking this contract.

## Declared last, destroyed first

**Declare the scope after (below) every member its callbacks touch.** Members
are destroyed in reverse declaration order, so a last-declared scope is the
first thing to die and every gated callback is already refused before the fields
it would have touched are torn down.

This was previously a per-class convention, separately re-derived and
re-documented in at least six places in this repository. It is now stated once,
here and in the type's own doc comment.

**Teardown that pumps.** Members are destroyed *after* the destructor body runs.
A destructor body that can pump a nested event loop (a `sendSync`-style blocking
call) can therefore still deliver into a half-destroyed receiver. The escape
hatch is explicit: such a destructor calls `requestStop()` as its first
statement. `morph::flows::FlowSession` does exactly this.

## Failure modes

| Situation | Behaviour |
|---|---|
| Default-constructed (unbound) token | Permanently `Expired`; every gated callback is suppressed. Gating is **fail-closed**: a token that was never bound suppresses rather than admits, trading a silently-dropped callback for never dereferencing a freed receiver. |
| Callback attached to an already-stopped scope | Suppressed at delivery, as if it had been stopped afterwards. There is no attach-time diagnostic. |
| Scope reset while a subscription sink is installed | The sink stays installed but is never delivered to again; nothing prunes it. `unsubscribe<R>()` or handler destruction removes the entry. |
| Gated callable returns non-`void` | Compile error (`static_assert` in `CallbackToken::guard()`). |
| Gated handler throws | Unchanged from the ungated path: `CompletionState` isolates each handler in its own `try`/`catch` and logs. The gate is not an exception boundary. |

## Design decisions

| Decision | Rationale |
|---|---|
| **Composition, not inheritance** | A base class (`HasLifetime`, tried in #150 and closed) is a requirement on every consumer's hierarchy — impossible or costly for `QObject`s, aggregates, and types that already have a base. A member composes with all of them, and non-adopting code is untouched. |
| **A distinct type, not `std::stop_token` alone** | `std::stop_token` covers requirement (2) but not (1): destroying a `stop_source` does not request stop, so a bare stop token says nothing about liveness. The verb names deliberately mirror `stop_source`/`stop_token` so a C++20 reader recognises the shape, and so #116's work-side cancellation can share this vocabulary rather than growing a second one. |
| **Not named `CallbackContext`** | `morph::session::Context` already exists and means something entirely different (authenticated principal, token, request id). Two unrelated "Context" types in one framework is a readability tax. `Scope`/`Token` also matches the `std::stop_source`/`stop_token` pairing. |
| **Fail-closed default token** | An unbound token suppressing is a visible functional bug (a callback that did not fire); an unbound token admitting is a use-after-free. The type exists to make that trade. |
| **Three-way `status()` rather than a single `bool`** | Liveness and stop are genuinely different facts. Collapsing them is what made the hand-rolled `weak_ptr` idiom unable to express "alive but cancelled" in the first place. |
| **`guard()` as a first-class member, not just `Completion` overloads** | Roughly a third of the at-risk callbacks in the tree are timer ticks, posted closures, poller dispatch and event sinks rather than `Completion` attachments. Without a standalone guard those sites keep the incantation and only the `Completion` subset improves. |
| **Gate wrapped at attach, not enforced in `CompletionState`** | Keeps the shared state untouched, so fan-out, the fire-now path and orphan accounting all keep their existing semantics; and the same wrapper serves every other callback surface unchanged. |
| **`then(fn)` not deprecated** | The migration is mechanical but wide. Landing the primitive without breaking or warning on existing call sites is what makes it adoptable per subsystem; `thenDetached()` gives the greppable spelling now, and any deprecation is a later, separate decision. |
| **Non-movable** | See [API surface](#api-surface): a move would strand or silently retarget tokens already in flight. |
| **No block-until-drained** | See [the boundary of the guarantee](#thread-safety-and-the-boundary-of-the-guarantee): it is a deadlock by construction across executors. |

## Out of scope

- **Cancelling the work.** This gates *delivery* of a result nobody wants; it
  does nothing to the work still in flight producing it. That is issue #116's
  half of the story, and the two are meant to end up one vocabulary — if #116
  lands, `requestStop()` is its natural upstream trigger.
- **Interop with `std::stop_token`.** Constructing a `CallbackToken` from an
  externally supplied `std::stop_token` (so callbacks tie into an existing
  cancellation tree) is a deliberate future extension, not present today.
- **Pruning dead subscription sinks.** A subscription whose scope has gone
  inactive stops being delivered to but is not removed from the subscriber list.
- **Debug-build affinity assertions.** Asserting that scope operations happen on
  the delivery executor's thread needs a cheap "am I on your thread?" query on
  `IExecutor`, which does not exist yet.

## Cross-references

- [`completion.md`](completion.md) — `Completion<T>`, the gated `then`/`onError`
  overloads, `thenDetached`/`onErrorDetached`, and the orphan-logging contract a
  suppressed error does not disturb.
- [`bridge.md`](bridge.md) — `Bridge`'s own use of the primitive (`_callbacks`,
  `liveness()`) and `BridgeHandler::subscribe<R>(scope, cb)`.
- [`concurrency_and_lifetimes.md`](../concurrency_and_lifetimes.md) — the
  framework-wide destruction-ordering rules this type's "declared last" rule
  belongs to, and the self-join deadlock family the no-block-until-drained
  decision avoids.
- [`executor.md`](executor.md) — `IExecutor`; `morph::qt::QtExecutor`'s own
  `_alive` token (issue #151) is the adjacent-but-distinct case: *the executor*
  going away, rather than the receiver.
- [`workflows_navigation.md`](../forms/workflows_navigation.md) —
  `morph::flows::FlowSession`, the second in-framework adopter and the worked
  example of the "teardown that pumps" escape hatch.
