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

For GUI-led workflows, `subscribe<Action>(cb)` registers a result callback,
`set<&Action::field>(value)` fills one field of an in-progress draft, and
`reset<Action>()` discards the draft. When all required fields are filled, the
handler automatically fires the action.

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
with serialization/deserialization lambdas and a `localOp` that calls
`Model::execute(*action)` and optionally records a journal `LogEntry` for
loggable actions. The typed result is unwrapped from `std::shared_ptr<void>`
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

**`switchBackend(newBackend)`** replaces the active backend atomically: the
switch either fully succeeds or leaves everything exactly as it was. It runs
in two phases under `_mtx`:

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
stores a `SubscriberState` shared pointer that supports the field-by-field and
subscription APIs.

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
**reconciles Quantity precision and enforces the action's validator** (see
below), calls `execute<Action>`, and serializes the result back to JSON. Throws
`std::runtime_error` if the action was never registered.

Between decode and dispatch the registry executor applies two normalisations, so
the request/reply path matches the schema and the reactive `set<>` path rather
than trusting the raw wire body:

- **Declared-precision reconciliation.** `morph::forms::reconcileDeclaredPrecision`
  retags every `Quantity` field of the decoded action to its *declared* precision
  (`Quantity<U, Dec>::declaredDecimals`), so the stored value's precision matches
  the schema's advertised `x-decimalPlaces` instead of whatever runtime `dp` the
  client sent. It is a no-op for actions with no `Quantity` members and for
  actions whose type glaze cannot reflect. See [forms.md](../forms/forms.md).
- **Validator enforcement.** `ActionValidator<Action>::ready(action)` is checked;
  if it returns `false` the executor throws `std::invalid_argument` and the
  completion resolves through `onError` (a proper error reply upstream) — the
  handler is never invoked with an invalid action. This closes a gap where the
  request/reply path skipped the readiness/validity check that the reactive
  `set<>` path (`tryFireImpl`) already performs. `ActionValidator::ready`
  auto-detects a `bool validate() const` member and defaults to `true`, so
  actions without a validator dispatch exactly as before (backward compatible).

**`subscribe<Action>(cb)`** / **`subscribe<Action>(cb, errCb)`** stores a
result (and optional error) callback keyed by
`ActionTraits<Action>::typeId()`. Callbacks execute on the GUI executor.

**`unsubscribe<Action>()`** clears both result and error callbacks for the
action type.

**`set<&Action::field>(value)`** updates one field of the in-progress draft.
Uses `MemberPointerTraits` to recover the action and field types from the
pointer-to-member. After setting the value, checks
`ActionValidator<Action>::ready(snapshot)`. If all required fields are
present, fires the action via `Bridge::executeVia` and delivers the result
to the registered `sink` callback. If a flight is already in progress, marks
`pending = true` and refires when the current flight completes
(debounce-like coalescence). On failure, the registered `errSink` is invoked;
if none is registered, the error is logged via `morph::log::logError`
(tagged `[subscription:<typeId>]`) rather than silently dropped.

**`reset<Action>()`** discards the in-progress draft.

**`guiExecutor()`** returns the executor passed at construction.

### SubscriberState

```cpp
struct SubscriberState {
    std::mutex mtx;
    Bridge* bridge;
    std::shared_ptr<HandlerBinding> binding;
    IExecutor* guiExec;
    std::unordered_map<std::string_view, SubscriberEntry> entries;
};
```

Keys are `std::string_view` pointing to `ActionTraits<A>::typeId()` string
literals (static storage duration — keys never dangle). Each `SubscriberEntry`
holds a `draft` (`std::any` of the action struct), `sink`, `errSink`, and
`running`/`pending` flags for flight tracking.

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

Compile-time decomposition of a pointer-to-data-member type. Used by
`BridgeHandler::set<auto FieldPtr>` to recover both the action type (`A`)
and the field type (`V`) from a single non-type template parameter, so
callers write `handler.set<&MyAction::c>(7.0)` with no redundant type
arguments.

## Subscription semantics

The fielded/reactive surface (`subscribe`, `set`, `unsubscribe`, `reset`) is
built on the per-handler `SubscriberState`. Exactly one `SubscriberEntry`
exists per action type, keyed by `ActionTraits<A>::typeId()`. The rules:

- **One subscriber per `(handler, action type)`.** `subscribe<A>(cb)` (or the
  two-argument `subscribe<A>(cb, errCb)`) *replaces* any callback previously
  registered for `A` on this handler — there is no fan-out. The two-argument
  overload additionally stores the error sink; the one-argument overload leaves
  the existing `errSink` untouched.
- **Fire without a subscriber.** A `set<>`-triggered fire runs whether or not a
  subscriber is installed. If no `sink` is registered when the result arrives,
  the result is simply dropped (the entry exists because `set<>` created the
  draft, but its `sink` is empty). Errors are different: with no `errSink` the
  error is logged via `morph::log::logError` tagged `[subscription:<typeId>]`,
  never silently dropped.
- **Default validator fires on the first `set<>`.** `ActionValidator<A>::ready`
  returns `true` only for an action that has *neither* a
  `BRIDGE_REGISTER_VALIDATOR` specialisation *nor* a `bool validate() const`
  member — for such an action the very first `set<>` puts the
  (single-field-populated) draft into a ready state and dispatches immediately.
  Actions that need several fields before firing add a `validate()` member (the
  preferred, macro-free path — auto-detected via the `HasValidate` concept) or
  specialise the validator.
- **Every ready `set<>` re-fires.** Each `set<>` landing a `ready()==true`
  snapshot dispatches the action again — live recomputation. Rapid patches
  coalesce: while a flight is running, further `set<>` calls set
  `pending=true`, and exactly one re-fire with the latest snapshot is issued
  when the in-flight completion resolves (`consumeFlight`).
- **Draft lifetime.** A draft is created lazily on the first `set<>` for its
  action type and persists across fires, across `unsubscribe<A>()`, and across
  `Bridge::switchBackend` (the draft lives in the handler's `SubscriberState`,
  not the backend). It is destroyed only when the handler is destroyed or when
  `reset<A>()` is called. `unsubscribe<A>()` clears both callbacks but leaves
  the draft intact.

## Thread safety

`Bridge` is fully thread-safe (see the `Bridge` section: separate
`_backendMtx`, `_mtx`, and `_sessionMtx`, with `executeVia` taking its backend
snapshot under the short, dedicated `_backendMtx` rather than `_mtx`).

`BridgeHandler`'s individual mutating operations — `set`, `subscribe`,
`unsubscribe`, `reset` — are each internally safe: they take the
`SubscriberState::mtx` while touching the entry map. Result and error callbacks
never run under that mutex; they are marshalled to the `guiExec` executor
passed at construction, and `tryFireImpl` captures a `weak_ptr<SubscriberState>`
so a callback that fires after the handler is destroyed is a no-op. The
intended usage is nonetheless **single-GUI-thread affinity**: a handler and its
subscriptions belong to one GUI thread, and interleaving concurrent `set<>`
storms from multiple threads onto the same handler, while memory-safe, has no
defined ordering guarantee beyond the per-operation locking.

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
| `switchBackend` | `void switchBackend(unique_ptr<IBackend>)` | Atomic: stages all re-registrations on the new backend, commits (publishes new ids + swaps) only if all succeed, else rolls back and rethrows leaving old backend + `currentId`s intact. Cancels old backend's pending ops with `BackendChangedError`. |
| `deregisterHandler` | `void deregisterHandler(const shared_ptr<HandlerBinding>&)` | Deregisters from active backend (if bound), resets `currentId` to 0, removes from tracking. |
| `executeVia<Model, Action>` | `Completion<R> executeVia(const shared_ptr<HandlerBinding>&, Action, IExecutor*)` | Lock-free dispatch. Attaches default session. Records journal for loggable actions. Value-forwarding into the typed `Completion` is `try`/`catch`-guarded — a throwing result move/copy resolves the completion via `onError` instead of hanging or terminating. |
| `setDefaultSession` | `void setDefaultSession(session::Context)` | Installs default session context. |
| `defaultSession` | `session::Context defaultSession() const` | Returns snapshot of default session. |

### `BridgeHandler<Model>`

| Member | Signature | Notes |
|---|---|---|
| ctor (default) | `BridgeHandler(Bridge&, IExecutor*)` | Registers via `Bridge::registerHandler<Model>()`. |
| ctor (custom binding) | `BridgeHandler(Bridge&, IExecutor*, shared_ptr<HandlerBinding>)` | Registers pre-built binding. |
| dtor | `~BridgeHandler()` | Deregisters via `Bridge::deregisterHandler`, but only if the bridge's liveness token is still alive; a no-op if the `Bridge` was already destroyed. |
| `execute<Action>` | `Completion<R> execute(Action)` | Typed dispatch through the bridge. |
| `executeJson` | `Completion<string> executeJson(string_view actionType, string_view bodyJson)` | Type-erased dispatch through `ActionExecuteRegistry`. |
| `subscribe<Action>(cb)` | `void subscribe(function<void(R)>)` | Result callback. |
| `subscribe<Action>(cb, errCb)` | `void subscribe(function<void(R)>, function<void(exception_ptr)>)` | Result + error callbacks. |
| `unsubscribe<Action>` | `void unsubscribe()` | Clears both callbacks. |
| `set<auto FieldPtr>` | `void set(MemberPointerTraits<decltype(FieldPtr)>::ValueType value)` | Field-by-field update; auto-fires when ready. |
| `reset<Action>` | `void reset()` | Discards in-progress draft. |
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
| Teardown order | **`shared_ptr<const void> _liveness` + per-handler `weak_ptr`** | Makes bridge-vs-handler destruction order-independent: a handler outliving its bridge skips deregistration instead of dereferencing a dangling `Bridge&`. Normal `execute`/`set` still require the bridge to outlive its handlers. |
| Backend pointer | **Short snapshot under the dedicated `_backendMtx`** | `executeVia()` reads the backend through a `loadBackend()` helper that copies the `shared_ptr` under `_backendMtx` (never `_mtx`), so it never blocks on `switchBackend()`'s `_mtx`. |
| Session storage | **Separate `_sessionMtx` from `_mtx`** | Session access is a hot path (every `executeVia` reads it). A separate mutex avoids contention with handler registration/switchBackend. |
| Reconnect handler | **Liveness guard + weak‑backend guard + stale check; cleared in `~Bridge`** | The lambda captures a `weak_ptr<const void>` to `_liveness` and a `weak_ptr<IBackend>`. On invocation it first locks the liveness token — if the `Bridge` is gone it returns without touching `this` (no use-after-free). It then checks `pinned == loadBackend()` — if a switch occurred since the handler was installed, the reconnect is ignored. `~Bridge` and `switchBackend` also clear the outgoing backend's handler via `setReconnectHandler(nullptr)`; the liveness guard covers a reconnect already in flight when teardown races it. |
| Fielded actions | **`SubscriberState` shared across `BridgeHandler` copy-unsafe design** | The handler is non-copyable; the subscriber state is `shared_ptr` so `tryFireImpl` can capture a `weak_ptr` and survive handler destruction. Flight tracking (`running`/`pending`) coalesces rapid `set` calls. |
| Action readiness | **`ActionValidator<Action>::ready(snapshot)`** | Framework-agnostic validation — each action struct defines its own required-field semantics. The bridge never interprets action fields. |
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
- [`concurrency_and_lifetimes.md`](../concurrency_and_lifetimes.md) — the broader
  mutex-ordering and object-lifetime rules this type participates in.