# Bridge types — `Bridge`, `BridgeHandler`, `ActionExecuteRegistry`

`morph::bridge` owns the client-side dispatch path: a `Bridge` holds an active
`IBackend` and a set of registered model bindings; a `BridgeHandler<Model>` is
an RAII handle that registers one model instance and exposes typed
`execute()`, field-by-field `set()`, and subscription APIs. A
`ActionExecuteRegistry` provides dynamic (string-keyed) dispatch for call sites
that only know action names at runtime.

## Contents

- [Architecture overview](#architecture-overview)
- [`HandlerBinding`](#handlerbinding)
- [`Bridge`](#bridge)
- [`BridgeHandler<Model>`](#bridgehandlermodel)
- [Registration readiness — `isBound()` / `whenBound()`](#registration-readiness--isbound--whenbound)
- [`ActionExecuteRegistry`](#actionexecuteregistry)
  - [Why the key carries the sharing policy](#why-the-key-carries-the-sharing-policy)
- [`BRIDGE_REGISTER_ACTION` and `registerActionExecutorOnce`](#bridge_register_action-and-registeractionexecutoronce)
- [`MemberPointerTraits`](#memberpointertraits)
- [Subscription semantics](#subscription-semantics)
- [Thread safety](#thread-safety)
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
    // … shared-instance fields (`shared`, `primary`) — see shared_instances.md
    std::atomic<uint64_t> currentId{0};

    // Registration-settled seam — see "Registration readiness" below.
    std::mutex registrationMtx;
    bool registrationInFlight = false;
    std::vector<std::pair<std::function<void(bool)>,
                          std::function<void(std::exception_ptr)>>> registrationWaiters;
};
```

One record per registered model instance. `typeId` is the string
`ModelTraits<Model>::typeId()`. `modelFactory` creates a fresh
`IModelHolder` — used by `switchBackend()` to re-register on the new
backend. `contextKey` is an optional stable identity (e.g. account id) that
travels in the `register` wire envelope for remote backends. `currentId` is
the `ModelId` value the active backend assigned; 0 = unbound.

The last three fields are the state behind
[`isBound()` / `whenBound()`](#registration-readiness--isbound--whenbound).
`registrationInFlight` is `true` from the moment `registerHandlerImpl` hands
the binding's initial registration to `IBackend::registerModelAsync` (and that
call returns `true`) until the resulting `onRegistered`/`onError` callback
resolves; `registrationWaiters` holds the callbacks queued while it is.
Both are guarded by `registrationMtx` — deliberately a mutex of the binding's
own, not `Bridge::_mtx` or `_attachMtx`, because a waiter may be queued or
resolved from either the registering thread or the backend's reply-delivering
thread and must never block on the bridge's own locks.

## `Bridge`

Central dispatcher. Non-copyable, non-movable. Thread-safe.

**Construction** takes ownership of an `IBackend` and installs a reconnect
handler so backends with recoverable transports (e.g. `QtWebSocketBackend`)
re-register all live bindings on reconnection.

**`registerHandler<Model>()`** creates a `HandlerBinding` with the default
`ModelFactory::create<Model>()` factory and registers it on the active
backend. Returns the `shared_ptr<HandlerBinding>`. An overload accepts a
pre-built binding (for dependency injection, custom `contextKey`, or custom
factory captures). Both funnel through a shared `registerHandlerImpl`, which
prefers the backend's `IBackend::registerModelAsync` when it offers one (see
`backend.md`, "Asynchronous registration") and falls back to the synchronous
`registerModelWithContext` otherwise — the returned binding may therefore come
back **unbound** (`currentId == 0`) if the backend registered asynchronously
and the reply has not arrived yet.

**`executeVia<Model, Action>(binding, action, cbExec)`** dispatches one
action. Takes a short snapshot of the backend `shared_ptr` under the dedicated
`_backendMtx` (never `_mtx`) plus a lock-free atomic read of the binding's
`currentId`, so it never blocks on `switchBackend()`'s `_mtx`. If `currentId` is 0,
completes immediately with `"handler not bound"`. Constructs an `ActionCall`
with serialization/deserialization lambdas and a `localOp` that, on
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
**`localOp` is compiled — and so needs `Model::execute`'s definition to
link — every time `executeVia<Model, Action>` is instantiated, regardless of
which backend ends up installed at runtime.** Only `LocalBackend::execute`
ever actually calls `call.localOp`; every remote backend ignores it entirely.
But the closure itself is still compiled into the instantiation, so a build
that only ever installs a remote backend still forces the linker to resolve
`Model::execute` — see `registry.md`, "`MORPH_CLIENT_ONLY`". When
`MORPH_CLIENT_ONLY` is defined, `localOp`'s body is replaced with a
`std::logic_error` throw instead of the block described below, so nothing in
the compiled program references `Model::execute`'s definition. Reaching that
throw at runtime means `LocalBackend` was used in a build that promised never
to — a configuration error, not a normal failure mode.

Otherwise (the default, non-`MORPH_CLIENT_ONLY` build), `Model::execute(*action)`
itself is wrapped in a `try`/`catch (const std::exception&)`: on success it
records a journal `LogEntry` with `outcome = Outcome::Succeeded` for loggable
actions; on a throw it records `outcome = Outcome::Failed` (`error =
exc.what()`, `result` empty) for the same actions and rethrows unchanged, so
the exception still resolves the `Completion` through `onError` exactly as
before — the journal entry is a side effect of the attempt, not a change to
error propagation. Mirrors `ActionDispatcher::registerAction`'s runner
(`registry.md`) for remote topologies. See [journal.md,
"Outcome"](../journal/journal.md#logentry--one-recorded-action-execution) for
the full field/replay semantics.

The typed result is unwrapped from `std::shared_ptr<void>`
into the final `Completion<R>` inside a `try`/`catch`: moving the result out of
the opaque `shared_ptr<void>` can throw (a throwing move/copy on `R`, or a bad
cast), and if that exception escaped the `.then` callback it would be swallowed
by a `ThreadPoolExecutor` — leaving the typed `Completion` unresolved (a silent
hang) — or reach `QCoreApplication::exec` under `QtExecutor` and
`std::terminate`. On a caught exception the forwarding routes it to the typed
completion's error sink via `setException`, so the caller's `.onError(...)`
fires instead. This mirrors the identical forwarding guard in
`SimulatedRemoteBackend::execute` (remote.hpp). The type-erased `executeJson`
path (`ActionExecuteRegistry::registerAction`) guards its own `resultToJson`
forwarding the same way.

Before any of that forwarding, the same `.then` closure checks the bridge's
`CallbackToken` (captured as `alive = liveness()`) and gates every
bridge-touching side effect on it being active — checked first, before
`onResult` or `hasSubscribers()` run. A backend completion can in principle
resolve after the `Bridge` is gone: the backend may be co-owned and outlive
this `Bridge`, or the callback may already be running when `~Bridge()` runs
concurrently on another thread. `onResult` — used for a result-keyed action to
call back into the bridge via a captured raw pointer and adopt the binding's
primary — and `hasSubscribers()` — which reads `this` — must never run once
the bridge might be gone; running either on a dangling `Bridge` is a
use-after-free. The typed result is still delivered to the caller's own
`Completion` either way (`typedState->setValue` does not touch the bridge) —
only the two bridge-touching side effects are skipped when the token has
expired.

**`switchBackend(newBackend)`** replaces the active backend atomically: the
switch either fully succeeds or leaves everything exactly as it was. It has
two overloads:

- `switchBackend(shared_ptr<IBackend>)` — the caller keeps its own reference,
  so the same backend instance can be re-installed later (e.g. switching back
  to a long-lived remote backend, with its live socket and reconnect state,
  after a temporary fallback to a local one) instead of reconstructing it.
- `switchBackend(unique_ptr<Backend>)` — transfers ownership, as before.
  Templated on the concrete `Backend` type (rather than taking
  `unique_ptr<IBackend>` directly) so that a call like
  `switchBackend(std::make_unique<LocalBackend>(...))` is an *exact* match
  and is preferred over the `shared_ptr<IBackend>` overload; a non-template
  `unique_ptr<IBackend>` overload would tie with it (both are one
  equally-ranked user-defined conversion from `unique_ptr<Backend>`), making
  every existing call site ambiguous. It converts to a `shared_ptr` and
  delegates to the overload above.

Before either phase, the current default session is pushed onto the new
backend via `newBackend->setSession(...)` (read under `_sessionMtx` alone,
never held while calling into the backend) — so every control envelope phase
1 builds while re-registering handlers on the new backend already carries the
session, exactly as it would on the backend that is being replaced. See
`IBackend::setSession` and [backend.md](backend.md#session-propagation-to-control-envelopes).

Both phases below run under `_mtx`:

- **Phase 1 — stage, do not mutate.** Every live binding is registered on the
  new backend and the resulting `(binding, newId)` pairs are collected into a
  staging list. No `currentId` is touched and the old backend is still active,
  so concurrent `executeVia()` calls continue to hit the old backend. If any
  `registerModelWithContext` throws (a plausible remote/transport failure),
  the already-staged registrations are rolled back by calling
  `deregisterModel` on the new backend for each staged id (rollback-deregister
  failures are logged, not propagated), and the original exception is
  rethrown. On this path the old backend and every `currentId` are untouched —
  the call is a no-op.
- **Phase 2 — commit.** Only reached when *all* registrations succeeded. The
  staged ids are published into each `binding->currentId`, the handler list is
  replaced with the surviving-bindings list, the backend pointer is swapped via
  `exchangeBackend`, and `notifyBackendChanged()` fires.

After the swap the reconnect handler is re-installed on the new backend and
cleared on the old one, and the old backend's pending completions are cancelled
with `BackendChangedError` — outside `_mtx`, because `cancelPending` delivers
callbacks through user executors and the bridge never runs user code while
holding its mutex. Lock ordering: `_mtx` is acquired before the backend's
internal mutex.

**`onBackendChanged()` runs on the model's strand, not inline under `_mtx`.**
`notifyBackendChanged()` fires while `_mtx` is held, but for `LocalBackend` it
does not *call* each model's `onBackendChanged()` — it **posts** it onto that
model's own strand (the same per-`ModelId` serial queue `execute` uses) and
returns. Consequences:

- **Strand-serialised, lock-free.** The callback body runs single-threaded per
  model, never overlapping an `execute()` on the same model, so a model
  reconciling state there (e.g. draining a shared `IOfflineQueue`) needs no
  locking of its own fields. It runs *after* `switchBackend` returns, so an
  observer must wait for it rather than assume it ran synchronously.
- **`registerHandler()` / `deregisterHandler()` are now safe from
  `onBackendChanged()`.** Because the body runs on a pool thread with `_mtx`
  free, a re-entrant `registerHandler`/`deregisterHandler` acquires `_mtx`
  freshly instead of self-deadlocking on the switch caller's held lock (the
  earlier inline-under-`_mtx` design deadlocked here — this was the fix).
- **`switchBackend()` from `onBackendChanged()` is still unsupported** — but for
  a different reason than the lock. The callback runs on the *outgoing* backend's
  strand; a nested `switchBackend` would release the last reference to that
  backend when it returns, and `~StrandExecutor` blocks until its in-flight tasks
  finish — including the very task calling it — a self-join hang. Re-registering
  models or reconciling queue state is the supported reaction; swapping the
  backend again from inside the notification is not.

**`deregisterHandler(binding)`** calls `deregisterModel` on the active backend
(only if the binding still has a non-zero `currentId`), then resets
`binding->currentId` to the `0 = unbound` sentinel and removes the binding from
tracking. Resetting to `0` means a late or concurrent `executeVia()` on a
deregistered binding fails fast on the "handler not bound" guard rather than
sending a now-destroyed `ModelId` to the backend.

**`setDefaultSession(session)`** / **`defaultSession()`** installs a default
`morph::session::Context` that is attached to every `executeVia()` call.
Thread-safe, separate mutex from `_mtx`. `setDefaultSession` also pushes the
new session to the active backend via `IBackend::setSession` (copied out from
under `_sessionMtx` before the call, never while holding it), so every
control envelope (`register`/`registerShared`/`attach`/`assign`/`deregister`)
the backend subsequently builds carries the session too — not only `execute`
envelopes. The constructor does the same with the (typically empty) initial
session. See [session.md](../session/session.md#how-a-context-originates-and-flows)
and [backend.md](backend.md#session-propagation-to-control-envelopes).

**`setExecuteDeadline(deadline)`** / **`executeDeadline()`** installs an
opt-in, client-side wall-clock bound on how long any subsequent `executeVia()`
waits for a reply. Defaults to `std::chrono::milliseconds{0}` (disabled — the
pre-existing behavior, and no extra thread). When enabled, each dispatch races
the real reply against a `morph::async::detail::TimeoutScheduler` timer that
resolves the pending `Completion` with `morph::backend::ClientTimeoutError`;
whichever settles first wins, and the loser is discarded by
`CompletionState`'s first-result-wins rule. The on-time reply path disarms the
timer as the first statement of its completion callback. Thread-safe, its own
mutex (`_executeDeadlineMtx`). Full semantics — including how
`ClientTimeoutError` differs from the server-reported `TimeoutError` — in
[completion.md](completion.md#client-side-execute-deadline).

**`setPrincipal(principal)`** / **`currentPrincipal()`** installs and reads
back a `morph::session::Principal` — the verified identity + roles, readable
*outside* a dispatch (unlike `session::current()`, which only exists during
one), so UI code can gate itself (`bridge.currentPrincipal().hasRole("editor")`)
instead of attempting an action and catching the refusal. Thread-safe, its own
mutex (`_principalMtx`, separate from both `_mtx` and the session mutex).
Scoped to this `Bridge` instance, not a process-wide global — see
[session.md](../session/session.md#principal--readable-authorization-state-outside-a-dispatch)
for the full rationale and trust model. Purely a client-side convenience: it
has no wire representation and does not affect dispatch or `Context` in any
way — every dispatch is still authorized server-side via `IAuthorizer`
regardless of what `currentPrincipal()` says.

**`pendingCalls()`** returns the number of actions dispatched via
`executeVia()` (and so, transitively, `BridgeHandler::execute()`) that have
not yet resolved — a client-side quiescence signal for building a "still
loading" indicator or gating a feature on "has everything settled" without
hand-rolling a counter around every call site. Backed by a single
`std::atomic<std::size_t> _pendingCalls`, incremented once per call right
before the backend dispatch inside `executeVia()` and decremented exactly
once by whichever of the two mutually-exclusive resolution continuations
(the `.then`/`.onError` `executeVia()` attaches to the backend's own
completion) actually fires — success, error, or a client-visible failure
delivered through either path (e.g. a cancelled/backend-changed/disconnected
completion, all of which resolve through `.onError`). The synchronous
"handler not bound" early return in `executeVia()` never increments the
counter in the first place (it resolves before any dispatch), so it needs no
matching decrement. Both continuations are additionally guarded by the same
`liveness()` token every other bridge-touching side effect in `executeVia()`
uses: a completion resolving after `~Bridge()` skips the decrement (touching
`this` on a dangling `Bridge` would be a use-after-free) rather than running
it. A single relaxed atomic load/increment/decrement — cheap enough to poll
every UI frame. See [Design decisions](#design-decisions) for why the counter
lives at the `Bridge` layer rather than per-`HandlerBinding` or per-backend.

**Destructor** first **clears the active backend's reconnect handler**
(`setReconnectHandler(nullptr)`), then cancels every pending completion on that
backend with `BridgeDestroyedError`. In-flight replies that arrive after
destruction are no-ops (`CompletionState::setValue`/`setException` is
idempotent). Clearing the reconnect handler matters because the installed
handler captures `this`: a backend co-owned elsewhere can outlive the `Bridge`,
and a reconnect firing afterward would otherwise dereference the freed `Bridge`.
The clear happens outside `_mtx` — `setReconnectHandler` only stores a callback
and never re-enters the bridge, so there is no lock-ordering hazard. This closes
the common case; the handler additionally guards on a `_callbacks` token (see
below) so a reconnect already latched on the transport thread when the `Bridge`
is destroyed is also a safe no-op.

**`liveness()`** (private, exposed only to `BridgeHandler` via friendship)
returns a `morph::async::CallbackToken` issued from the bridge's `_callbacks`
member — a `morph::async::CallbackScope` created with the bridge and destroyed
with it. Each `BridgeHandler` captures this weak token at construction and
consults it in its destructor so that destroying the `Bridge` before its
handlers is a safe no-op rather than a use-after-free. See
[Lifetime & ownership](#lifetime--ownership).

The bridge is the framework's own first consumer of the primitive every caller
now gets (see [callback_scope.md](callback_scope.md)); it uses only the liveness
half — it never calls `requestStop()`, so its tokens go inactive only when the
`Bridge` is destroyed.

## `BridgeHandler<Model>`

RAII handle. Registers a `HandlerBinding` on construction, deregisters on
destruction. Non-copyable.

**Construction** takes a `Bridge&` and a GUI executor. Optionally accepts a
pre-built `HandlerBinding` (for dependency injection). It captures the bridge's
`liveness()` weak token into `_bridgeAlive`, holds a strong `Bridge&`, and
and carries the sharing policy as its second template argument (see
`BridgeHandler<Model, Sharing>` below).

**Destruction** deregisters the binding via `Bridge::deregisterHandler` — but
only if `_bridgeAlive.active()` still reports `true`. If the `Bridge` was
already destroyed the token is inactive, so the destructor skips deregistration
instead of dereferencing a dangling `Bridge&`. Destroying the bridge before
its handlers is still discouraged (see [Lifetime & ownership](#lifetime--ownership)),
but it is now defined behaviour, not a use-after-free.

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
the reactive `set<>` path rather than trusting the raw wire body:

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
instance, so it follows the handler when it re-points. Delivers the result
to the registered `sink` callback. If a flight is already in progress, marks
`pending = true` and refires when the current flight completes
(debounce-like coalescence). On failure, the registered `errSink` is invoked;
if none is registered, the error is logged via `morph::log::logError`

**`guiExecutor()`** returns the executor passed at construction.

## Registration readiness — `isBound()` / `whenBound()`

A handler built over a backend that offers `registerModelAsync` comes back
**unbound**: `registerHandlerImpl` returns as soon as the request is sent, and
`currentId` stays `0` until the reply arrives (`backend.md`,
["Asynchronous registration"](backend.md#asynchronous-registration--registermodelasync)).
`executeVia` fails fast with `"handler not bound"` for anything dispatched in
that window. The window is unavoidable — it is a network round trip — so the
contract this pair provides is not that it can be closed, but that a caller can
**observe** it without reaching into the framework's internals.

That is the point of these two being public API. `HandlerBinding` is an
internal-linkage record ([`HandlerBinding`](#handlerbinding) above): the state
that answers "is this handler usable yet" lives in a struct callers are not
supposed to name, so before these seams existed the only ways to answer it were
to poll `binding->currentId` directly — which is what the WASM spike and the
testkit did — or to guess with a timer. Both couple caller code to a field
whose synchronisation is the bridge's business.

**`isBound()`** is the synchronous, point-in-time answer: a lock-free atomic
read of `currentId != 0`. It is a *snapshot*, not a guarantee — the binding can
become bound (or, via `deregisterHandler`, unbound) the instant after it
returns. Correct uses are ones where a stale answer is harmless: a poll from a
UI timer, an assertion in a test, a readiness gate that will be re-consulted.
`Bridge::isBound(binding)` is the free-function form; `BridgeHandler::isBound()`
forwards to it.

**`whenBound()`** is the awaitable counterpart, and exists so that a caller
wanting to dispatch the moment registration settles does not have to invent a
polling loop for it. It returns `Completion<bool>`, delivered on the handler's
GUI executor like every other completion the class hands out, and resolves on
exactly one of three paths:

| Binding state when called | Resolution |
|---|---|
| Already bound | `true`, immediately (before the function returns). |
| An async registration is in flight | `true` once `onRegistered` fires; the registration's failure via `.onError(...)` if `onError` fires instead. |
| Nothing in flight and still unbound | `false`, immediately. |

The third row is the one worth stating explicitly, because "false" and "an
error" are different answers to different questions. `false` means *there is
nothing to wait for* — no async registration was ever started for this binding,
so the completion resolves rather than hanging forever on a reply that is not
coming. It is reachable in two ways: a `shared` binding (`registerSharedHandler`
files the binding but dispatches nothing, since a shared registration waits for
a primary), and the narrow window before `registerHandlerImpl` has run at all,
which the pre-built-binding `registerHandler` overload exposes by handing the
caller the `shared_ptr` first. An ordinary non-shared handler is always either
bound or mid-registration by the time anyone can observe it, so it never sees
`false`.

Three ordering rules make the above hold, and each exists because the obvious
implementation would be wrong:

- **`registrationInFlight` is set *before* the backend call, not after.** No
  backend documented here invokes `onRegistered` synchronously from inside
  `registerModelAsync`, but one could; setting the flag afterwards would leave
  a window in which the registration has already resolved while a concurrent
  `whenBound()` still reads "nothing in flight" and answers `false`.
- **`whenBound()` re-checks `isBound()` under `registrationMtx` after its
  lock-free check.** The resolving callback binds the id and settles the
  waiters as two steps; a caller landing between them would otherwise queue a
  waiter onto a list that has already been drained, and wait forever.
- **The synchronous fallback settles waiters too.** When `registerModelAsync`
  returns `false` and `registerHandlerImpl` falls back to
  `registerModelWithContext`, it routes through the same
  `resolveRegistrationWaiters` rather than clearing the flag directly — a
  `whenBound()` call that raced into the window while the flag was still `true`
  has a real `Completion` outstanding, and nothing else would ever settle it.

Waiters are settled **exactly once**, by whichever callback resolves first: the
resolver swaps the waiter list out under `registrationMtx`, clears
`registrationInFlight`, and invokes the callbacks outside the lock. This covers
the stale-reply case too — a success callback whose id is discarded (because a
`switchBackend()` already moved past this registration, or because the `Bridge`
itself is gone) still settles the waiters, since the binding's *initial
registration attempt* has finished either way and nothing further is coming to
settle them. A discarded reply settles waiters through the **failure** arm, and
the only failure it has to report is the absence of a result — so the resolver
substitutes an exception saying exactly that, naming the binding's `typeId`.
Treat a `whenBound()` error as "registration did not complete".

That substitution is not cosmetic. This path previously passed a **null**
`exception_ptr` straight through, which settled the completion `ready` with
`error` still falsy — a state `attachOnError` cannot act on, so an
`.onError(...)` attached after the settlement was silently discarded and the
completion never resolved for that caller, in either direction. `whenBound()`
resolves through the executor even in `Local` mode, so "attach after settle" is
an ordinary interleaving rather than an exotic race. `CompletionState` now
refuses to settle on a null at all
([completion.md](completion.md#setting-a-value-or-exception)); this site
supplies the specific message because the meaning of *this* failure is known
here and nowhere else. See issue #347.

Scope limits worth knowing, because each is a question `whenBound()` looks like
it answers and does not:

- **It tracks the initial registration only.** `registerHandlerImpl` is the
  sole writer of `registrationInFlight`. Re-registration by `switchBackend()`
  and by the reconnect handler is synchronous and never sets it, so
  `whenBound()` says nothing about a backend swap or a reconnect in progress.
- **It does not track the shared attach path.** `attachHandler`/`ensureBound`
  and their async counterparts bind a shared handler without going through
  `registerHandlerImpl`, so for an `AllowShared` handler `whenBound()` is only
  ever `isBound()` in awaitable clothing: `true` if the attach has already
  landed, `false` immediately if it has not, never a wait. Gate a shared
  handler on the `Completion` its own keyed `execute()` returns, which does
  carry the attach ([shared_instances.md](shared_instances.md)).
- **`true` means bound, not reachable.** It reports that the backend assigned
  this binding an id, not that the transport is still up; the socket can drop
  the moment after.


## `ActionExecuteRegistry`

Process-level singleton (`instance()`). Maps a **three-part key** —
`(modelTypeId, actionTypeId, typeid(Sharing))` — to `Executor` values
(`std::function<Completion<string>(void*, string_view)>`). The backing store is
`unordered_map<Key, Executor, KeyHash>`, where `Key` is
`{string modelId; string actionId; std::type_index sharing;}` and `KeyHash`
mixes the `type_index`'s `hash_code()` into the `morph::model::detail::PairKeyHash`
the server-side `ActionDispatcher` uses over the two strings alone. Populated by
`registerActionExecutorOnce<Model, Action>()`, which
`BRIDGE_REGISTER_ACTION` calls during static initialization.

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
fires whenever an `R` is produced on the instance the handler is attached to —
by this handler, by another handler sharing that instance, or by another screen
entirely. The subscriber names *what it renders*, not what somebody else must
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
  throttle itself. Sinks are snapshotted under the registry lock and invoked
  outside it, so a subscriber that re-enters the bridge cannot deadlock.
- **Only copy-constructible results are published.** A result type that cannot
  be copied is delivered to its caller's `Completion` as usual but is never
  boxed for subscribers.
- **Fan-out is per `Bridge`, i.e. per client process.** This is the one place
  subscriptions are narrower than instance sharing: an instance really is shared
  across clients ([shared_instances.md](shared_instances.md)), but a result
  produced by *another client* on that instance does not reach this client's
  subscribers — there is no server-initiated frame, and both transports would
  need an unsolicited-message path to carry one. Two handlers in one process see
  each other's work; two clients do not, and must re-read to notice a change.


## Thread safety

`Bridge` is fully thread-safe (see the `Bridge` section: separate
`_backendMtx`, `_mtx`, `_sessionMtx`, and `_attachMtx`, with `executeVia`
taking its backend snapshot under the short, dedicated `_backendMtx` rather
than `_mtx`, and a shared handler's `attachHandler`/`ensureBound`/
`assignHandlerPrimary` running under `_attachMtx` rather than `_mtx`, so a
slow remote round-trip on one handler's attach never blocks another
handler's construction, destruction, or a `switchBackend()` call on the same
`Bridge`).

`attachHandlerAsync`/`ensureBoundAsync` — the non-blocking counterparts
`BridgeHandler::execute()` routes its keyed dispatches through — take
`_attachMtx` over the same scope their synchronous twins do, but **release it
before invoking their `onDone` callback**, on every path including the
synchronous fallback. That is a hard requirement, not a style choice:
`onDone` is where the action itself is dispatched, and a result-keyed dispatch
promotes its binding via `assignHandlerPrimary`, which re-takes `_attachMtx`.
It is the same rule `registerHandlerImpl` already follows for `_mtx`. See
[shared_instances.md](shared_instances.md), "Async register-or-attach and
attach".

The guarantee is unconditional, including for a backend that completes its
`attachModelAsync`/`registerModelSharedAsync` callback **inline** — from inside
the dispatch call itself, while the dispatching frame still holds `_attachMtx`
(`QtWebSocketBackend` does exactly this on its `!_connected` error branch).
Such a callback does not act: it parks its outcome in a
`detail::AsyncDispatchHandoff` and returns, and the dispatching frame applies
the outcome once its own dispatch call has returned — publishing under the lock
it already holds, then releasing it, then calling `onDone`. A tiny mutex inside
the handoff makes the window race-free even against a backend that replies from
another thread while its dispatch call is still on this stack, and keeps
`onDone` invoked exactly once on every interleaving. Because the inline case can
never reach the callback body, `attachHandlerAsync`'s out-of-frame success
callback is free to re-acquire `_attachMtx` for the two `std::string` fields it
publishes (`HandlerBinding::contextKey`/`primary`, which every other reader
takes that lock for); `ensureBoundAsync`'s publishes only the atomic
`currentId` and needs no lock at all.

`whenBound()` synchronises on the *binding's* `registrationMtx`, never on a
`Bridge` mutex, and never holds it across a callback: the resolver swaps the
waiter list out under the lock and invokes the callbacks after releasing it.
That is what lets a waiter be queued from a GUI thread and settled from a
backend's transport thread without either blocking on the bridge's own locks —
see [Registration readiness](#registration-readiness--isbound--whenbound).

`subscribe`/`unsubscribe` mutate the bridge's subscription registry under
`_subMtx`. Callbacks never run under that mutex: `publishResult` snapshots the
matching sinks under the lock and invokes them outside it, marshalled to the
`guiExec` executor passed at construction, so a subscriber that re-enters the
bridge cannot deadlock. A subscription holds a `weak_ptr` to its binding, so one
belonging to a destroyed handler is skipped and pruned rather than dangling. The
intended usage remains **single-GUI-thread affinity**: a handler and its
subscriptions belong to one GUI thread.

## Lifetime & ownership

**Binding ownership (shared/weak split).** `BridgeHandler` owns the
`shared_ptr<HandlerBinding>`; the `Bridge` holds only a
`weak_ptr<HandlerBinding>` in its `_handlers` list. The binding therefore lives
exactly as long as its handler. The bridge can enumerate live bindings (for
`switchBackend` and reconnect re-registration) and skip dead ones via
`weak.lock()`, but it never keeps a handler alive.

**Bridge-vs-handler teardown order.** The bridge holds a
`morph::async::CallbackScope _callbacks`; every handler captures a matching
`morph::async::CallbackToken` (`_bridgeAlive`) at construction. This makes
*teardown* order-independent:

- Handler destroyed first (the normal case): its destructor deregisters the
  binding from the still-live bridge.
- Bridge destroyed first: the handler's `_bridgeAlive` token is no longer
  active, so its destructor skips deregistration — a safe no-op instead of a
  use-after-free.

**The lifetime rule.** The liveness token makes only *destruction* safe in
either order. It does **not** make a `Bridge` optional for live handlers: any
`execute()`, `executeJson()`, or `set<>`-triggered fire dereferences the
`Bridge&` and must run while the bridge is alive. In other words, the bridge
must still outlive all normal use of its handlers; only mis-ordered
*destruction* is now defined behaviour. (This corrects an earlier claim that
"weak references let `BridgeHandler` outlive `Bridge`" — they do not; they only
make teardown order-independent.)

## API reference

### `ActionExecuteRegistry`

| Member | Signature | Notes |
|---|---|---|
| `instance` | `static ActionExecuteRegistry& instance()` | Process-level singleton. |
| `registerAction` | `template<Model, Action> void registerAction(string_view modelId, string_view actionId)` | Registers an executor that deserializes JSON → `ActionTraits::fromJson`, calls `BridgeHandler<Model, Sharing>::execute<>`, serializes result back. Files **two** entries — one per sharing tag (`NoSharing`, `AllowShared`) — from one generic-lambda template. Defined out-of-line after `BridgeHandler`. |
| `execute` | `template<Sharing> Completion<string> execute(string_view modelId, string_view actionId, void* handler, string_view bodyJson) const` | Lookup + invoke, under the caller's own sharing policy. Key is `(modelId, actionId, typeid(Sharing))`. Throws `runtime_error` on an unknown key. |

### `Bridge`

| Member | Signature | Notes |
|---|---|---|
| ctor | `explicit Bridge(unique_ptr<IBackend>)` | Installs reconnect handler on the backend, then pushes the (initially empty) default session via `setSession`. |
| dtor | `~Bridge()` | Clears the active backend's reconnect handler, then cancels all pending completions with `BridgeDestroyedError`. |
| `registerHandler<Model>` | `shared_ptr<HandlerBinding> registerHandler()` | Default factory. Prefers `IBackend::registerModelAsync`; see `backend.md`. |
| `registerHandler(binding)` | `void registerHandler(const shared_ptr<HandlerBinding>&)` | Pre-built binding. Same async-preferring behavior. |
| `switchBackend` | `void switchBackend(unique_ptr<IBackend>)` / `void switchBackend(shared_ptr<IBackend>)` | Pushes the current default session onto the new backend via `setSession` before staging. Atomic: stages all re-registrations on the new backend, commits (publishes new ids + swaps) only if all succeed, else rolls back and rethrows leaving old backend + `currentId`s intact. Cancels old backend's pending ops with `BackendChangedError`. Holds both `_mtx` and `_attachMtx` for its duration. The `unique_ptr` overload is a template on the concrete backend type and delegates to the `shared_ptr` one — see below. |
| `deregisterHandler` | `void deregisterHandler(const shared_ptr<HandlerBinding>&)` | Deregisters from active backend (if bound), resets `currentId` to 0, removes from tracking. |
| `executeVia<Model, Action>` | `Completion<R> executeVia(const shared_ptr<HandlerBinding>&, Action, IExecutor*)` | Lock-free dispatch. Attaches default session. On `LocalBackend`, rejects an action whose `ActionValidator::ready` returns `false` with `morph::model::ValidationError` via `onError`, before `Model::execute` runs. Records a journal `LogEntry` for loggable actions on both success (`Outcome::Succeeded`) and a throwing `Model::execute` (`Outcome::Failed`, rethrown unchanged). Value-forwarding into the typed `Completion` is `try`/`catch`-guarded — a throwing result move/copy resolves the completion via `onError` instead of hanging or terminating. The bridge-touching side effects (`onResult`, `hasSubscribers()`/`publishResult`, the `pendingCalls()` decrement, and the execute-deadline disarm) are gated on the bridge's `CallbackToken`, checked before any runs, so a completion resolving after `~Bridge()` skips them instead of touching the dangling `Bridge`. Increments `pendingCalls()` once per call before dispatch (never for the synchronous "handler not bound" early return); decrements it exactly once, from whichever of the two mutually-exclusive resolution continuations actually fires. Arms the client-side execute deadline when one is installed (see `setExecuteDeadline`); the fast-fail "handler not bound" path returns before that and arms nothing. |
| `setDefaultSession` | `void setDefaultSession(session::Context)` | Installs default session context; also pushes it to the active backend via `IBackend::setSession` so control envelopes (register/attach/assign/deregister) carry it too, not only `execute`. |
| `defaultSession` | `session::Context defaultSession() const` | Returns snapshot of default session. |
| `setExecuteDeadline` | `void setExecuteDeadline(std::chrono::milliseconds)` | Opt-in client-side execute deadline; `0` (the default) disables it. Lazily creates the backing `TimeoutScheduler` thread on first enable. |
| `executeDeadline` | `std::chrono::milliseconds executeDeadline() const` | Returns the installed deadline; `0` when disabled. |
| `setPrincipal` | `void setPrincipal(session::Principal)` | Installs the verified `Principal`, readable outside a dispatch. Pass `Principal{}` to clear (sign-out). |
| `currentPrincipal` | `session::Principal currentPrincipal() const` | Returns a snapshot of the installed `Principal`; default-constructed if none was ever set. |
| `isBound` | `[[nodiscard]] static bool isBound(const shared_ptr<HandlerBinding>&) noexcept` | Lock-free `currentId != 0`. Point-in-time snapshot; see [Registration readiness](#registration-readiness--isbound--whenbound). |
| `whenBound` | `[[nodiscard]] Completion<bool> whenBound(const shared_ptr<HandlerBinding>&, IExecutor* cbExec)` | Resolves `true` once the binding's initial registration settles, `false` immediately when nothing is in flight, or the registration's error via `onError`. Delivered on `cbExec`. Waiters settle exactly once. |
| `pendingCalls` | `[[nodiscard]] size_t pendingCalls() const noexcept` | Count of `executeVia()` dispatches not yet resolved. Relaxed atomic load; see the `Bridge` section above. |

### `BridgeHandler<Model>`

| Member | Signature | Notes |
|---|---|---|
| ctor (default) | `BridgeHandler(Bridge&, IExecutor*)` | Registers via `Bridge::registerHandler<Model>()`. |
| ctor (custom binding) | `BridgeHandler(Bridge&, IExecutor*, shared_ptr<HandlerBinding>)` | Registers pre-built binding. |
| dtor | `~BridgeHandler()` | Deregisters via `Bridge::deregisterHandler`, but only if the bridge's `CallbackToken` is still active; a no-op if the `Bridge` was already destroyed. |
| `execute<Action>` | `Completion<R> execute(Action)` | Typed dispatch through the bridge. For a shared handler, a payload-/result-keyed action's attach or promote step never throws synchronously — a backend refusal (e.g. `LimitPolicy::maxLiveModels`) resolves the returned `Completion` via `.onError(...)`. |
| `executeJson` | `Completion<string> executeJson(string_view actionType, string_view bodyJson)` | Type-erased dispatch through `ActionExecuteRegistry`. |
| `subscribe<R>(cb)` | `void subscribe(function<void(R)>)` | Fire `cb` whenever an `R` is produced on the attached instance. |
| `subscribe<R>(scope, cb)` | `void subscribe(CallbackScope const&, function<void(R)>)` | As above, gated on the scope's liveness and stop state ([callback_scope.md](callback_scope.md)). Dead sinks are refused, not pruned. |
| `subscribe<R>(token, cb)` | `void subscribe(CallbackToken, function<void(R)>)` | Token-taking form of the above. |
| `unsubscribe<R>` | `void unsubscribe()` | Drops this handler's callback for `R`. |
| `attach(key)` | `void attach(const PrimaryKeyOf<Model>&)` | Attaches/re-points a shared handler. |
| `primary()` | `optional<PrimaryKeyOf<Model>> primary()` | The handler's current primary, or empty. |
| `instances()` | `Completion<vector<PrimaryKeyOf<Model>>> instances()` | Snapshot of live shared keys. |
| `isBound` | `[[nodiscard]] bool isBound() const noexcept` | Forwards to `Bridge::isBound(binding())`. `false` while an async registration is still outstanding. |
| `whenBound` | `[[nodiscard]] Completion<bool> whenBound()` | Forwards to `Bridge::whenBound(binding(), guiExecutor())`. The supported way to gate a first dispatch on registration settling — see [Registration readiness](#registration-readiness--isbound--whenbound). |
| `guiExecutor` | `IExecutor* guiExecutor() const noexcept` | Returns the callback executor. |
| `binding` | `const shared_ptr<HandlerBinding>& binding() const` | Returns the underlying binding. |

### `HandlerBinding`

| Field | Type | Notes |
|---|---|---|
| `typeId` | `string` | `ModelTraits<Model>::typeId()`. |
| `modelFactory` | `function<unique_ptr<IModelHolder>()>` | Factory for re-registration on backend switch. |
| `contextKey` | `string` | Stable identity for remote backends (optional, empty by default). |
| `currentId` | `atomic<uint64_t>` | Backend-assigned model id; 0 = unbound. Read lock-free by `isBound()`. |
| `registrationMtx` | `mutex` | Guards the two fields below. The binding's own lock, not `Bridge::_mtx`/`_attachMtx`: it is taken from the backend's reply-delivering thread as well as the registering one. |
| `registrationInFlight` | `bool` | `true` from just before `registerModelAsync` is called until its callback settles. The synchronous fallback path never leaves it set. |
| `registrationWaiters` | `vector<pair<function<void(bool)>, function<void(exception_ptr)>>>` | `whenBound()` callbacks queued while a registration is in flight; invoked and cleared exactly once, by the same call that clears `registrationInFlight`. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Binding storage | **`vector<weak_ptr<HandlerBinding>>`** | `Bridge` does not own the bindings — `BridgeHandler` holds the `shared_ptr`. Weak references let `switchBackend` and the reconnect handler skip dead bindings without keeping handlers alive. (Handler *teardown* after the bridge is made safe separately, by the `_callbacks` scope's tokens — not by this weak storage.) |
| Teardown order | **`morph::async::CallbackScope _callbacks` + per-handler `CallbackToken`** | Makes bridge-vs-handler destruction order-independent: a handler outliving its bridge skips deregistration instead of dereferencing a dangling `Bridge&`. Normal `execute`/`subscribe` still require the bridge to outlive its handlers. Was a hand-rolled `shared_ptr<const void>`/`weak_ptr<const void>` pair; it is now the shared primitive ([callback_scope.md](callback_scope.md)) so the framework does not reimplement what it asks callers to use. |
| Backend pointer | **Short snapshot under the dedicated `_backendMtx`** | `executeVia()` reads the backend through a `loadBackend()` helper that copies the `shared_ptr` under `_backendMtx` (never `_mtx`), so it never blocks on `switchBackend()`'s `_mtx`. |
| Session storage | **Separate `_sessionMtx` from `_mtx`** | Session access is a hot path (every `executeVia` reads it). A separate mutex avoids contention with handler registration/switchBackend. |
| Attach-path locking | **Separate `_attachMtx` from `_mtx`** | `attachHandler`/`ensureBound`/`assignHandlerPrimary` can block on a full network round-trip for a remote backend. A dedicated mutex means that round-trip never blocks unrelated `registerHandler`/`deregisterHandler`/`switchBackend` calls on the same `Bridge`, closing a deadlock hazard if the thread expected to deliver the pending reply itself needs `_mtx`. `HandlerBinding::primary`/`contextKey` are mutated only under `_attachMtx`; `switchBackend()` and the reconnect handler, which also touch them, take both mutexes together. |
| Reconnect handler | **Liveness guard + weak‑backend guard + stale check; cleared in `~Bridge`** | The lambda captures a `CallbackToken` from `_callbacks` and a `weak_ptr<IBackend>`. On invocation it first checks the token — if the `Bridge` is gone it returns without touching `this` (no use-after-free). It then checks `pinned == loadBackend()` — if a switch occurred since the handler was installed, the reconnect is ignored. `~Bridge` and `switchBackend` also clear the outgoing backend's handler via `setReconnectHandler(nullptr)`; the liveness guard covers a reconnect already in flight when teardown races it. |
| Subscription keying | **On the result type, and against the binding rather than an instance id** | A subscriber is a renderer: it cares about the state it draws, not about which of several actions produced it, so a new action yielding the same type never breaks it. Storing against the binding makes a subscription follow a re-pointed handler, which is what "tell me about the account I am looking at" requires. |
| Action readiness | **`ActionValidator<Action>::ready(snapshot)`** | Framework-agnostic validation — each action struct defines its own required-field semantics. The bridge never interprets action fields. |
| Local-path validation enforcement | **`localOp` checks `ActionValidator<Action>::ready` before `Model::execute`** | Closes the gap where an `Action` built by hand and dispatched via `BridgeHandler::execute<Action>()` (without a client-side gate) reached the model unvalidated; mirrors `ActionDispatcher::registerAction`'s server-side runner (`registry.md`). Backward compatible: `ready()` defaults to `true` for actions with no validator. |
| Subscription keys | **`string_view` into static storage** | `ActionTraits::typeId()` returns `constexpr` string literals with static duration. The `unordered_map` holds non-owning keys; no allocation, no lifetime issues. |
| `executeJson` | **Separate registry, not a vtable** | The action type is unknown at the call site. A flat `unordered_map` keyed on registered ids lets any translation unit register its actions without central registration or RTTI. |
| Executor keying | **`(modelId, actionId, typeid(Sharing))` — one executor per sharing policy** | The executor is the only place a typed handler is recovered from a `void*`, and `BridgeHandler<M, NoSharing>` / `BridgeHandler<M, AllowShared>` are unrelated types. A single `NoSharing`-only executor handed a shared handler would `static_cast` to the wrong instantiation, so its `kShared` — which gates a payload-/result-keyed action's attach-or-promote step — would answer for the wrong one and that step would silently never run. No runtime type information survives to check it by then, so the key carries the distinction instead: two entries per action, built from one generic-lambda template so they cannot diverge, and `execute` templated on `Sharing` so each call site selects its own. Costs one closure per registered action at static-init time, not per call. |
| `registerActionExecutorOnce` | **`inline` definition in header** | The function is forward-declared in `registry.hpp` (`morph::model::detail`) but defined `inline` in `bridge.hpp`, after `ActionExecuteRegistry`. `inline` lets that definition be instantiated in every TU that transitively includes `bridge.hpp` without an ODR/link violation. The registration runs from the anonymous-namespace initializer the macro emits. Because the definition lives only in `bridge.hpp`, any TU expanding `BRIDGE_REGISTER_ACTION` must include it (directly or transitively) or the link fails with an unresolved symbol. |
| `pendingCalls()` counter placement | **One `std::atomic<size_t>` on `Bridge`, not per-`HandlerBinding` or per-backend** | Issue #45 asks for client-side quiescence — "has everything settled" — which is a property of the `Bridge` as a whole (the thing the GUI actually holds one of), not of any single handler or backend. A per-binding counter would force a caller wanting a global "still loading" signal to sum across every live `BridgeHandler`; a per-backend counter (mirroring `LocalBackend::_inFlight`/`RemoteServer`'s `_inFlightExecutes`, both server/backend-side) would miss calls dispatched before a `switchBackend()` mid-flight. Incrementing/decrementing directly in `executeVia()` — the one chokepoint every dispatch path (`execute<Action>`, `executeJson`) funnels through — needs no cooperation from `IBackend` implementations at all. |

## Limitations

- **Backend switch interrupts in-flight fielded edits.** When `switchBackend`
  commits, it cancels the old backend's pending completions with
  `BackendChangedError`. A fielded subscriber whose `set<>`-triggered flight is
  still in the air will therefore receive `BackendChangedError` on its
  `errSink` mid-edit (or see it logged if no `errSink` is registered). The
  client-side draft survives the switch, so the next `set<>` re-fires cleanly
  against the new backend, but the interrupted flight itself surfaces as an
  error rather than being transparently retried.
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
  `FlowSession`, the typed sequencer that extends the per-action reactive
  draft (`subscribe`/`set<>`/`reset`, draft persistence across fires and
  backend switches) to span a wizard's ordered steps, with no new dispatch
  path.
- [`concurrency_and_lifetimes.md`](../concurrency_and_lifetimes.md) — the broader
  mutex-ordering and object-lifetime rules this type participates in.
- [`forms.md`](../forms/forms.md) — `morph::forms::computed`/`computeList`/`recomputeAll`,
  the computed-field declaration this spec's reactive and dispatch paths recompute.