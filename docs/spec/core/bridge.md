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
- [`ActionExecuteRegistry`](#actionexecuteregistry)
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
    std::atomic<uint64_t> currentId{0};
};
```

One record per registered model instance. `typeId` is the string
`ModelTraits<Model>::typeId()`. `modelFactory` creates a fresh
`IModelHolder` — used by `switchBackend()` to re-register on the new
backend. `contextKey` is an optional stable identity (e.g. account id) that
travels in the `register` wire envelope for remote backends. `currentId` is
the `ModelId` value the active backend assigned; 0 = unbound.

## `Bridge`

Central dispatcher. Non-copyable, non-movable. Thread-safe.

**Construction** takes ownership of an `IBackend` and installs a reconnect
handler so backends with recoverable transports (e.g. `QtWebSocketBackend`)
re-register all live bindings on reconnection.

**`registerHandler<Model>()`** creates a `HandlerBinding` with the default
`ModelFactory::create<Model>()` factory and registers it on the active
backend. Returns the `shared_ptr<HandlerBinding>`. An overload accepts a
pre-built binding (for dependency injection, custom `contextKey`, or custom
factory captures).

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
`_liveness` token (captured as `alive = liveness()`) and gates every
bridge-touching side effect on it being unexpired — checked first, before
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

Both run the same two phases under `_mtx`:

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
Thread-safe, separate mutex from `_mtx`.

**Destructor** first **clears the active backend's reconnect handler**
(`setReconnectHandler(nullptr)`), then cancels every pending completion on that
backend with `BridgeDestroyedError`. In-flight replies that arrive after
destruction are no-ops (`CompletionState::setValue`/`setException` is
idempotent). Clearing the reconnect handler matters because the installed
handler captures `this`: a backend co-owned elsewhere can outlive the `Bridge`,
and a reconnect firing afterward would otherwise dereference the freed `Bridge`.
The clear happens outside `_mtx` — `setReconnectHandler` only stores a callback
and never re-enters the bridge, so there is no lock-ordering hazard. This closes
the common case; the handler additionally guards on the `_liveness` token (see
below) so a reconnect already latched on the transport thread when the `Bridge`
is destroyed is also a safe no-op.

**`liveness()`** (private, exposed only to `BridgeHandler` via friendship)
returns a `std::weak_ptr<const void>` observing the bridge's `_liveness`
member — a `shared_ptr<const void>` that is created with the bridge and
destroyed with it. Each `BridgeHandler` captures this weak token at
construction and consults it in its destructor so that destroying the `Bridge`
before its handlers is a safe no-op rather than a use-after-free. See
[Lifetime & ownership](#lifetime--ownership).

## `BridgeHandler<Model>`

RAII handle. Registers a `HandlerBinding` on construction, deregisters on
destruction. Non-copyable.

**Construction** takes a `Bridge&` and a GUI executor. Optionally accepts a
pre-built `HandlerBinding` (for dependency injection). It captures the bridge's
`liveness()` weak token into `_bridgeAlive`, holds a strong `Bridge&`, and
and carries the sharing policy as its second template argument (see
`BridgeHandler<Model, Sharing>` below).

**Destruction** deregisters the binding via `Bridge::deregisterHandler` — but
only if `_bridgeAlive.lock()` still succeeds. If the `Bridge` was already
destroyed the token has expired, so the destructor skips deregistration
instead of dereferencing a dangling `Bridge&`. Destroying the bridge before
its handlers is still discouraged (see [Lifetime & ownership](#lifetime--ownership)),
but it is now defined behaviour, not a use-after-free.

**`execute<Action>(action)`** delegates to `Bridge::executeVia` with the
handler's binding and GUI executor. Default session is attached
automatically by the bridge.

**`executeJson(actionType, bodyJson)`** type-erased variant. Looks up the
executor in `ActionExecuteRegistry::instance()` and dispatches. The
registry's executor deserializes the JSON body via
`ActionTraits<Action>::fromJson`, then — before invoking the handler —
**reconciles Quantity precision, overwrites any declared computed fields
(`morph::forms::recomputeAll`, [forms.md](../forms/forms.md)), and enforces
the action's validator** (see below), calls `execute<Action>`, and serializes
the result back to JSON. Throws `std::runtime_error` if the action was never
registered.

Between decode and dispatch the registry executor applies three
normalisations, in order, so the request/reply path matches the schema and
the reactive `set<>` path rather than trusting the raw wire body:

- **Declared-precision reconciliation.** `morph::forms::reconcileDeclaredPrecision`
  retags every `Quantity` field of the decoded action to its *declared* precision
  (`Quantity<U, Dec>::declaredDecimals`), so the stored value's precision matches
  the schema's advertised `x-decimalPlaces` instead of whatever runtime `dp` the
  client sent. It is a no-op for actions with no `Quantity` members and for
  actions whose type glaze cannot reflect. See [forms.md](../forms/forms.md).
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

**`subscribe<R>(cb)`** registers `cb` against this handler's binding. The
subscription is matched at publish time by comparing the binding's current
instance, so it follows the handler when it re-points. Delivers the result
to the registered `sink` callback. If a flight is already in progress, marks
`pending = true` and refires when the current flight completes
(debounce-like coalescence). On failure, the registered `errSink` is invoked;
if none is registered, the error is logged via `morph::log::logError`

**`guiExecutor()`** returns the executor passed at construction.

## `ActionExecuteRegistry`

Process-level singleton (`instance()`). Maps `(modelTypeId, actionTypeId)`
pairs to `Executor` values (`std::function<Completion<string>(void*,
string_view)>`). The backing store is
`unordered_map<pair<string,string>, Executor, morph::model::detail::PairKeyHash>` —
the same `PairKeyHash` the server-side `ActionDispatcher` uses to key its
`(modelId, actionId)` map. Populated by
`registerActionExecutorOnce<Model, Action>()`, which
`BRIDGE_REGISTER_ACTION` calls during static initialization.

**`execute(modelId, actionId, handler, bodyJson)`** looks up the executor and
invokes it. The executor `static_cast`s the `void*` back to
`BridgeHandler<Model>*`, deserializes the JSON body, reconciles Quantity fields
to their declared precision, enforces `ActionValidator<Action>::ready` (throwing
`std::invalid_argument` on failure), calls `handler->execute<Action>()`, and
serializes the result back to JSON. Throws `std::runtime_error` if no executor is
registered for the pair.

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
`shared_ptr<const void> _liveness`; every handler captures a matching
`weak_ptr` (`_bridgeAlive`) at construction. This makes *teardown*
order-independent:

- Handler destroyed first (the normal case): its destructor deregisters the
  binding from the still-live bridge.
- Bridge destroyed first: the handler's `_bridgeAlive` token has expired, so
  its destructor skips deregistration — a safe no-op instead of a use-after-free.

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
| `registerAction` | `template<Model, Action> void registerAction(string_view modelId, string_view actionId)` | Registers an executor that deserializes JSON → `ActionTraits::fromJson`, calls `BridgeHandler<Model>::execute<>`, serializes result back. Defined out-of-line after `BridgeHandler`. |
| `execute` | `Completion<string> execute(string_view modelId, string_view actionId, void* handler, string_view bodyJson) const` | Lookup + invoke. Throws `runtime_error` on unknown pair. |

### `Bridge`

| Member | Signature | Notes |
|---|---|---|
| ctor | `explicit Bridge(unique_ptr<IBackend>)` | Installs reconnect handler on the backend. |
| dtor | `~Bridge()` | Clears the active backend's reconnect handler, then cancels all pending completions with `BridgeDestroyedError`. |
| `registerHandler<Model>` | `shared_ptr<HandlerBinding> registerHandler()` | Default factory. |
| `registerHandler(binding)` | `void registerHandler(const shared_ptr<HandlerBinding>&)` | Pre-built binding. |
| `switchBackend` | `void switchBackend(unique_ptr<IBackend>)` / `void switchBackend(shared_ptr<IBackend>)` | Atomic: stages all re-registrations on the new backend, commits (publishes new ids + swaps) only if all succeed, else rolls back and rethrows leaving old backend + `currentId`s intact. Cancels old backend's pending ops with `BackendChangedError`. Holds both `_mtx` and `_attachMtx` for its duration. The `unique_ptr` overload is a template on the concrete backend type and delegates to the `shared_ptr` one — see below. |
| `deregisterHandler` | `void deregisterHandler(const shared_ptr<HandlerBinding>&)` | Deregisters from active backend (if bound), resets `currentId` to 0, removes from tracking. |
| `executeVia<Model, Action>` | `Completion<R> executeVia(const shared_ptr<HandlerBinding>&, Action, IExecutor*)` | Lock-free dispatch. Attaches default session. On `LocalBackend`, rejects an action whose `ActionValidator::ready` returns `false` with `morph::model::ValidationError` via `onError`, before `Model::execute` runs. Records journal for loggable actions. Value-forwarding into the typed `Completion` is `try`/`catch`-guarded — a throwing result move/copy resolves the completion via `onError` instead of hanging or terminating. The bridge-touching side effects (`onResult`, `hasSubscribers()`/`publishResult`) are gated on the `_liveness` token, checked before either runs, so a completion resolving after `~Bridge()` skips them instead of touching the dangling `Bridge`. |
| `setDefaultSession` | `void setDefaultSession(session::Context)` | Installs default session context. |
| `defaultSession` | `session::Context defaultSession() const` | Returns snapshot of default session. |

### `BridgeHandler<Model>`

| Member | Signature | Notes |
|---|---|---|
| ctor (default) | `BridgeHandler(Bridge&, IExecutor*)` | Registers via `Bridge::registerHandler<Model>()`. |
| ctor (custom binding) | `BridgeHandler(Bridge&, IExecutor*, shared_ptr<HandlerBinding>)` | Registers pre-built binding. |
| dtor | `~BridgeHandler()` | Deregisters via `Bridge::deregisterHandler`, but only if the bridge's liveness token is still alive; a no-op if the `Bridge` was already destroyed. |
| `execute<Action>` | `Completion<R> execute(Action)` | Typed dispatch through the bridge. For a shared handler, a payload-/result-keyed action's attach or promote step never throws synchronously — a backend refusal (e.g. `LimitPolicy::maxLiveModels`) resolves the returned `Completion` via `.onError(...)`. |
| `executeJson` | `Completion<string> executeJson(string_view actionType, string_view bodyJson)` | Type-erased dispatch through `ActionExecuteRegistry`. |
| `subscribe<R>(cb)` | `void subscribe(function<void(R)>)` | Fire `cb` whenever an `R` is produced on the attached instance. |
| `unsubscribe<R>` | `void unsubscribe()` | Drops this handler's callback for `R`. |
| `attach(key)` | `void attach(const PrimaryKeyOf<Model>&)` | Attaches/re-points a shared handler. |
| `primary()` | `optional<PrimaryKeyOf<Model>> primary()` | The handler's current primary, or empty. |
| `instances()` | `Completion<vector<PrimaryKeyOf<Model>>> instances()` | Snapshot of live shared keys. |
| `guiExecutor` | `IExecutor* guiExecutor() const noexcept` | Returns the callback executor. |
| `binding` | `const shared_ptr<HandlerBinding>& binding() const` | Returns the underlying binding. |

### `HandlerBinding`

| Field | Type | Notes |
|---|---|---|
| `typeId` | `string` | `ModelTraits<Model>::typeId()`. |
| `modelFactory` | `function<unique_ptr<IModelHolder>()>` | Factory for re-registration on backend switch. |
| `contextKey` | `string` | Stable identity for remote backends (optional, empty by default). |
| `currentId` | `atomic<uint64_t>` | Backend-assigned model id; 0 = unbound. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Binding storage | **`vector<weak_ptr<HandlerBinding>>`** | `Bridge` does not own the bindings — `BridgeHandler` holds the `shared_ptr`. Weak references let `switchBackend` and the reconnect handler skip dead bindings without keeping handlers alive. (Handler *teardown* after the bridge is made safe separately, by the `_liveness` token — not by this weak storage.) |
| Teardown order | **`shared_ptr<const void> _liveness` + per-handler `weak_ptr`** | Makes bridge-vs-handler destruction order-independent: a handler outliving its bridge skips deregistration instead of dereferencing a dangling `Bridge&`. Normal `execute`/`subscribe` still require the bridge to outlive its handlers. |
| Backend pointer | **Short snapshot under the dedicated `_backendMtx`** | `executeVia()` reads the backend through a `loadBackend()` helper that copies the `shared_ptr` under `_backendMtx` (never `_mtx`), so it never blocks on `switchBackend()`'s `_mtx`. |
| Session storage | **Separate `_sessionMtx` from `_mtx`** | Session access is a hot path (every `executeVia` reads it). A separate mutex avoids contention with handler registration/switchBackend. |
| Attach-path locking | **Separate `_attachMtx` from `_mtx`** | `attachHandler`/`ensureBound`/`assignHandlerPrimary` can block on a full network round-trip for a remote backend. A dedicated mutex means that round-trip never blocks unrelated `registerHandler`/`deregisterHandler`/`switchBackend` calls on the same `Bridge`, closing a deadlock hazard if the thread expected to deliver the pending reply itself needs `_mtx`. `HandlerBinding::primary`/`contextKey` are mutated only under `_attachMtx`; `switchBackend()` and the reconnect handler, which also touch them, take both mutexes together. |
| Reconnect handler | **Liveness guard + weak‑backend guard + stale check; cleared in `~Bridge`** | The lambda captures a `weak_ptr<const void>` to `_liveness` and a `weak_ptr<IBackend>`. On invocation it first locks the liveness token — if the `Bridge` is gone it returns without touching `this` (no use-after-free). It then checks `pinned == loadBackend()` — if a switch occurred since the handler was installed, the reconnect is ignored. `~Bridge` and `switchBackend` also clear the outgoing backend's handler via `setReconnectHandler(nullptr)`; the liveness guard covers a reconnect already in flight when teardown races it. |
| Subscription keying | **On the result type, and against the binding rather than an instance id** | A subscriber is a renderer: it cares about the state it draws, not about which of several actions produced it, so a new action yielding the same type never breaks it. Storing against the binding makes a subscription follow a re-pointed handler, which is what "tell me about the account I am looking at" requires. |
| Action readiness | **`ActionValidator<Action>::ready(snapshot)`** | Framework-agnostic validation — each action struct defines its own required-field semantics. The bridge never interprets action fields. |
| Local-path validation enforcement | **`localOp` checks `ActionValidator<Action>::ready` before `Model::execute`** | Closes the gap where an `Action` built by hand and dispatched via `BridgeHandler::execute<Action>()` (without a client-side gate) reached the model unvalidated; mirrors `ActionDispatcher::registerAction`'s server-side runner (`registry.md`). Backward compatible: `ready()` defaults to `true` for actions with no validator. |
| Subscription keys | **`string_view` into static storage** | `ActionTraits::typeId()` returns `constexpr` string literals with static duration. The `unordered_map` holds non-owning keys; no allocation, no lifetime issues. |
| `executeJson` | **Separate registry, not a vtable** | The action type is unknown at the call site. A flat `unordered_map<(modelId, actionId), Executor, PairKeyHash>` lets any translation unit register its actions without central registration or RTTI. |
| `registerActionExecutorOnce` | **`inline` definition in header** | The function is forward-declared in `registry.hpp` (`morph::model::detail`) but defined `inline` in `bridge.hpp`, after `ActionExecuteRegistry`. `inline` lets that definition be instantiated in every TU that transitively includes `bridge.hpp` without an ODR/link violation. The registration runs from the anonymous-namespace initializer the macro emits. Because the definition lives only in `bridge.hpp`, any TU expanding `BRIDGE_REGISTER_ACTION` must include it (directly or transitively) or the link fails with an unresolved symbol. |

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
  `static_cast`s back to `BridgeHandler<Model>*` for the model type it was
  registered under. Passing a handler whose model type does not match the
  `modelId` is undefined behaviour — there is no runtime type check. In
  practice `BridgeHandler::executeJson` always passes `this` with a matching
  `ModelTraits<Model>::typeId()`, so the invariant holds by construction; the
  hazard only exists for callers that invoke the registry directly.

## Cross-references

- [`backend.md`](backend.md) — `IBackend`, `LocalBackend`,
  `SimulatedRemoteBackend`, `registerModelWithContext`, `cancelPending`,
  `BackendChangedError`/`BridgeDestroyedError`, reconnect handlers.
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