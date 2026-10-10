# Bridge types — `Bridge`, `BridgeHandler`, `ActionExecuteRegistry`

`morph::bridge` owns the client-side dispatch path: a `Bridge` holds an active
`IBackend` and a set of registered model bindings; a `BridgeHandler<Model>` is
an RAII handle that registers one model instance and exposes typed
`execute()`, type-erased `executeJson()`, and subscription APIs. A
`ActionExecuteRegistry` provides dynamic (string-keyed) dispatch for call sites
that only know action names at runtime.

## Contents

- [Architecture overview](#architecture-overview)
- [`HandlerBinding`](#handlerbinding)
- [`Bridge`](#bridge)
  - [`BridgeSink` — the typed state the backend settles](#bridgesink--the-typed-state-the-backend-settles)
- [`BridgeHandler<Model>`](#bridgehandlermodel)
- [Model-free dispatch — `RawHandler`](#model-free-dispatch--rawhandler)
- [Registration readiness — the bind rule](#registration-readiness--the-bind-rule)
- [`ActionExecuteRegistry`](#actionexecuteregistry)
  - [Why the key carries the sharing policy](#why-the-key-carries-the-sharing-policy)
- [`BRIDGE_REGISTER_ACTION` and `registerActionExecutorOnce`](#bridge_register_action-and-registeractionexecutoronce)
- [`MemberPointerTraits`](#memberpointertraits)
- [Subscription semantics](#subscription-semantics)
- [Thread safety — one owner](#thread-safety--one-owner)
- [Lifetime & ownership](#lifetime--ownership)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Architecture overview

```
GUI / call site
       │
       ▼  BridgeHandler<Model>::execute(action)
       │
       ▼  Bridge::executeVia<Model, Action>(binding, action, cbExec)
       │
       ▼  IBackend::execute(ModelId, ActionCall, cbExec)
       │
  ┌─────┴─────┐
  │  Local    │  Remote (SimulatedRemoteBackend, QtWebSocketBackend)
  └───────────┘
```

A `Bridge` owns one active backend at a time and tracks all `HandlerBinding`
instances. On `switchBackend()` it re-registers every live binding on the new
backend. `BridgeHandler<Model>` wraps one binding with an RAII lifecycle: it
registers on construction, deregisters on destruction.

`BridgeHandler` exposes two execute paths: the typed `execute<Action>(action)`
(compile-time dispatch) and `executeJson(actionType, bodyJson)` (runtime
dispatch via `ActionExecuteRegistry`).

For GUI-led workflows, `subscribe<R>(cb)` observes *the instance the handler is
attached to*: it fires whenever an `R` is produced there, by any handler
attached to it. `unsubscribe<R>()` drops the callback.

## `HandlerBinding`

Declared as `morph::bridge::detail::HandlerBinding` (internal linkage record,
not part of the public namespace).

```cpp
struct HandlerBinding {
    std::string typeId;
    std::function<std::unique_ptr<IModelHolder>()> modelFactory;
    std::string contextKey;
    std::string primary;   // shared bindings only — see shared_instances.md
    bool shared = false;
    std::atomic<uint64_t> currentId{0};

    // The bind rule's state — see "Registration readiness" below.
    bool bindInFlight = false;
    std::uint64_t bindGeneration = 0;
    std::exception_ptr bindFailure;
    std::function<void(std::exception_ptr)> afterBind;
    std::deque<std::function<void(std::exception_ptr)>> waiting;
};
```

One record per registered model instance. `typeId` is the string
`ModelTraits<Model>::typeId()` for a typed handler, or the id a raw handler was
bound by (`bindByType`). `modelFactory` creates a fresh
`IModelHolder` — used by `switchBackend()` and the reconnect handler to
re-create the instance on a backend. `contextKey` is an optional stable
identity (e.g. account id) that travels in the `register` wire envelope for
remote backends. `currentId` is the `ModelId` value the active backend
assigned; 0 = unbound.

Every field is touched only on the bridge's owner, except `currentId`, which is
an atomic so `isBound()` can be asked from any thread. The last five fields
carry the [bind rule](#registration-readiness--the-bind-rule): whether a bind
is in flight, a count that tells a current reply from a superseded one, the
last bind's failure, the continuation of the operation that issued the bind
(an attach, a result-keyed action's first bind), and the calls held until the
bind settles.

## `Bridge`

Central dispatcher. Non-copyable, non-movable. Belongs to one owner executor.

**Construction** takes ownership of an `IBackend` and the owner executor
(required; see [Thread safety — one owner](#thread-safety--one-owner)), tells
the backend its owner (`IBackend::setOwner`), pushes the initially empty
default session, and installs a reconnect handler posted to the owner, so a
backend with a recoverable transport (e.g. `QtWebSocketBackend`,
`SocketBackend`) re-registers every live binding on the owner after a
reconnect.

The reconnect handler runs as a task on the owner. It ignores a reconnect of a
backend the bridge has since switched away from. For every live binding it
clears `currentId` — the id belonged to the connection that dropped — and
issues the ordinary bind on the owner: a shared binding holding neither a
`primary` nor an instance has nothing to re-create and is skipped; an anonymous
shared binding (an instance a result-keyed action bound, not yet promoted)
re-binds anonymous, as a private one does; an attached
shared binding re-binds with its `primary` (the register-or-attach shape), so
the instance stays shared; a private binding re-binds with the private shape.
A bind that settles before returning binds at once; one still in flight holds
the binding's calls until it settles. A failed re-bind is recorded on its
binding and rejects its held calls; the rest of the loop continues.

**`registerHandler<Model>()`** creates a `HandlerBinding` with the default
`ModelFactory::create<Model>()` factory and registers it on the active
backend. Returns the `shared_ptr<HandlerBinding>`. An overload accepts a
pre-built binding (for dependency injection, custom `contextKey`, or custom
factory captures). Both issue the bind through `IBackend::bindModel` with the
owner as the delivery executor and return without waiting: a backend that
settles before returning (`LocalBackend`, `SimulatedRemoteBackend`, a
blocking-configured `QtWebSocketBackend`) has bound the binding by then;
otherwise the binding comes back with a bind in flight and every call made
through it is held until the bind settles. See
[Registration readiness](#registration-readiness--the-bind-rule).

**`executeVia<Model, Action>(binding, action, cbExec)`** dispatches one
action, on the owner. It counts the call pending and arms the client-side
deadline at once; then, when no bind is in flight for the binding, it
dispatches to the active backend here, and otherwise holds the call on the
binding until the bind settles (see
[Registration readiness](#registration-readiness--the-bind-rule)). A binding
with no instance and nothing to wait for — an `AllowShared` handler never
attached — rejects the call with `"handler not bound"`. Constructs an `ActionCall`
holding one `make_shared<Action>` and three stateless function pointers — the
serialiser, the deserialiser, and a `localOp` that, on
`LocalBackend`, first overwrites any declared computed fields from their
inputs (`morph::forms::recomputeAll`, [forms.md](../forms/forms.md), a no-op
for actions with no `computedFields`) — the authoritative recompute for
`LocalBackend`, mirroring `ActionDispatcher::registerAction`'s runner
(`registry.md`) for remote topologies — then enforces
`morph::model::ActionValidator<Action>::ready(action)` — throwing
`morph::model::ValidationError` (which resolves the `Completion` through
`onError`) when it returns `false` — then calls `Model::execute(*action)` and
optionally records a journal `LogEntry` for loggable actions. Recompute runs
**before** the validator check so a validator inspecting a computed field
sees the authoritative value, not whatever the caller constructed the action
with; actions with no validator are unaffected (`ready()` defaults to
`true`). No JSON is involved on this path, so there is no declared-precision
reconciliation step here (that only applies to decoded wire payloads); the
`Quantity` fields carry whatever precision the caller constructed them with.
The `ActionCall` the two type ids are stamped onto holds them as
`std::string_view`s of the `constexpr` literals `BRIDGE_REGISTER_MODEL` /
`BRIDGE_REGISTER_ACTION` were given, and the three callables are plain function
pointers reading the action out of `ActionCall::action`. Nothing in this block
captures, so nothing in it allocates but the action itself — which matters
because it runs on every call, including the `LocalBackend` calls that will
never look at the two remote-path callables. See
[backend.md](backend.md), "Why the callables are function pointers", for the
measurement and for the borrow contract that shape carries.

**`localOp` is compiled — and so needs `Model::execute`'s definition to
link — every time `executeVia<Model, Action>` is instantiated, regardless of
which backend ends up installed at runtime.** Only `LocalBackend::execute`
ever actually calls `call.localOp`; every remote backend ignores it entirely.
But the function itself is still compiled into the instantiation, so a build
that only ever installs a remote backend still forces the linker to resolve
`Model::execute` — see `registry.md`, "`MORPH_CLIENT_ONLY`". When
`MORPH_CLIENT_ONLY` is defined, `localOp`'s body is replaced with a
`std::logic_error` throw instead of the block described below, so nothing in
the compiled program references `Model::execute`'s definition. Reaching that
throw at runtime means `LocalBackend` was used in a build that promised never
to — a configuration error, not a normal failure mode.

Otherwise (the default, non-`MORPH_CLIENT_ONLY` build), `Model::execute(*action)`
itself — and nothing else — is wrapped in a `try`/`catch (const std::exception&)`:
on a throw it records `outcome = Outcome::Failed` (`error = exc.what()`,
`result` empty) for loggable actions and rethrows unchanged, so the exception
still resolves the `Completion` through `onError` exactly as before — the
journal entry is a side effect of the attempt, not a change to error
propagation. Serialising the result and recording `outcome = Outcome::Succeeded`
run after that `try`, because by then the mutation has committed: a sink whose
`append` throws, or a result that will not serialise, resolves the `Completion`
through `onError` with `morph::model::ActionRecordingError` rather than being
reported — and journaled — as the model refusing the action. Mirrors
`ActionDispatcher::registerAction`'s runner (`registry.md`) for remote
topologies. See [journal.md, "A refused recording is not an execution
failure"](../journal/journal.md#a-refused-recording-is-not-an-execution-failure)
and [journal.md,
"Outcome"](../journal/journal.md#logentry--one-recorded-action-execution) for
the full field/replay semantics.

### `BridgeSink` — the typed state the backend settles

`executeVia` used to create **two** completions per dispatch: the typed one it
hands the caller, and the erased `Completion<std::shared_ptr<void>>` the backend
produced, with a `.then`/`.onError` pair forwarding one into the other. The
forwarding cost six heap allocations — the erased `CompletionState`, the two
closures, their two handler vectors, and one of the two posted settle tasks.

`detail::BridgeSink<R>` is both halves at once: it derives from
`async::detail::CompletionState<R>` (so it *is* the caller's typed state) and
from `async::detail::ISettleSink` (so the backend settles it directly, through
[`IBackend::executeInto`](backend.md#executeinto--settling-the-callers-own-completion)).
One allocation replaces six. Measured on a `Ping{int}` → `Pong{int}` local
round trip, clang 22.1.8 Release, twelve runs each, every run identical:
**14.06 → 8.06 allocations per call, 1158.7 → 814.7 bytes**. The schema-driven
`executeJson` path, which runs through the same function, falls **21.07 →
15.06**.

Everything the forwarding block did is a method on that class:

| Invariant | Where it lives |
|---|---|
| The deadline is disarmed **first**, before any forwarding work, so a slow `onResult`/`publishResult` cannot give the timer a window to resolve the completion with `ClientTimeoutError` while the real result is in hand | `BridgeSink::settleOnce`, called at the top of both settle methods |
| `pendingCalls()` is decremented on **exactly one** of two mutually-exclusive paths, whether or not the forwarding that follows then throws | `settleOnce`'s latch |
| The bridge-side work a result triggers — `onResult` (a result-keyed action's promotion) and the subscription fan-out — runs on the owner, gated on the bridge's `CallbackToken`, before the value reaches the caller | `BridgeSink::settleValue` / `forward` |
| The value forwarding is `try`/`catch`-guarded, so a throwing move of `R` reaches the error sink rather than the callback executor | `BridgeSink::forward` |
| A throw out of the backend undoes both the pending count and the deadline | `BridgeSink::abandon`, routed through the same latch |

The latch is load-bearing. With a plain `Completion`, "exactly one decrement per
dispatch" is carried by `.then` and `.onError` being mutually exclusive on one
`CompletionState`. A sink has no such guarantee:
`IBackend::cancelPending` settles it from one thread while a reply may be
settling it from another. Without the latch the second settle decrements
`_pendingCalls` again, and the counter is a `std::size_t` — a second decrement
from zero reads as `18446744073709551615`, in a value `pendingCalls()`
documents as a quiescence gate. `test_bridge_pending_calls.cpp` pins exactly
that sequence.

The typed result is unwrapped from `std::shared_ptr<void>`
into the final `Completion<R>` inside a `try`/`catch`: moving the result out of
the opaque `shared_ptr<void>` can throw (a throwing move/copy on `R`, or a bad
cast), and if that exception escaped it would be swallowed by a
`ThreadPoolExecutor` — leaving the typed `Completion` unresolved (a silent
hang) — or reach `QCoreApplication::exec` under `QtExecutor` and
`std::terminate`. On a caught exception the forwarding routes it to the typed
completion's error sink via `setException`, so the caller's `.onError(...)`
fires instead. The type-erased `executeJson` path
(`ActionExecuteRegistry::registerAction`) guards its own `resultToJson`
forwarding the same way.

**Where the settle runs.** A backend settles the sink on its own thread — a
pool strand, the I/O loop, the Qt thread. When the result has no bridge-side
work (no `onResult`, no subscriber — `hasSubscribers()` is an atomic the sink
reads there), the value is forwarded at once, where the backend settled, and
the caller's continuations are posted to its `cbExec`. When it has some, the
rest of the settle is done on the owner: inline when the backend already
settled there, posted otherwise. That task checks the bridge's
`CallbackToken` first; a bridge destroyed in the meantime has expired it, so
the bridge-side work is skipped and only the value is delivered. On the owner
the check is exact, because `~Bridge` runs there too.

**`switchBackend(newBackend)`** replaces the active backend, on the owner. It
tells the new backend its owner and pushes the current default session onto it
(so every control envelope the re-binds build carries the session, exactly as
it would on the backend being replaced — see `IBackend::setSession` and
[backend.md](backend.md#session-propagation-to-control-envelopes)), then
issues a bind on it for every live binding with the owner as the delivery
executor. A shared binding holding neither a `primary` nor an instance has
nothing to re-create and is left as it is; an anonymous shared one re-binds
anonymous, since its id belongs to the backend being replaced; an attached
shared one re-binds with its `primary`.

The binds the new backend settles **before returning** decide the switch. If
any of them failed, every instance acquired so far is released — at once for
the settled ones, when it settles for one still in flight — and the failure is
rethrown with the old backend and every binding untouched: the call is a
no-op. Otherwise the switch commits:

- each settled id is published into its binding's `currentId`;
- a binding whose bind is still in flight gets `currentId = 0` and holds its
  calls until the bind settles; a failure then cannot roll the switch back and
  is recorded on the binding, rejecting its held calls;
- the operation continuation of a bind superseded by the switch (an attach in
  flight on the old backend) is rejected with `BackendChangedError`;
- the backend is swapped, `notifyBackendChanged()` runs, the reconnect handler
  moves to the new backend and is cleared on the old one, and the old backend's
  pending calls are cancelled with `BackendChangedError` — a Task handler still
  running there is asked to stop (its `StopSource`), not only settled;
- finally the calls each binding held are drained.

Every backend in the tree settles a bind before returning except a
`SynchronousBackendAdapter`, a `SocketBackend` and an async-configured
`QtWebSocketBackend`; over those the switch is all-or-nothing only for the
binds that had settled when it committed.

It has two overloads:

- `switchBackend(shared_ptr<IBackend>)` — the caller keeps its own reference,
  so the same backend instance can be re-installed later (e.g. switching back
  to a long-lived remote backend, with its live socket and reconnect state,
  after a temporary fallback to a local one) instead of reconstructing it.
- `switchBackend(unique_ptr<Backend>)` — transfers ownership.
  Templated on the concrete `Backend` type (rather than taking
  `unique_ptr<IBackend>` directly) so that a call like
  `switchBackend(std::make_unique<LocalBackend>(...))` is an *exact* match
  and is preferred over the `shared_ptr<IBackend>` overload; a non-template
  `unique_ptr<IBackend>` overload would tie with it (both are one
  equally-ranked user-defined conversion from `unique_ptr<Backend>`), making
  every existing call site ambiguous. It converts to a `shared_ptr` and
  delegates to the overload above.

**`onBackendChanged()` runs on the model's strand.** `notifyBackendChanged()`
does not *call* each model's `onBackendChanged()` on `LocalBackend` — it
**posts** it onto that model's own strand (the same per-`ModelId` serial queue
`execute` uses) and returns. Consequences:

- **Strand-serialised, lock-free.** The callback body runs single-threaded per
  model, never overlapping an `execute()` on the same model, so a model
  reconciling state there (e.g. draining a shared `IOfflineQueue`) needs no
  locking of its own fields. It runs *after* `switchBackend` returns, so an
  observer must wait for it rather than assume it ran synchronously.
- **It runs on a pool thread, off the owner.** A model that wants to register
  or deregister a handler from there is a handler constructed inside a running
  action: its constructor posts the registration to the owner (see
  [Registration readiness](#registration-readiness--the-bind-rule)).
- **`switchBackend()` from `onBackendChanged()` is unsupported**: it is an
  owner-only verb, and the callback runs on the *outgoing* backend's strand;
  releasing the last reference to that backend from there makes
  `~LocalBackend` drain its strands — including the very task calling it — a
  self-join hang, asserted in a debug build.

**`deregisterHandler(binding)`** (called by `~BridgeHandler`) advances the
binding's bind count, so a bind reply still in flight is superseded and
releases its instance when it lands; rejects the in-flight operation's
continuation and every held call with `HandlerDestroyedError`; deregisters
`currentId` from the active backend if it is non-zero and resets it to `0`;
and removes the binding from tracking.

**`setDefaultSession(session)`** / **`defaultSession()`** installs a default
`morph::session::Context` that is attached to every `executeVia()` call.
`setDefaultSession` also pushes the new session to the active backend via
`IBackend::setSession`, so every control envelope
(`register`/`registerShared`/`attach`/`assign`/`deregister`) the backend
subsequently builds carries the session too — not only `execute` envelopes.
The constructor does the same with the (typically empty) initial session. See
[session.md](../session/session.md#how-a-context-originates-and-flows) and
[backend.md](backend.md#session-propagation-to-control-envelopes).

**`setExecuteDeadline(deadline)`** / **`executeDeadline()`** installs an
opt-in, client-side wall-clock bound on how long any subsequent `executeVia()`
waits for a reply, measured from the call — a wait for the binding's bind
included. Defaults to `std::chrono::milliseconds{0}` (disabled, and no extra
thread). When enabled, each dispatch races the real reply against a
`morph::async::detail::TimeoutScheduler` timer that resolves the pending
`Completion` with `morph::backend::ClientTimeoutError` and requests stop on a
Task handler's `StopSource`; whichever settles first wins, and the loser is
discarded by `CompletionState`'s first-result-wins rule. The on-time reply
disarms the timer first. A call whose deadline fired while it was held for its
bind is never dispatched. Full semantics — including how `ClientTimeoutError`
differs from the server-reported `TimeoutError` — in
[completion.md](completion.md#client-side-execute-deadline).

**`setPrincipal(principal)`** / **`currentPrincipal()`** installs and reads
back a `morph::session::Principal` — the verified identity + roles, readable
*outside* a dispatch (unlike `session::current()`, which only exists during
one), so UI code can gate itself (`bridge.currentPrincipal().hasRole("editor")`)
instead of attempting an action and catching the refusal. Scoped to this
`Bridge` instance, not a process-wide global — see
[session.md](../session/session.md#principal--readable-authorization-state-outside-a-dispatch)
for the full rationale and trust model. Purely a client-side convenience: it
has no wire representation and does not affect dispatch or `Context` in any
way — every dispatch is still authorized server-side via `IAuthorizer`
regardless of what `currentPrincipal()` says.

**`pendingCalls()`** returns the number of calls made through
`executeVia()` and its keyed variants (and so, transitively,
`BridgeHandler::execute()`), or through `executeRaw()`/`executeRawOn()`, that
have not yet resolved — a client-side quiescence signal for building a "still
loading" indicator or gating a feature on "has everything settled" without
hand-rolling a counter around every call site. Backed by a
`std::shared_ptr<std::atomic<std::size_t>>` (`_pendingCalls`), incremented
once per call when it is made (a call held for its bind included) and
decremented exactly once by the sink's settle latch — success, error, or a
cancellation (`BackendChangedError`, `BridgeDestroyedError`, a dropped
transport, a destroyed handler). Pinned in every sink rather than reached
through `this`, because a sink is settled on the backend's thread and may be
settled after `~Bridge`. A single relaxed atomic load — cheap enough to poll
every UI frame, from any thread. See [Design decisions](#design-decisions) for
why the counter lives at the `Bridge` layer rather than per-`HandlerBinding`
or per-backend.

**Destructor**, on the owner, clears the active backend's reconnect handler,
supersedes every live binding's bind and rejects the calls each still holds
with `BridgeDestroyedError`, and cancels every pending call on the backend with
the same error (a running Task handler is asked to stop). In-flight replies that
arrive afterwards are no-ops (`CompletionState::setValue`/`setException` is
idempotent); a bind reply for a binding the bridge no longer tracks releases its
instance on the backend that issued it. A reconnect, bind reply or result task
already queued on the owner finds the bridge's `CallbackToken` expired — the
`CallbackScope` member is declared last, so it is destroyed first — and touches
nothing.

**`liveness()`** (private, exposed only to `BridgeHandler` and `RawHandler` via friendship)
returns a `morph::async::CallbackToken` issued from the bridge's `_callbacks`
member — a `morph::async::CallbackScope` created with the bridge and destroyed
with it. Each `BridgeHandler` captures it at construction and consults it in
its destructor, so destroying the `Bridge` before its handlers (on the owner)
deregisters nothing rather than touching a destroyed bridge. Every continuation
the bridge queues on the owner — bind and promote replies, reconnects,
bridge-side result work — checks the same token first. See
[Lifetime & ownership](#lifetime--ownership) and
[callback_scope.md](callback_scope.md).

## `BridgeHandler<Model>`

RAII handle. Registers a `HandlerBinding` on construction, deregisters on
destruction. Non-copyable.

**Construction** takes a `Bridge&` and a GUI executor (`guiExec`, the
executor its completions are delivered on: the bridge's owner or one that runs
its tasks on the owner's thread). Optionally accepts a pre-built
`HandlerBinding` (for dependency injection). It copies the bridge's
`CallbackToken` (`liveness()`) and owner affinity, holds a `Bridge&`, and
carries the sharing policy as its second template argument (see
`BridgeHandler<Model, Sharing>` below). On the owner it registers its binding
at once; constructed anywhere else — inside a running action on a pool thread —
it marks the binding as binding and posts the registration to the owner, so a
call made through it meanwhile is held rather than refused. Either way it never
waits.

**Destruction**, on the owner, deregisters the binding via
`Bridge::deregisterHandler` — which rejects every call still held for a bind
with `HandlerDestroyedError` and releases a late bind reply's instance — but
only if the bridge's token is still active. If the `Bridge` was destroyed first
the token has expired and the destructor deregisters nothing; on one thread
that check cannot be overtaken by `~Bridge`.

**`execute<Action>(action)`** delegates to `Bridge::executeVia` with the
handler's binding and GUI executor. Default session is attached
automatically by the bridge.

**`executeJson(actionType, bodyJson)`** type-erased variant. Looks up the
executor in `ActionExecuteRegistry::instance()` — under *this handler's own*
`Sharing` policy, not unconditionally under `NoSharing`, so the executor casts
the `void* this` back to the instantiation it actually is (see
[Why the key carries the sharing policy](#why-the-key-carries-the-sharing-policy)) —
and dispatches. The
registry's executor deserializes the JSON body via
`ActionTraits<Action>::fromJson`, then — before invoking the handler —
**reconciles Quantity precision, overwrites any declared computed fields
(`morph::forms::recomputeAll`, [forms.md](../forms/forms.md)), and enforces
the action's validator** (see below), calls `execute<Action>`, and serializes
the result back to JSON. Throws `std::runtime_error` if the action was never
registered.

Between decode and dispatch the registry executor applies four
normalisations, in order, so the request/reply path matches the schema and
`morph::flows::FlowSession::set<>`'s gate rather than trusting the raw wire
body:

- **Declared-precision reconciliation.** `morph::forms::reconcileDeclaredPrecision`
  **rounds** every `Quantity` field of the decoded action to its *declared*
  precision (`Quantity<U, Dec>::declaredDecimals`), so the stored *value* — not
  merely its precision tag — matches the schema's advertised `x-decimalPlaces`
  instead of whatever runtime `dp` the client sent. Rounding rather than
  retagging is what keeps the stored value and the displayed value the same
  number. It is a no-op for actions with no `Quantity` members and for
  actions whose type glaze cannot reflect. See [forms.md](../forms/forms.md).
- **Pre-decode wire validation.** `morph::forms::enforceQuantityBounds` rejects
  a `Quantity` field whose engaged value falls outside its unit's declared
  bounds (`UnitTraits<E>::bounds`), throwing `QuantityDecodeError` — caught by
  the same catch block as every other decode/validation failure on this path.
  Runs after precision reconciliation and before the validator check below, so
  an out-of-bounds value never reaches business-rule validation or the
  handler. No-op for actions with no `Quantity` members, or whose units
  declare no `bounds()`. See [forms.md](../forms/forms.md), "Pre-decode wire
  validation".
- **Computed-field recompute.** `morph::forms::recomputeAll` overwrites every
  `A::computedFields` destination from its declared inputs, discarding
  whatever value the wire carried for it — a computed field is never trusted
  from the client, on any dispatch path. No-op for actions with no
  `computedFields`. Runs after precision reconciliation (so the inputs it
  reads are already at their declared precision) and before the validator
  check below (so a validator inspecting a computed field sees the
  authoritative value). See [forms.md](../forms/forms.md).
- **Validator enforcement.** `ActionValidator<Action>::ready(action)` is checked;
  if it returns `false` the executor throws `std::invalid_argument` and the
  completion resolves through `onError` (a proper error reply upstream) — the
  handler is never invoked with an invalid action. This closes a gap where the
  request/reply path skipped the readiness/validity check the dispatch paths
  perform. `ActionValidator::ready`
  auto-detects a `bool validate() const` member and defaults to `true`, so
  actions without a validator dispatch exactly as before (backward compatible).

**`unsubscribe<R>()`** removes this handler's callback for result type `R`.

**`subscribe<R>(scope, cb)`** / **`subscribe<R>(token, cb)`** are the gated
forms: same subscription, but `cb` is delivered only while the supplied
`morph::async::CallbackScope` is alive and un-stopped. Subscription sinks are
the longest-lived callbacks in the system — they fire repeatedly for as long as
the handler exists — so they are the attachments most likely to outlive the
screen that installed them. A sink whose scope has gone inactive is **not**
pruned: delivery is refused and the entry stays until `unsubscribe<R>()` or
handler destruction removes it. See [callback_scope.md](callback_scope.md).

**`subscribe<R>(cb)`** registers `cb` against this handler's binding. The
subscription is matched at publish time by comparing the binding's current
instance, so it follows the handler when it re-points. There is one callback
per `(handler, R)` — subscribing again replaces the previous one — and it is
delivered on the handler's executor. Delivery is best-effort and unbuffered,
with no replay, and a failed action notifies nobody: an error reaches the
caller through the `Completion` its `execute()` returned, not through the
subscription.

**`guiExecutor()`** returns the executor passed at construction.

## Model-free dispatch — `RawHandler`

A client compiled without a model's C++ type — the declarative UI's generic
client — still has to bind that model, dispatch its actions and share its
instances. It knows only the ids the registry publishes. These seams on
`Bridge` serve it, and `RawHandler` wraps them as `BridgeHandler` wraps the
typed calls:

| Seam | What it does |
|---|---|
| `bindByType(typeId, BindSharing, instanceKey = {})` | Builds a `HandlerBinding` whose factory resolves `typeId` in the process registry (`defaultRegistry().create`) when a local backend binds it; a remote backend sends the id and never calls the factory. It is adopted through `adoptHandler`, the path a typed handler takes, so the [bind rule](#registration-readiness--the-bind-rule) applies unchanged. A private binding is bound at once; a shared one when it is attached. |
| `executeRaw(binding, actionId, bodyJson, cbExec[, stop])` | Dispatches the action registered as `actionId` with a JSON body on the instance the binding holds, and resolves with the reply's JSON text. |
| `executeRawOn(binding, instanceKey, actionId, bodyJson, cbExec)` | `executeRaw` on the instance for `instanceKey`: attaches the shared binding to it first, and rejects the call if that attach does not leave the binding there. |
| `attach(binding, key)` | Attaches or re-points a shared binding to the instance for a canonical key string. Refuses an empty key (`std::invalid_argument`) and a private binding (`std::logic_error`). `attachHandler<Model>`, the typed path, skips the private-binding check, since a typed handler's sharing is fixed by its type, and refuses an empty key without throwing: logged, and recorded as the bind failure of a binding with no instance. |
| `RawHandler` | RAII: binds through `bindByType` on construction, dispatches through `executeRaw` (`execute`) and `executeRawOn` (`executeOn`), attaches through `attach`, deregisters through `deregisterHandler` on destruction. Non-copyable, non-movable. |

**The call.** `detail::RawAction` holds both ids and the body, and the call's
`ActionCall::action` owns it. `ActionCall::modelTypeId` and `actionTypeId` are
views into it, so they outlive the dispatch however short-lived the caller's
strings were. On a remote backend `serializeAction` returns the body unchanged
and `deserializeResult` keeps the reply text. On `LocalBackend` the call runs
the server's own JSON runner on the holder — `ActionDispatcher::dispatch`
through `localOp` for an ordinary handler, `dispatchAsync` through
`localOpAsync` for a `Task` handler, chosen by `dispatchesAsync` when the call
is built — so the local path recomputes, validates and journals exactly as a
server does, and an ordinary handler pays no `Task` run's cost. The client side
does neither: `recomputeAll` and `ActionValidator<Action>::ready` need the
action's type, and whoever runs the model runs both.

**Shared with a typed call.** `makeSinkFor` builds the sink, as it does for
`executeVia`: it is counted in `pendingCalls()` and armed with the execute
deadline. `dispatchWith` — `dispatchNow`'s body — stamps the default session
and dispatches through `IBackend::executeInto`. `executeRawOn` waits for, and
attaches through, `dispatchAttached`, the body a typed payload-keyed call runs.
The cancellable overload links the caller's `StopToken` as
`BridgeHandler::execute(action, stop)` does. `cancelPending` on a
`switchBackend` settles the call, and the binding is re-bound with every other
live binding. `deregisterHandler` rejects calls held for its bind with
`HandlerDestroyedError`.

One part is a copy rather than a shared function: `executeRaw`'s
hold-while-binding block repeats `executeVia`'s. Sharing it would make
`executeVia` allocate its action on every call to hand it to a type-erased
continuation, on the path `bench.alloc_budget` gates.

**Different from a typed call, and why:**

- **No subscription publish.** The sink is built with `detail::Publish::No`.
  The reply is JSON text, and a `subscribe<R>` on the same instance expects the
  typed result, not the encoded one. A typed subscriber is therefore not told
  about a raw call's change to a shared instance.
- **A stop source on every call.** On a remote backend the bridge cannot tell
  whether the handler is a `Task`, so it cannot omit one the way a typed call
  omits it for an ordinary handler.
- **The key is the caller's.** A raw call cannot extract a key from the body
  without the action's type, so it neither attaches to the key a payload-keyed
  action names nor creates and promotes an instance for a result-keyed one.
  `executeRaw` runs on the instance the binding holds, and one sent before any
  attach is rejected as not bound. A keyed action goes through `executeRawOn`
  with the key the caller already has (the UI document's `instance`, spec 5 §5).
  `attach` alone is not enough: it is issued, not awaited, and a refused attach
  leaves the binding on its previous instance.
- **The key is matched exactly.** It must be the canonical encoding
  `keyToString` gives the model's key type. Another spelling of the same key —
  `"041"` for `41` — names a second instance.
- **Keyedness is not checked.** A shared binding of an unkeyed model is not
  refused by the bridge; the backend refuses it when attached, if at all. The
  interpreter refuses such a document at load from the catalog's `keyed` flag.
- **A key off the owner is refused.** `bindByType` constructed inside a
  running action posts its registration to the owner like any handler, but
  attaching is the owner's, so a key given there throws `std::logic_error`.

Under `MORPH_CLIENT_ONLY` no model registers with the process registry, so a
`LocalBackend` bind fails with `unknown model type` and every call through the
binding is rejected with that failure. A client-only build talks to a server
and does not install `LocalBackend`.

## Registration readiness — the bind rule

**The bind is a `Completion` delivered on the owner; a backend that can settles
it before returning; `execute` dispatches immediately when the binding is bound
and no bind is in flight, and chains on the bind otherwise.**

Every bind the bridge issues — a handler's registration, a shared handler's
attach, a result-keyed action's first bind, a switch's or reconnect's re-bind —
goes through `IBackend::bindModel` (`promoteModel` for a promotion) with the
owner as the delivery executor. Nothing waits for it:

- **Settled before returning.** The bridge reads the returned `Completion`'s
  outcome in the same call (`detail::takeSettled`) and applies it at once. A
  `LocalBackend` handler is therefore bound when its constructor returns, and
  its first `execute` dispatches without the owner being pumped.
- **Still in flight.** The binding is marked `bindInFlight`; the reply is a task
  on the owner that applies the outcome, gated on the bridge's token.

Applying an outcome, on the owner: a reply for a binding that is gone, or whose
`bindGeneration` a newer bind (a switch, a reconnect, the handler's
destruction) has passed, releases a successful id on the backend that issued it
and changes nothing else. The current reply publishes `currentId` (and, for an
attach, the binding's `primary`) or records its failure in `bindFailure` and
logs it; then runs the issuing operation's continuation (`afterBind`) and
resumes the calls held in `waiting`, in the order they were made, while no new
bind is in flight.

A call made through a binding:

| Binding state | What `execute` does |
|---|---|
| No bind in flight, bound | Dispatches now. |
| A bind in flight | Holds the call; dispatches it on the owner when the bind succeeds. |
| The bind it waited for failed | Rejects it with the bind's error. |
| The handler is destroyed while the call is held | Rejects it with `HandlerDestroyedError`. |
| The bridge is destroyed while the call is held | Rejects it with `BridgeDestroyedError`. |
| A switch supersedes the attach or first bind the call itself issued | Rejects it with `BackendChangedError`. Calls merely held behind that bind resume against the new backend. |
| No instance and nothing to wait for (an `AllowShared` handler never attached) | Rejects it with `"handler not bound"`. |

A held call is never left unsettled: every path that drops a binding's held
calls rejects them, so a coroutine awaiting one is resumed.

The client-side deadline, when one is set, is armed when the call is made, so
it covers the wait for the bind; a call whose deadline fired while it was held
is not dispatched.

**Why registration is asynchronous.** A `BridgeHandler` may be constructed
inside a running action, on a pool thread that is not the owner (a model that
registers a sub-model). The handler cannot touch the bridge from there, so it
posts its registration to the owner and returns; whatever is made through it
meanwhile is held. That case is the reason the bind is a `Completion` rather
than a blocking call, not a cost of it.

**`isBound()`** remains the synchronous, point-in-time answer: an atomic read of
`currentId != 0`, safe from any thread. Nothing needs to wait on it, because a
call made while a bind is in flight is held. `Bridge::isBound(binding)` is the
free-function form; `BridgeHandler::isBound()` forwards to it. `true` means
bound, not reachable: the transport can drop the moment after.

## `ActionExecuteRegistry`

Process-level singleton (`instance()`). Maps a **three-part key** —
`(modelTypeId, actionTypeId, typeid(Sharing))` — to `Executor` values
(`std::function<Completion<string>(void*, string_view)>`). The backing store is
`unordered_map<Key, Executor, KeyHash, KeyEqual>`, where `Key` is
`{string modelId; string actionId; std::type_index sharing;}` and `KeyHash`
mixes the `type_index`'s `hash_code()` into the `morph::model::detail::PairKeyHash`
the server-side `ActionDispatcher` uses over the two strings alone. Populated by
`registerActionExecutorOnce<Model, Action>()`, which
`BRIDGE_REGISTER_ACTION` calls during static initialization.

**No key is built to look one up.** `KeyHash` and `KeyEqual` are
both transparent, so `execute` probes with `KeyView`
(`{string_view modelId; string_view actionId; std::type_index sharing;}`)
rather than constructing a `Key{...}` temporary and its two `std::string`s.
`KeyView` is a type of its own rather than a reuse of
`morph::model::detail::PairKeyView`: this key carries the sharing tag as well
as the two ids, and a reuse would have to drop it. Both `KeyHash` overloads
reduce to the `KeyView` body for the reason `PairKeyHash`'s do — a lookup hash
that disagreed with the stored hash would miss the bucket and report a
registered action as unknown, with no diagnostic anywhere.

`BridgeHandler::executeJson` also stopped copying
`ModelTraits<Model>::typeId()` into a `std::string` to pass it. That was the
same allocation, on the same call, for the same reason; the traits accessor is
a `constexpr std::string_view` over a string literal, so it is passed through.

**Measured** with `morph_bench_alloc`'s `executeJson` census
(clang 22.1.8 / libstdc++ 16.2.1, Release, 200 round trips, 50 warm-up
excluded), for a model and action whose ids both exceed libstdc++'s
15-character SSO buffer and again for a pair whose ids both fit inside it:

| | ids past SSO | ids inside SSO |
| --- | --- | --- |
| before | 24.07 allocations per call | 21.06 |
| after | 21.07 | 21.06 |

Three allocations per user action for long ids — the `Key`'s two plus
`executeJson`'s own — and none for short ones, which is why the census reports
both sides rather than averaging them. The figure is a whole round trip, codec
included, because this registry cannot be probed on its own: it dispatches
whatever it finds. The gap between the two columns is what the ctest gate
`bench.alloc_budget` watches, via `--id-length-budget`.

### Why the key carries the sharing policy

This registry is the one place in the framework that recovers a typed handler
from a `void*`, and `BridgeHandler<Model, Sharing>` is a class template over
its sharing policy: `BridgeHandler<M, NoSharing>` and
`BridgeHandler<M, AllowShared>` are unrelated types. An executor that
`static_cast`s to one of them and is handed the other produces a pointer to the
wrong type — and, concretely, one whose `kShared` constant answers for the
wrong instantiation. `kShared` is what decides whether a payload- or
result-keyed action performs its attach-or-promote step, so the failure of a
single `NoSharing`-only executor is not a crash or a thrown error but a shared
handler whose keyed action silently never acquires its instance.

Type erasure removes the compiler's ability to catch that, so the key restores
it by hand: `registerAction` files **one executor per sharing policy the
framework defines**, and `execute` is a template on `Sharing` so each call site
names the entry matching its own handler. `BridgeHandler::executeJson` passes
its own `Sharing` (see [`BridgeHandler<Model>`](#bridgehandlermodel) above), so
the match holds by construction for every in-framework call site.

Both executors are built from the same generic-lambda template, instantiated
once for each tag, so their bodies cannot drift apart — the difference between
them is exactly the type named in the `static_cast` and nothing else. The cost
is one extra closure per registered action at static-init time, not per call.

**`execute<Sharing>(modelId, actionId, handler, bodyJson)`** looks up the
executor and invokes it. The executor `static_cast`s the `void*` back to
`BridgeHandler<Model, Sharing>*`, deserializes the JSON body, reconciles
Quantity fields to their declared precision, enforces
`ActionValidator<Action>::ready` (throwing `std::invalid_argument` on failure),
calls `handler->execute<Action>()`, and serializes the result back to JSON.
Throws `std::runtime_error` if no executor is registered for the key — which
includes a correctly registered action requested with a sharing tag other than
`NoSharing`/`AllowShared`, since only those two are ever filed (see
[Limitations](#limitations)).

**Requirement**: every translation unit calling `BRIDGE_REGISTER_ACTION`
must include `bridge.hpp` (directly or transitively), because
`registerActionExecutorOnce` is defined in this header and the static
initializer will fail to link otherwise.

## `BRIDGE_REGISTER_ACTION` and `registerActionExecutorOnce`

```cpp
// namespace morph::model::detail
template <typename Model, typename Action>
inline bool registerActionExecutorOnce(std::string_view modelId,
                                        std::string_view actionId) noexcept;
```

Lives in `morph::model::detail`. It is *forward-declared* (non-`inline`) in
`registry.hpp` and *defined* `inline` here in `bridge.hpp` (after
`ActionExecuteRegistry`), breaking the `registry.hpp` → `bridge.hpp` include
cycle. The body calls
`ActionExecuteRegistry::instance().registerAction<Model, Action>()` and
returns `true`.

The `BRIDGE_REGISTER_ACTION` macro is defined in `registry.hpp`. Its
expansion (a) specialises `morph::model::ActionTraits<Action>` with JSON
codecs, `typeId()`, and a `loggable` flag, and (b) emits two anonymous-
namespace `const bool` initializers that call
`morph::model::detail::registerActionOnce<M, A>` (server-side dispatcher) and
`morph::model::detail::registerActionExecutorOnce<M, A>` (this registry) at
static-init time. The `inline` keyword lets the definition in this header be
included and instantiated across many translation units without an ODR/link
violation; the registration itself runs from the macro's static initializer.

`BRIDGE_REGISTER_ACTION_SOURCE(M, A)` — the `.cpp`-side half of
`BRIDGE_REGISTER_ACTION` decomposed into a header-only declaration
(`BRIDGE_DECLARE_ACTION`) plus a registration call, for a model header whose
per-TU registration cost is worth moving out (see
[registry.md, "Moving a registrar out of the
header"](registry.md#moving-a-registrar-out-of-the-header)) — emits
exactly the same two initializers as (b) above, so it carries the identical
`#include <morph/core/bridge.hpp>` requirement: the translation unit calling
`BRIDGE_REGISTER_ACTION_SOURCE` must include this header, or the link fails
on the same unresolved `registerActionExecutorOnce<M, A>` symbol.

## `MemberPointerTraits`

Declared as `morph::bridge::detail::MemberPointerTraits`.

```cpp
template <typename T> struct MemberPointerTraits;

template <typename V, typename A>
struct MemberPointerTraits<V A::*> {
    using ClassType = A;
    using ValueType = V;
};
```

Compile-time decomposition of a pointer-to-data-member type. Recovers both the
class (`A`) and the member type (`V`) from a single non-type template
parameter, so a caller names a field as `&MyAction::c` with no redundant type
arguments. Used by `morph::flows::FlowSession::set<>`
([workflows_navigation.md](../forms/workflows_navigation.md)) and by
`morph::forms`' computed-field declarations.

## Subscription semantics

`subscribe<R>(cb)` is keyed on the **result/state type**, not on an action. It
fires whenever an `R` is produced by a typed call on the instance the handler is
attached to — by this handler, by another handler sharing that instance, or by
another screen entirely. A `RawHandler`'s call on the same instance notifies
nobody: its result is JSON text, not an `R`. The subscriber names *what it renders*, not what somebody else must
call to produce it, so adding an action that also yields an `R` never breaks an
existing subscriber.

- **Scope is the instance, not the model type.** A subscription is matched at
  publish time by comparing the binding's *current* instance, so re-pointing a
  handler ([`attach`](#bridgehandlermodel-sharing)) moves its subscriptions with
  it. A handler with no primary hears only its own results, because nothing else
  is attached to what it holds.
- **One callback per `(handler, R)`.** Subscribing again replaces the previous
  callback; there is no fan-out within a handler.
- **The originating handler is notified too.** Suppressing the echo would force
  every subscriber to special-case "was this mine", which is exactly the
  bookkeeping this replaces.
- **Callbacks run on the handler's executor**, as `.then` does. Two handlers in
  one process with different executors each get their callback where they asked
  for it.
- **Ordering is per instance.** Every action on an instance runs on that
  instance's strand, so notifications are naturally ordered; nothing is
  guaranteed *between* instances.
- **Failed actions notify nobody.** A notification is produced from a successful
  result only.
- **Delivery is best-effort and unbuffered.** There is no replay, no cursor, no
  checkpointing, and no coalescing — a model that emits at high frequency must
  throttle itself. The matching sinks are snapshotted before any is invoked,
  so a subscriber that re-enters the bridge (subscribing, unsubscribing)
  cannot invalidate the iteration.
- **Only copy-constructible results are published.** A result type that cannot
  be copied is delivered to its caller's `Completion` as usual but is never
  boxed for subscribers.
- **Fan-out is per `Bridge`, i.e. per client process.** This is the one place
  subscriptions are narrower than instance sharing: an instance really is shared
  across clients ([shared_instances.md](shared_instances.md)), but a result
  produced by *another client* on that instance does not reach this client's
  subscribers — there is no server-initiated frame, and both transports would
  need an unsolicited-message path to carry one. Two typed handlers in one process
  see each other's work; two clients do not, and must re-read to notice a
  change.


## Thread safety — one owner

`Bridge` and every `BridgeHandler` built on it belong to one executor, the
**owner**, which the `Bridge` constructor takes and which is required:

```cpp
morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), guiExecutor};
```

Both are created, called and destroyed there, and every field either keeps is
touched only there, so neither holds a lock. The backend installed in the
bridge is reached only from there as well, which is why `LocalBackend`,
`SynchronousBackendAdapter` and `SimulatedRemoteBackend` keep their registries,
pending lists and sessions without a lock too (see
[backend.md](backend.md)).

"There" means one of two things, which `exec::detail::OwnerAffinity` checks
([executor.md](executor.md#owner-affinity)): inside a task the owner runs
(`morph::exec::runningOn(owner)`), or on the thread that constructed the
bridge, when that construction ran outside every executor's task. The second
half is the owner's own thread running code outside the owner's tasks — a Qt
application's slots and QML handlers, a test body, a `main()` before its event
loop starts. A scope stated for that code would not do: executor scopes nest
strictly per thread, and QML destroys the objects that own bridges in no
particular order. A bridge constructed inside a task (on a strand, on a pool)
records no thread and is checked by `runningOn` alone. A call from anywhere
else is a contract violation. Debug builds assert it at every public entry
point and at every site that touches owner state; a release build does not
check.

Every public verb is owner-only: construction and destruction,
`registerHandler`, `registerSharedHandler`, `deregisterHandler`, `executeVia`,
`switchBackend`, `setDefaultSession`/`defaultSession`,
`setPrincipal`/`currentPrincipal`, `setExecuteDeadline`/`executeDeadline`,
the subscription verbs, and each `BridgeHandler` verb. Three answers are
atomics and may be read from any thread: `pendingCalls()`, `isBound()` and
`hasSubscribers()`.

What does cross threads, and how it reaches the owner:

| From | What | How it reaches the owner |
|---|---|---|
| A backend's thread (a pool strand, the I/O loop, the Qt thread) | A bind or promote reply | `IBackend::bindModel`/`promoteModel` are called with the owner as their executor, so the continuation is a task on the owner. A backend that settles before returning is applied at once, on the owner, by the call that issued it. |
| A backend's thread | An action's result | The backend settles the call's `detail::BridgeSink` there; the caller's continuations run on the handler's `guiExec`. The bridge-side work a result triggers — subscription fan-out, a result-keyed action's promotion — is posted to the owner. |
| A transport's thread | A reconnect | `IBackend::setReconnectHandler` takes the executor the handler runs on; the bridge installs its owner, so the re-registration is an ordinary task on the owner. |
| A pool thread inside a running action | A `BridgeHandler` constructed there | The constructor posts its registration to the owner and returns. |

`BridgeHandler`'s `guiExec` must be the owner or an executor that runs its
tasks on the owner's thread. The constructor checks it in a debug build by
posting one task to `guiExec` that checks, where it runs, that it is on the owner.

**Teardown is on the owner, handlers before the bridge.** `~BridgeHandler`
deregisters its binding; `~Bridge` rejects every call still waiting for a
bind with `BridgeDestroyedError`, clears the reconnect handler and cancels the
backend's pending calls. A handler whose bridge was destroyed first finds the
bridge's `CallbackToken` expired and deregisters nothing: the check and the
destructor are on one thread, so they cannot interleave.

Subscriptions follow the same rule: `subscribe`/`unsubscribe` and the fan-out
run on the owner, and `detail::SubscriptionRegistry` holds no lock.
`publishResult` still snapshots the matching sinks before invoking them,
because a subscriber may re-enter the registry.

## Lifetime & ownership

**Binding ownership (shared/weak split).** `BridgeHandler` owns the
`shared_ptr<HandlerBinding>`; the `Bridge` holds only a
`weak_ptr<HandlerBinding>` in its `_handlers` list. The binding therefore lives
exactly as long as its handler. The bridge can enumerate live bindings (for
`switchBackend` and reconnect re-registration) and skip dead ones via
`weak.lock()`, but it never keeps a handler alive.

**Teardown order: handlers before the bridge, on the owner.** Both
destructors run on the owner, so they never overlap and need no gate.
`~BridgeHandler` deregisters its binding from the bridge and rejects every
call of its own still waiting for a bind. `~Bridge` rejects the calls still
waiting on any live binding with `BridgeDestroyedError`, clears the active
backend's reconnect handler, and cancels its pending calls.

A handler that outlives its bridge — the reverse order, still on the owner —
checks the bridge's `CallbackToken` (`liveness()`) in its destructor and
deregisters nothing once it has expired. On one thread that check is exact:
the destructor it guards against cannot run between the check and the call.
From any other thread the destructor is a contract violation, asserted in a
debug build.

**The lifetime rule.** The bridge must outlive every *use* of its handlers:
any `execute()`, `executeJson()`, or `FlowSession::set<>`-triggered fire
dereferences the `Bridge&`. Only the destruction of a handler after its bridge
is tolerated, as above.

## API reference

### `ActionExecuteRegistry`

| Member | Signature | Notes |
|---|---|---|
| `instance` | `static ActionExecuteRegistry& instance()` | Process-level singleton. |
| `registerAction` | `template<Model, Action> void registerAction(string_view modelId, string_view actionId)` | Registers an executor that deserializes JSON → `ActionTraits::fromJson`, calls `BridgeHandler<Model, Sharing>::execute<>`, serializes result back. Files **two** entries — one per sharing tag (`NoSharing`, `AllowShared`) — from one generic-lambda template. Defined out-of-line after `BridgeHandler`. |
| `execute` | `template<Sharing> Completion<string> execute(string_view modelId, string_view actionId, void* handler, string_view bodyJson) const` | Lookup + invoke, under the caller's own sharing policy. Key is `(modelId, actionId, typeid(Sharing))`. Throws `runtime_error` on an unknown key. |
| `contains` | `template<Sharing> bool contains(string_view modelId, string_view actionId) const noexcept` | Existence check over the same key `execute` looks up, without invoking anything. Since `registerAction` always files both sharing tags together, this answers the same for either `Sharing` for any action registered via `BRIDGE_REGISTER_ACTION`. Backs `BridgeHandler::servesAction`. |

### `Bridge`

| Member | Signature | Notes |
|---|---|---|
| ctor | `Bridge(unique_ptr<IBackend>, IExecutor& owner)` | `owner` is required: the executor the bridge, its handlers and its backend belong to. Tells the backend its owner (`setOwner`), installs a reconnect handler posted to the owner, then pushes the (initially empty) default session via `setSession`. |
| dtor | `~Bridge()` | On the owner. Clears the active backend's reconnect handler, rejects every call still held for a bind with `BridgeDestroyedError`, then cancels all pending completions with the same error. |
| `registerHandler<Model>` | `shared_ptr<HandlerBinding> registerHandler()` | Default factory. Issues `IBackend::bindModel` with the owner as delivery executor and returns without waiting; see [Registration readiness](#registration-readiness--the-bind-rule). |
| `registerHandler(binding)` | `void registerHandler(const shared_ptr<HandlerBinding>&)` | Pre-built binding. Same bind rule. |
| `switchBackend` | `void switchBackend(unique_ptr<Backend>)` / `void switchBackend(shared_ptr<IBackend>)` | Tells the new backend its owner and pushes the default session, then issues a re-bind for every live binding. Binds settled before returning decide it: a failure releases what was acquired and rethrows with the old backend and every binding untouched; otherwise it commits — publishes settled ids, leaves bindings with a bind in flight holding their calls, swaps, notifies, moves the reconnect handler, and cancels the old backend's pending calls with `BackendChangedError` (stopping running Task handlers). The `unique_ptr` overload is a template on the concrete backend type and delegates to the `shared_ptr` one. |
| `deregisterHandler` | `void deregisterHandler(const shared_ptr<HandlerBinding>&)` | Supersedes a bind in flight (its reply releases its instance), rejects held calls with `HandlerDestroyedError`, deregisters from the active backend (if bound), resets `currentId` to 0, removes from tracking. |
| `executeVia<Model, Action>` | `Completion<R> executeVia(const shared_ptr<HandlerBinding>&, Action, IExecutor*, function<void(const R&)> onResult = {})` | On the owner. Counts the call pending and arms the execute deadline when it is made; dispatches now when no bind is in flight, else holds it until the bind settles (a failed bind rejects it with the bind's error; a never-attached `AllowShared` binding with `"handler not bound"`). Attaches the default session. On `LocalBackend`, rejects an action whose `ActionValidator::ready` returns `false` with `morph::model::ValidationError` via `onError`, before `Model::execute` runs. Records a journal `LogEntry` for loggable actions on both success (`Outcome::Succeeded`) and a throwing `Model::execute` (`Outcome::Failed`, rethrown unchanged); a failure to serialise the result or to append the success entry happens after the mutation committed and rejects the completion with `morph::model::ActionRecordingError` instead. Dispatches through `IBackend::executeInto`, handing the backend a `detail::BridgeSink<R>` that is simultaneously the caller's typed completion state and the backend's settle sink. The bridge-side work a result triggers (`onResult`, the subscription fan-out) runs on the owner, gated on the bridge's `CallbackToken`. A call whose handler returns a `Task` carries a `StopSource`, requested by the deadline, by a `CallbackScope` a callback was attached through, by a `co_await` of its completion that is stopped, by the token given to `BridgeHandler::execute(action, stop)`, and by the backend's `cancelPending`. |
| `bindByType` | `shared_ptr<HandlerBinding> bindByType(string typeId, BindSharing, string instanceKey = {})` | Binds a model by its registered id; see [Model-free dispatch](#model-free-dispatch--rawhandler). Throws `invalid_argument` for a private binding with a key, `logic_error` for a key given off the owner. |
| `executeRaw` | `Completion<string> executeRaw(const shared_ptr<HandlerBinding>&, string actionId, string bodyJson, IExecutor*)` | Dispatches by action id with a JSON body; resolves with the reply's JSON text. Session, deadline, `pendingCalls()`, held-while-binding and `cancelPending` as `executeVia`; not published to typed subscribers. |
| `executeRaw` (cancellable) | `Completion<string> executeRaw(…, IExecutor*, core::async::StopToken stop)` | As above, with the caller's cancel, as `BridgeHandler::execute(action, stop)`. |
| `executeRawOn` | `Completion<string> executeRawOn(const shared_ptr<HandlerBinding>&, string instanceKey, string actionId, string bodyJson, IExecutor*)` | `executeRaw` on the instance for `instanceKey`, attaching first; a refused or superseded attach rejects the call. Throws `invalid_argument` for an empty key, `logic_error` for a private binding. |
| `attach` | `void attach(const shared_ptr<HandlerBinding>&, string primary)` | Attaches or re-points a shared binding by canonical key string, issued after any bind in flight. Throws `invalid_argument` for an empty key, `logic_error` for a private binding. `attachHandler<Model>` skips the private-binding check and refuses an empty key without throwing. |
| `setDefaultSession` | `void setDefaultSession(session::Context)` | Installs default session context; also pushes it to the active backend via `IBackend::setSession` so control envelopes (register/attach/assign/deregister) carry it too, not only `execute`. |
| `defaultSession` | `session::Context defaultSession() const` | Returns snapshot of default session. |
| `setExecuteDeadline` | `void setExecuteDeadline(std::chrono::milliseconds)` | Opt-in client-side execute deadline; `0` (the default) disables it. Lazily creates the backing `TimeoutScheduler`, on a private `exec::IoLoop` with one thread, on first enable. |
| `executeDeadline` | `std::chrono::milliseconds executeDeadline() const` | Returns the installed deadline; `0` when disabled. |
| `setPrincipal` | `void setPrincipal(session::Principal)` | Installs the verified `Principal`, readable outside a dispatch. Pass `Principal{}` to clear (sign-out). |
| `currentPrincipal` | `session::Principal currentPrincipal() const` | Returns a snapshot of the installed `Principal`; default-constructed if none was ever set. |
| `isBound` | `[[nodiscard]] static bool isBound(const shared_ptr<HandlerBinding>&) noexcept` | Atomic `currentId != 0`, safe from any thread. Point-in-time snapshot; see [Registration readiness](#registration-readiness--the-bind-rule). |
| `pendingCalls` | `[[nodiscard]] size_t pendingCalls() const noexcept` | Count of typed and raw calls not yet resolved, held calls included. Relaxed atomic load, safe from any thread; see the `Bridge` section above. |

### `BridgeHandler<Model>`

| Member | Signature | Notes |
|---|---|---|
| ctor (default) | `BridgeHandler(Bridge&, IExecutor*)` | Registers via the bridge's bind rule: at once on the owner, posted to the owner from anywhere else. Never waits. |
| ctor (custom binding) | `BridgeHandler(Bridge&, IExecutor*, shared_ptr<HandlerBinding>)` | Registers the pre-built binding the same way. |
| dtor | `~BridgeHandler()` | On the owner. Deregisters via `Bridge::deregisterHandler` (rejecting held calls with `HandlerDestroyedError`), but only if the bridge's `CallbackToken` is still active; a no-op if the `Bridge` was already destroyed. |
| `execute<Action>` | `Completion<R> execute(Action)` | Typed dispatch through the bridge; held until the handler's bind settles when one is in flight. For a shared handler, a payload-/result-keyed action's attach or promote step never throws synchronously — a backend refusal (e.g. `LimitPolicy::maxLiveModels`) resolves the returned `Completion` via `.onError(...)`. |
| `execute<Action>` (cancellable) | `Completion<R> execute(Action, core::async::StopToken stop)` | As `execute(Action)`, with a caller's cancel. A stop on `stop` before the call settles rejects the `Completion` with `core::async::OperationCancelled` — posted from the thread that requested the stop — and requests stop on the call's `StopSource`, so a Task handler on a `LocalBackend` sees it; a synchronous handler runs to its end, and a remote server's handler runs on, since no cancel crosses the wire. The call stays pending until the backend settles it, as for a fired deadline. A token already stopped rejects the call without dispatching it. See the cancellation policy in [concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md#cancellation-policy). |
| `executeJson` | `Completion<string> executeJson(string_view actionType, string_view bodyJson)` | Type-erased dispatch through `ActionExecuteRegistry`. |
| `subscribe<R>(cb)` | `void subscribe(function<void(R)>)` | Fire `cb` whenever an `R` is produced on the attached instance. |
| `subscribe<R>(scope, cb)` | `void subscribe(CallbackScope const&, function<void(R)>)` | As above, gated on the scope's liveness and stop state ([callback_scope.md](callback_scope.md)). Dead sinks are refused, not pruned. |
| `subscribe<R>(token, cb)` | `void subscribe(CallbackToken, function<void(R)>)` | Token-taking form of the above. |
| `unsubscribe<R>` | `void unsubscribe()` | Drops this handler's callback for `R`. |
| `attach(key)` | `void attach(const PrimaryKeyOf<Model>&)` | Attaches/re-points a shared handler: the same posted bind, issued after any bind in flight. Never throws; a refusal is logged and leaves the handler on the instance it held (a call held behind it is rejected with the refusal only when there is none). |
| `primary()` | `optional<PrimaryKeyOf<Model>> primary()` | The handler's current primary, or empty. |
| `instances()` | `Completion<vector<PrimaryKeyOf<Model>>> instances()` | Snapshot of live shared keys, asked of the backend through `Bridge::instancesOf` (`IBackend::instances`) and delivered on the handler's `guiExec`. |
| `isBound` | `[[nodiscard]] bool isBound() const noexcept` | Forwards to `Bridge::isBound(binding())`. `false` while the first bind is in flight. |
| `guiExecutor` | `IExecutor* guiExecutor() const noexcept` | Returns the callback executor. |
| `binding` | `const shared_ptr<HandlerBinding>& binding() const` | Returns the underlying binding. |

### `RawHandler`

| Member | Signature | Notes |
|---|---|---|
| ctor | `RawHandler(Bridge&, IExecutor*, string typeId, BindSharing = Private, string instanceKey = {})` | Binds through `Bridge::bindByType`. |
| dtor | `~RawHandler()` | As `~BridgeHandler`: deregisters on the owner while the bridge's `CallbackToken` is active, a no-op after the bridge is gone. |
| `execute` | `Completion<string> execute(string actionId, string bodyJson)` | `Bridge::executeRaw` on this handler's binding. |
| `execute` (cancellable) | `Completion<string> execute(string actionId, string bodyJson, core::async::StopToken)` | The cancellable `executeRaw`. |
| `executeOn` | `Completion<string> executeOn(string instanceKey, string actionId, string bodyJson)` | `Bridge::executeRawOn`: the way to send a keyed action. |
| `attach` | `void attach(string instanceKey)` | `Bridge::attach`: issued, not awaited. |
| `primary` | `string primary() const` | The attached key, or empty before the first attach settles. |
| `isBound` | `bool isBound() const noexcept` | `Bridge::isBound` on the binding. |
| `typeId` | `const string& typeId() const noexcept` | The id given at construction. |

`BindSharing` is `Private` or `Shared`: an enum, so a call site reads as what it
means.

### `HandlerBinding`

| Field | Type | Notes |
|---|---|---|
| `typeId` | `string` | `ModelTraits<Model>::typeId()`, or the id given to `bindByType`. |
| `modelFactory` | `function<unique_ptr<IModelHolder>()>` | Factory for re-registration on backend switch. |
| `contextKey` | `string` | Stable identity for remote backends (optional, empty by default). |
| `currentId` | `atomic<uint64_t>` | Backend-assigned model id; 0 = unbound. The one field read off the owner, by `isBound()`. |
| `primary` / `shared` | `string` / `bool` | Shared-instance key and policy; see [shared_instances.md](shared_instances.md). |
| `bindInFlight` | `bool` | A bind issued for this binding has not settled. Owner-only. |
| `bindGeneration` | `uint64_t` | Counts binds issued; a reply carrying an older count is superseded and releases its instance. Owner-only. |
| `bindFailure` | `exception_ptr` | The last bind's failure; what a held call is rejected with. Owner-only. |
| `afterBind` | `function<void(exception_ptr)>` | The issuing operation's continuation, run before `waiting`. Owner-only. |
| `waiting` | `deque<function<void(exception_ptr)>>` | Calls held until no bind is in flight, resumed in order with null to proceed or an error to reject. Owner-only. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Binding storage | **`vector<weak_ptr<HandlerBinding>>`** | `Bridge` does not own the bindings — the handler (`BridgeHandler` or `RawHandler`) holds the `shared_ptr`. Weak references let `switchBackend` and the reconnect handler skip dead bindings without keeping handlers alive. |
| Threading | **One owner, given at construction; every verb owner-only, asserted in debug builds** | Every consumer already called its `Bridge` from one thread, and the free-threaded contract cost a lock per runtime-settable field, a lifetime gate for teardown on any thread, and a registration handoff for replies that settle on a thread the bridge does not own. Handing the owner to `bindModel`/`promoteModel` as the delivery executor, and having backends post reconnects to it, makes every one of those a task on the owner instead. Teardown follows: handlers before the bridge, on the owner. |
| Registration | **Asynchronous, with no policy: the bind rule** | A handler can be constructed inside a running action, off the owner, so its registration has to be posted; once it can be, every bind can be a `Completion` delivered on the owner. A backend that settles before returning keeps the synchronous feel — a `LocalBackend` handler is bound when its constructor returns — and the caller never has to gate a first dispatch on readiness, because `execute` is held until the bind settles. |
| Reconnect handler | **Posted to the owner; liveness guard + weak-backend guard + stale check; cleared in `~Bridge`** | `IBackend::setReconnectHandler` takes the executor the handler is posted to, and the bridge passes its owner, so the re-bind is an ordinary owner task. The handler checks the bridge's `CallbackToken`, then that the backend that reconnected is still the active one; a reconnect of a backend switched away from is ignored. `~Bridge` and `switchBackend` clear the outgoing backend's handler. |
| Subscription keying | **On the result type, and against the binding rather than an instance id** | A subscriber is a renderer: it cares about the state it draws, not about which of several actions produced it, so a new action yielding the same type never breaks it. Storing against the binding makes a subscription follow a re-pointed handler, which is what "tell me about the account I am looking at" requires. |
| Action readiness | **`ActionValidator<Action>::ready(snapshot)`** | Framework-agnostic validation — each action struct defines its own required-field semantics. The bridge never interprets action fields. |
| Local-path validation enforcement | **`localOp` checks `ActionValidator<Action>::ready` before `Model::execute`** | Closes the gap where an `Action` built by hand and dispatched via `BridgeHandler::execute<Action>()` (without a client-side gate) reached the model unvalidated; mirrors `ActionDispatcher::registerAction`'s server-side runner (`registry.md`). Backward compatible: `ready()` defaults to `true` for actions with no validator. |
| Subscription keys | **`string_view` into static storage** | `ActionTraits::typeId()` returns `constexpr` string literals with static duration. The `unordered_map` holds non-owning keys; no allocation, no lifetime issues. |
| `executeJson` | **Separate registry, not a vtable** | The action type is unknown at the call site. A flat `unordered_map` keyed on registered ids lets any translation unit register its actions without central registration or RTTI. |
| Executor keying | **`(modelId, actionId, typeid(Sharing))` — one executor per sharing policy** | The executor is the only place a typed handler is recovered from a `void*`, and `BridgeHandler<M, NoSharing>` / `BridgeHandler<M, AllowShared>` are unrelated types. A single `NoSharing`-only executor handed a shared handler would `static_cast` to the wrong instantiation, so its `kShared` — which gates a payload-/result-keyed action's attach-or-promote step — would answer for the wrong one and that step would silently never run. No runtime type information survives to check it by then, so the key carries the distinction instead: two entries per action, built from one generic-lambda template so they cannot diverge, and `execute` templated on `Sharing` so each call site selects its own. Costs one closure per registered action at static-init time, not per call. |
| `registerActionExecutorOnce` | **`inline` definition in header** | The function is forward-declared in `registry.hpp` (`morph::model::detail`) but defined `inline` in `bridge.hpp`, after `ActionExecuteRegistry`. `inline` lets that definition be instantiated in every TU that transitively includes `bridge.hpp` without an ODR/link violation. The registration runs from the anonymous-namespace initializer the macro emits. Because the definition lives only in `bridge.hpp`, any TU expanding `BRIDGE_REGISTER_ACTION` must include it (directly or transitively) or the link fails with an unresolved symbol. |
| `pendingCalls()` counter placement | **One `std::atomic<size_t>` on `Bridge`, not per-`HandlerBinding` or per-backend** | Client-side quiescence — "has everything settled" — is a property of the `Bridge` as a whole (the thing the GUI actually holds one of), not of any single handler or backend. A per-binding counter would force a caller wanting a global "still loading" signal to sum across every live `BridgeHandler`; a per-backend counter (mirroring `LocalBackend::_inFlight`/`RemoteServer`'s `_inFlight`, both server/backend-side) would miss calls dispatched before a `switchBackend()` mid-flight. Counting where the call's sink is built (`makeSinkFor`, reached through `makeSink` from `executeVia`, `executeAttachedVia` and `executeCreatingVia`, and directly from `executeRaw` and `executeRawOn` — the chokepoints every dispatch path, `execute<Action>`, `executeJson` and a `RawHandler`'s calls included, funnels through) and releasing it as the sink settles needs no cooperation from `IBackend` implementations at all. |

## Limitations

- **Backend switch interrupts in-flight fielded edits.** When `switchBackend`
  commits, it cancels the old backend's pending completions with
  `BackendChangedError`. A flow step whose `FlowSession::set<>`-triggered
  flight is still in the air will therefore reach that session's `onError`
  callback mid-edit (or be logged, if the session was constructed without
  one). The draft lives in `FlowSession`'s own `std::tuple<Steps...>` and so
  survives the switch, letting the next `set<>` re-fire cleanly against the
  new backend, but the interrupted flight itself surfaces as an error rather
  than being transparently retried.
- **`ActionExecuteRegistry::execute` trusts its `void* handler`.** The
  type-erased entry point takes a `void*` that each registered executor
  `static_cast`s back to `BridgeHandler<Model, Sharing>*` for the model type
  and sharing policy it was registered under. Passing a handler whose model
  type does not match the `modelId`, or whose sharing policy does not match the
  `Sharing` template argument, is undefined behaviour — there is no runtime
  type check on either half. In practice `BridgeHandler::executeJson` always
  passes `this` with a matching `ModelTraits<Model>::typeId()` *and* its own
  `Sharing`, so the invariant holds by construction; the hazard only exists for
  callers that invoke the registry directly.
- **The sharing half of the key is a closed set of two by convention, not by
  construction.** `registerAction` enumerates `NoSharing` and `AllowShared`
  explicitly, but `BridgeHandler`'s `Sharing` parameter is unconstrained:
  nothing rejects `BridgeHandler<M, MyOwnTag>` at compile time. Such a handler
  behaves as `NoSharing` everywhere (`kShared` is
  `is_same_v<Sharing, AllowShared>`) *except* `executeJson`, which throws
  "unknown action for executeJson" for an action that *is* registered, because
  no executor was ever filed under its `type_index`. A concept constraining
  `Sharing` to the two tags would make that a compile error instead; none
  exists today.

## Lifetime annotations

`BridgeHandler`'s constructors mark `Bridge& bridge` and `IExecutor* guiExec`
`MORPH_LIFETIMEBOUND` (`morph/attributes.hpp`), and `binding()` marks its implicit
object parameter.

The `Bridge&` annotation is a deliberate over-approximation. It states "must
outlive", while [Lifetime & ownership](#lifetime--ownership) above says the bridge
must outlive all *use* and that mis-ordered *destruction* is defined behaviour.
`[[clang::lifetimebound]]` has no way to carve destruction out, so a bridge
destroyed before a live handler — legal, and still asserted by
`tests/test_switch_backend.cpp` — reads to Clang as a use-after-scope. The
carve-out remains part of the contract; the attribute simply cannot say it. See
[concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md#morph_lifetimebound--the-must-outlive-rules-told-to-the-compiler).

## Cross-references

- [`coroutines.md`](coroutines.md) — action handlers that return
  `core::async::Task<R>`: how `executeVia`'s `localOpAsync` drives them on the
  model's strand, the per-instance action gate, and how an execute deadline
  stops a suspended handler.
- [`backend.md`](backend.md) — `IBackend`, `LocalBackend`,
  `SimulatedRemoteBackend`, `registerModelWithContext`, `cancelPending`,
  `BackendChangedError`/`BridgeDestroyedError`, reconnect handlers.
- [`callback_scope.md`](callback_scope.md) — `CallbackScope`/`CallbackToken`,
  the primitive behind `Bridge::_callbacks` / `liveness()` and the gated
  `subscribe<R>(scope, cb)` overload.
- [`session.md`](../session/session.md) — `session::Context` attached to every
  `executeVia` call via the default session.
- [`security.md`](../security.md) — how the session principal drives
  authorization on the execute path.
- [`wire.md`](wire.md) — the `register` envelope carrying `contextKey` and the
  action call/result serialization used by remote backends.
- [`completion.md`](completion.md) — `Completion<T>`/`CompletionState<T>`
  semantics, executor marshalling, and idempotent value/exception setting.
- [`registry.md`](registry.md) — `ModelTraits`, `ActionTraits`,
  `ActionValidator`, `Loggable`, `BRIDGE_REGISTER_ACTION`, and the server-side
  `ActionDispatcher` counterpart.
- [`../forms/workflows_navigation.md`](../forms/workflows_navigation.md) —
  `FlowSession`, the typed sequencer that spans a wizard's ordered steps over
  this type's ordinary `execute<Action>` path — sequencing, not a new dispatch
  path.
- [`concurrency_and_lifetimes.md`](../concurrency_and_lifetimes.md) — the broader
  mutex-ordering and object-lifetime rules this type participates in.
- [`forms.md`](../forms/forms.md) — `morph::forms::computed`/`computeList`/`recomputeAll`,
  the computed-field declaration this spec's dispatch paths recompute.