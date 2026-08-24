# morph Architecture

## Overview

`morph` is a typed, asynchronous bridge between a GUI thread and business-object models. Models may live in-process (local mode) or in a remote server process (remote mode). The GUI code is identical in both cases — only the backend implementation changes.

The framework is header-only (C++23, namespace `morph`), depends on Glaze for JSON reflection, and optionally integrates with Qt 6 via a separate target.

> **New to morph?** `docs/GETTING-STARTED.md` is the step-by-step tutorial that
> comes before this document: it builds one small app end to end
> (`examples/pastebin`) and explains why each piece exists. This page assumes
> you are already oriented. (Named, not linked, for the Doxygen reason below.)

> **Design specs.** This document is the cross-cutting map. The authoritative,
> per-subsystem reference lives in `docs/spec/` — one file per public type or
> subsystem, capturing invariants, API surface, and the reasoning behind each
> design. **`docs/spec/README.md` indexes all of them**, and is the quickest
> way to find the one you need: it groups every spec by the question it
> answers and traces one action end to end. (Named, not linked: Doxygen reads
> this file but not `docs/spec/`, and `WARN_AS_ERROR` turns a link it cannot
> resolve into a failed docs build — which is why every spec reference below
> is backticked too.)
> Consult the matching spec before changing a public type: e.g.
> `docs/spec/security.md` (authenticated sessions and the trust model),
> `docs/spec/VERSIONING.md` (the semantic-versioning / deprecation-window
> commitment), `docs/spec/core/completion.md`, `docs/spec/core/executor.md`,
> `docs/spec/core/bridge.md`, `docs/spec/journal/journal.md`,
> `docs/spec/offline/offline.md`, `docs/spec/session/session.md`,
> `docs/spec/core/wire.md`. Where this document and a spec disagree, the spec wins.

## Namespace map

The public surface is split per topic so callers always know whether a name is part of the stable API or an implementation detail.

| Namespace | Purpose | Public symbols |
|---|---|---|
| `morph::` | Macros only at this level | `BRIDGE_REGISTER_MODEL`, `BRIDGE_REGISTER_ACTION`, `BRIDGE_REGISTER_VALIDATOR` (at file scope, but specialise `morph::model::*` templates) |
| `morph::version` | Release version constants | `kMajor`, `kMinor`, `kPatch`, `kString` (see `docs/spec/VERSIONING.md`) |
| `morph::log` | Configurable logging | `LogLevel`, `setLogger`, `setLogLevel`, `getLogLevel`, `logDebug`, `logInfo`, `logWarn`, `logError` |
| `morph::exec` | Executor primitives | `IExecutor`, `ThreadPoolExecutor`, `MainThreadExecutor` |
| `morph::async` | Async result handle | `Completion<T>` |
| `morph::model` | Model & action traits | `ModelTraits<>`, `ActionTraits<>`, `ActionValidator<>`, `ActionLogPolicy<>`, `Loggable` |
| `morph::backend` | Pluggable backends | `LocalBackend`, `RemoteServer`, `SimulatedRemoteBackend` |
| `morph::bridge` | Bridge between handler and backend | `Bridge`, `BridgeHandler<M>` |
| `morph::offline` | Connectivity + replay | `NetworkMonitor`, `NetworkMonitorConfig`, `IOfflineQueue`, `QueueItem`, `OfflineQueueFullError`, `InMemoryOfflineQueue`, `FileOfflineQueue`, `FileOfflineQueueError`, `SqliteOfflineQueue`, `SqliteOfflineQueueError`, `SyncWorker` (incl. `DeadLetterSink`), `SyncResult`, `ReconnectCoordinator`, `ReconnectOutcome`, `ReconnectCoordinatorConfig` |
| `morph::session` | Per-call session context + authentication | `Context`, `IAuthorizer`, `AllowAllAuthorizer`, `allowAllAuthorizer`, `current`; authenticated sessions (`session_auth.hpp`): `SessionToken`, `TokenIssuer`, `TokenVerifier`, `SigningAuthorizer`, `MacFunction`, `hmacSha256`, `AuthError` |
| `morph::journal` | Ordered, replayable action log (issue #3) | `LogEntry`, `IActionLog`, `InMemoryActionLog`, `FileActionLog`, `SessionLog`, `OutboxRelay`, `OutboxRelayResult`, `PayloadMigrationRegistry`, `SchemaMismatchError`, `SerializationError`, `NullSinkError`, `replay()`, `toJson`/`fromJson`, `setActionLog`, `defaultActionLog`, `ScopedActionLog` |
| `morph::math` | Exact numeric values for actions | `Rational`, `DecimalPlaces`, `RationalError`, `kMaxDecimalPlaces`, `abs`/`ceil`/`floor`/`trunc` |
| `morph::units` | Unit-tagged, optionally-empty values | `Quantity<U>`, `UnitMeta`, `UnitTraits<E>` (app-specialised), `UnitAlternative<E>`, `HasUnitAlternatives`, `UnitEnum`, `isQuantity` |
| `morph::time` | UTC timestamps for actions | `DateTime`, `Timestamp` |
| `morph::forms` | JSON-Schema generation for auto-built GUIs | `schemaJson<A>()`, `allRequiredEngaged()`, `Choice<T, ...>`, `FixedString`, `isChoice`, `EmptyCapableField` |
| `morph::qt` | Qt integration (built only when `MORPH_BUILD_QT=ON`) | `QtExecutor`, `QtWebSocketBackend`, `QtWebSocketServer` |

Every nested `detail` namespace under those topics holds implementation symbols. These do appear in some public signatures (e.g. `Bridge`'s constructor takes `unique_ptr<backend::detail::IBackend>`), but callers never type a detail name directly — `std::make_unique<morph::backend::LocalBackend>(...)` converts implicitly.

## Layer diagram

```
┌─────────────────────────────────────────────────────────────────┐
│  Application / GUI                                              │
│  morph::bridge::BridgeHandler<Model>  ← typed user-facing API   │
├─────────────────────────────────────────────────────────────────┤
│  Public API  (morph::bridge / morph::async)                     │
│  Bridge · BridgeHandler<M> · Completion<T>                      │
├─────────────────────────────────────────────────────────────────┤
│  Backend abstraction  (morph::backend + morph::backend::detail) │
│  IBackend · LocalBackend · SimulatedRemoteBackend               │
│  RemoteServer · QtWebSocketBackend · QtWebSocketServer          │
├─────────────────────────────────────────────────────────────────┤
│  Registry & type erasure  (morph::model + morph::model::detail) │
│  ActionDispatcher · ModelRegistryFactory · ModelTraits · …      │
├─────────────────────────────────────────────────────────────────┤
│  Internal async core                                            │
│  IExecutor · ThreadPoolExecutor · MainThreadExecutor            │
│                                       (executor.hpp)            │
│  StrandExecutor · ModelId             (strand.hpp)              │
│  CompletionState<T>                   (completion.hpp)          │
├─────────────────────────────────────────────────────────────────┤
│  Cross-cutting                                                  │
│  LogLevel · setLogger · log helpers   (logger.hpp)              │
└─────────────────────────────────────────────────────────────────┘
```

## Deployment topologies

**Local mode** — model lives in the same process:

```
GUI thread
  └─ bridge::BridgeHandler<M>::execute(action)
       └─ bridge::Bridge::executeVia<M, A>
            └─ backend::LocalBackend::execute
                 └─ StrandExecutor → worker thread → Model::execute(action)
                      └─ async::Completion<T>::then callback → GUI executor
```

**Simulated-remote mode** — model lives behind an in-process `RemoteServer` (used in tests):

```
GUI thread
  └─ bridge::BridgeHandler<M>::execute(action)
       └─ bridge::Bridge::executeVia<M, A>
            └─ backend::SimulatedRemoteBackend::execute
                 └─ serialize action → backend::RemoteServer::handle (JSON wire envelope)
                      └─ ActionDispatcher → StrandExecutor → Model::execute
                           └─ serialize result → Completion<T>::then → GUI executor
```

**Qt WebSocket mode** — model lives in a separate process. Three classes with distinct responsibilities collaborate here: `qt::QtWebSocketBackend` is the **network client** (owns the `QWebSocket`, correlates replies to pending `Completion`s, handles reconnect); `qt::QtWebSocketServer` is the **network server** (pure transport — receives text frames and forwards them verbatim to `RemoteServer::handle()`, sends the reply back); `backend::RemoteServer` is the **protocol + dispatch** layer (parses envelopes, authorizes, routes to models) and is transport-agnostic — the same class also serves the simulated-remote topology above.

```
GUI thread (Qt process)                         Server process
                                                qt::QtWebSocketServer (transport)
  └─ bridge::BridgeHandler<M>::execute(action)
       └─ bridge::Bridge::executeVia<M, A>
            └─ qt::QtWebSocketBackend::execute   (network client)
                 └─ assign callId, send JSON  ──► qt::QtWebSocketServer::handle
                                                        └─ backend::RemoteServer::handle (JSON wire envelope)
                                                             └─ ActionDispatcher → StrandExecutor → Model::execute
                 ◄── JSON reply (ok|callId|result) ──────────────────────────────
            └─ resolve pending Completion
       └─ async::Completion<T>::then callback → qt::QtExecutor → GUI thread
```

The GUI sees the same `morph::async::Completion<T>` in all three modes.

**Offline / sync mode** — how the `morph::offline` primitives compose. These four classes do not form a backend of their own; they wrap whichever backend is active and drive `Bridge::switchBackend` + replay when connectivity flaps. The framework ships the primitives and the sequencing; the application wires them together (the probe, the queue's persistence, and the replay function are all caller-supplied):

```
morph::offline::NetworkMonitor            (detects connectivity; probe is caller-supplied)
  │ onOffline ──► app calls Bridge::switchBackend(local fallback)
  │               app/enqueuer writes pending actions into IOfflineQueue
  │                 (InMemoryOfflineQueue ships with the framework;
  │                  SqliteOfflineQueue/FileOfflineQueue ship; the app picks)
  │ onOnline  ──► morph::offline::ReconnectCoordinator  (sequences:)
  │                 1. tryReconnect()      → probe the primary backend
  │                 2. activatePrimary()   → Bridge::switchBackend(primary)
  │                 3. bindContext()       → rebind handlers
  │                 4. replay()            → morph::offline::SyncWorker::run()
  │                                            drains IOfflineQueue via a
  │                                            caller-supplied ReplayFunction
```

`NetworkMonitor` fires the transitions; `ReconnectCoordinator` owns the ordering and retry loop; `SyncWorker` owns the queue drain; `IOfflineQueue` owns the pending writes. Each is usable independently of the others.

## Wire protocol

`RemoteServer::handle` accepts JSON envelopes (`morph::wire::Envelope`). The `kind`
field is the discriminator:

| `kind` | Direction | Required fields | Meaning |
|---|---|---|---|
| `"register"` | client → server | `typeId`, `contextKey` (optional) | Register a model instance; server replies `ok` with `modelId` |
| `"deregister"` | client → server | `modelId` | Destroy model instance; server replies `ok` |
| `"execute"`   | client → server | `callId`, `modelId`, `modelType`, `actionType`, `body`, `session` (optional) | Dispatch an action |
| `"ok"`        | server → client | `callId`, plus `body` (execute result) or `modelId` (register reply) | Success |
| `"err"`       | server → client | `callId` (echoed), `message` | Failure |

All envelopes round-trip through Glaze JSON, so the protocol is self-describing,
escaping-safe, and easy to extend (add a field, ignore unknowns).

The `session` field is a `morph::session::Context` carrying a verified bearer
`token` alongside the (untrusted) client-asserted `principal`.
The server runs every incoming `execute` envelope through its configured
`IAuthorizer`; a `false` return causes the server to reply with
`err|unauthorized` (callId echoed). The default authorizer permits everything.
When a verifying authorizer (`SigningAuthorizer`) is installed, the server also
calls `IAuthorizer::authenticate` and, when it yields a principal from a valid
token, **overwrites** `Context::principal` with it before dispatch — so the
verified principal is authoritative and `session::current()->principal` read
inside a model is the authenticated identity, not the client's claim. See
"Authenticated sessions" below and `docs/spec/security.md`.

`RemoteServer::handleInline` is the synchronous variant used for control
messages (`register`/`deregister`) — e.g. when a `BridgeHandler` is constructed
from inside an action handler running on the worker pool. It **rejects**
`execute` up front with an `err` reply, because an `execute` reply is produced
asynchronously on the strand, after the synchronous call has already returned
and destroyed the reply buffer the deferred callback would write into.

`contextKey` carries a `register`ing instance's stable identity (e.g. an
account id) from `HandlerBinding::contextKey` across the wire, so a
server-side `RemoteServer::LogProvider` can attach an action log to the
instance it creates — see "Action log" below. Empty (the default) means no
identity; the field is ignored on every other envelope kind.

## Component detail

### Executors

All concurrency runs through `morph::exec::IExecutor::post(fn)`:

- **`ThreadPoolExecutor`** — fixed N worker threads, MPMC queue. A task exception is caught and logged via `morph::log` (`std::exception` with its `what()`, anything else as "unknown exception"); it never propagates out of the worker or aborts sibling tasks.
- **`MainThreadExecutor`** — single-threaded queue with `runFor(timeout)` drain; used in non-Qt tests to pump the "GUI" thread. It catches only `std::exception` from a task, logs it via `morph::log`, and continues with the next task.
- **`QtExecutor`** — posts via `QMetaObject::invokeMethod(Qt::QueuedConnection)`; safe from any thread; drops silently if the target object is deleted.

`morph::exec::detail::StrandExecutor` (below) is where `Model::execute()` actually runs; like `ThreadPoolExecutor`, it catches a task exception (`std::exception` or unknown) and logs it via `morph::log` so a throw neither stalls the strand nor vanishes — the next queued task for that model still runs.

### StrandExecutor

`morph::exec::detail::StrandExecutor` guarantees that all tasks for the same `ModelId` are serialised while tasks for different models are parallelised. Internally keeps one `std::queue` and a `running` flag per model; tasks are dispatched to the underlying `IExecutor` one at a time.

### Completion<T>

Move-only public result handle (`morph::async::Completion<T>`). Internally backed by `morph::async::detail::CompletionState<T>` (mutex-protected value + error + two callback slots). Key invariant: callbacks are always delivered via the `IExecutor` supplied at construction, so the GUI thread is never blocked and callbacks always fire on the right thread.

If a `Completion` is destroyed with an unhandled error (no `.onError` was attached), `CompletionState::~CompletionState` logs the exception through the configured logger.

### Registry & type erasure

`morph::model::ModelTraits<M>` and `morph::model::ActionTraits<A>` are specialised by registration macros. They provide:
- String type IDs for protocol routing — the string literals passed as the `NAME` argument to `BRIDGE_REGISTER_MODEL(M, "MyModel")` / `BRIDGE_REGISTER_ACTION(M, A, "MyAction")` in the `.cpp` that owns the model. These strings travel in the wire envelope's `modelType`/`actionType` fields and key the server's dispatch tables.
- Glaze-based `toJson` / `fromJson` for actions and results.

`morph::model::detail::ActionDispatcher` maps `(modelTypeId, actionTypeId)` → a runner lambda that downcasts `IModelHolder`, calls `model.execute(action)`, and returns the result as JSON.

`morph::model::detail::ModelRegistryFactory` maps `modelTypeId` → a factory closure that constructs an `IModelHolder`.

### HandlerBinding — why it exists

When `bridge::BridgeHandler<M>` registers with the `bridge::Bridge`, the backend assigns it a `ModelId` (just a `uint64_t`). That ID is backend-local — if the backend is ever replaced, all existing IDs are meaningless.

The problem is that `Bridge` needs to be able to find every live `BridgeHandler` later (e.g. to re-register it on a new backend), but it cannot hold a strong reference to them — that would prevent `BridgeHandler` from being destroyed naturally at the end of its scope.

`bridge::detail::HandlerBinding` is the indirection that solves both constraints:

```
BridgeHandler<M>  ──owns──►  shared_ptr<HandlerBinding>
Bridge            ──holds──►  weak_ptr<HandlerBinding>   (does NOT keep it alive)
```

`BridgeHandler` holds the `shared_ptr`, so the `HandlerBinding` lives exactly as long as the handler does. `Bridge` holds a `weak_ptr`, so it can observe whether the handler is still alive — but it cannot prevent its destruction.

**RAII lifetime:** When `BridgeHandler` goes out of scope, its destructor calls `Bridge::deregisterHandler`, which tells the backend to destroy the model instance and removes the stale `weak_ptr` from the Bridge's list. No manual cleanup is required from application code.

**Destruction order is safe either way:** the `BridgeHandler` also holds a `weak_ptr<const void>` liveness token published by the `Bridge` (`Bridge::liveness()`; the token is destroyed with the bridge). The destructor deregisters only if that token is still live — if the `Bridge` was destroyed first, the token has expired and the handler skips deregistration instead of dereferencing a dangling `Bridge&`. Destroying a bridge before its handlers is still discouraged, but is defined behaviour rather than a use-after-free.

**`atomic<uint64_t> currentId`:** Every call to `executeVia` reads `binding->currentId` to find out which backend-assigned ID to use. The value is an atomic so it can be updated during a backend switch without holding the bridge mutex for the duration of every execute call. A value of `0` is the sentinel for "not bound" — `executeVia` returns an immediate error in that case.

**Backend switching:** `Bridge::switchBackend(newBackend)` acquires the bridge mutex, re-registers every live `HandlerBinding` on the new backend, replaces the active backend, then calls `notifyBackendChanged()`. `LocalBackend` forwards this to every live model that implements `IBackendChangedSink` (detected at compile time via the `BackendChangedNotifiable` concept). `SimulatedRemoteBackend` is a no-op — its models live inside `RemoteServer`.

The switch is **exception-safe and atomic** (stage-all-then-commit): phase 1 registers every binding on the new backend into a staging list *without* touching any `currentId`; if any registration throws (a plausible remote/transport failure) the already-registered instances are rolled back with `deregisterModel` and the exception is rethrown, leaving the old backend and every `currentId` untouched — the switch either fully succeeds or is a no-op. Only after all registrations succeed does phase 2 publish the new `ModelId` values atomically into each `binding->currentId` and swap the backend in. The outgoing backend's pending completions are drained (`cancelPending` with `BackendChangedError`) *outside* the bridge mutex, so user callbacks never run while `_mtx` is held.

### Logger

`morph::log` provides a global, mutex-protected, replaceable sink (`std::function<void(LogLevel, std::string_view)>`). `LogLevel` is a `uint8_t`-backed enum (`debug < info < warn < error < off`). All framework internals route through `morph::log::detail::log(level, msg)`; call `morph::log::setLogger` at startup to redirect to spdlog, Qt logging, or a test spy.

### Authenticated sessions

`morph::session` carries a per-call `Context` (principal, verified `token`,
`requestId`, `locale`, and a free-form metadata bag) from the caller through the
bridge to the model — `session::current()` reads it inside `execute()` without
changing the model signature. The server-side authorization seam is
`IAuthorizer` (`authorize` gates each `execute`; `authenticate` returns the
verified principal to make authoritative); `AllowAllAuthorizer` /
`allowAllAuthorizer()` is the default and permits everything.

`session_auth.hpp` adds the opt-in authenticated variant: signed, stateless
bearer tokens (`SessionToken` claims, `base64url(claims).base64url(mac)`) minted
by `TokenIssuer`, verified by `TokenVerifier`, and enforced per request by
`SigningAuthorizer`. The MAC primitive is pluggable (`MacFunction`); a
self-contained, test-vector-verified `hmacSha256` is the default reference
implementation. `AuthError` enumerates verification failures.

**`docs/spec/security.md` is the authoritative spec** for the trust model, the
authoritative-principal flow, and the limits of the reference crypto — this
section is only a map, not a substitute.

### NetworkMonitor

`morph::offline::NetworkMonitor` provides a background probe thread that tracks connectivity and fires callbacks on transitions. The framework supplies no probe implementation — the caller provides a `bool()` callable that returns `true` when the system is reachable (TCP connect, HTTP ping, custom check, etc.). This keeps the monitor fully portable across OS and transport.

**State machine:**
- Starts online (assumes connectivity until proven otherwise).
- Goes offline after `failureThreshold` consecutive probe failures → fires `onOffline`.
- Returns online after `onlineThreshold` consecutive probe successes → fires `onOnline`.
- Each transition fires its callback exactly once.
- Callbacks run on the probe thread and must not block.

**Thread safety:** `isOnline()` reads an `std::atomic<bool>` — safe from any thread. `stop()` is idempotent and safe to call multiple times or from any thread. The destructor calls `stop()`, so explicit shutdown is optional.

**Intended integration with switchBackend:**
```cpp
morph::offline::NetworkMonitor monitor{
    myTcpProbe,
    [&] { bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(localPool)); },
    [&] { bridge.switchBackend(std::make_unique<morph::backend::SimulatedRemoteBackend>(server)); }
};
```

### IOfflineQueue + InMemoryOfflineQueue

`morph::offline::IOfflineQueue` provides an interface over three operations:

| Method | Semantics |
|---|---|
| `enqueue(payload)` | Append an opaque string item; returns a stable `uint64_t` id |
| `drain()` | Return all pending items in enqueue order; does **not** remove them |
| `markDone(id)` | Remove item by id; no-op if unknown |

`drain()` returning items without removing them is deliberate — items survive a crash between `drain()` and `markDone()`. A SQL-backed implementation can persist items across process restarts by storing them in a table with a UNIQUE constraint on the payload.

`InMemoryOfflineQueue` implements the interface with a `std::deque` protected by a mutex. It does not deduplicate.

### Action log — ordered, coalescing, identity-aware execution history

`morph::journal` records executed actions as an ordered, replayable log, distinct in purpose from `IOfflineQueue` above: `IOfflineQueue` holds pending writes awaiting retry and deletes them once delivered; the action log is a permanent audit/replay trail — entries are never removed by the framework.

**`IActionLog`** is the durable-sink interface (`append`, `flush`, `entries`), implemented by `InMemoryActionLog` and `FileActionLog` (append-only NDJSON, `flush()` fsyncs). Each `LogEntry` carries `modelType`, `entityKey`, `actionType`, `payload`/`result` JSON, `principal`, and a sink-assigned `seq`. `FileActionLog::entries()` **tolerates a torn trailing line**: a crash between `append`'s write and the next flush can leave a truncated final line, so a malformed *last* line is logged and skipped rather than throwing — but a malformed line mid-file is genuine corruption and is re-thrown.

**Set the sink once, in `main()` — every model uses it automatically.** `morph::journal::setActionLog(log)` installs a process-wide default. `ModelFactory::create<Model>()` — the factory behind every ordinary model registration, local *or* remote — attaches that default to each new instance automatically (empty `entityKey`). No per-model, per-handler, or per-backend wiring is required; `RemoteServer`-owned instances get it exactly the same way, since they're constructed through the same factory. `defaultActionLog()` reads the current sink back; `ScopedActionLog` (RAII, mirrors `morph::log::ScopedLoggerOverride`) installs one temporarily and restores the previous one on scope exit — the tool tests use it to avoid leaking a sink across test cases.

Application code that needs a specific instance identity (e.g. per-account auditing) can still call `IModelHolder::attachActionLog(log, contextKey)` explicitly on that instance — an explicit call always overrides the default, and is the seam `HandlerBinding::contextKey`/`RemoteServer::setLogProvider` (below) build on for the remote case. Recording itself happens at the two call sites that are the *only* two places `Model::execute()` is ever invoked in the whole codebase:

| Site | Topology | 
|---|---|
| `ActionDispatcher::registerAction`'s runner (`registry.hpp`) | Every remote/Qt topology — `RemoteServer` owns the persistent `IModelHolder`s and dispatches through here |
| `Bridge::executeVia`'s `localOp` (`bridge.hpp`) | Local mode only — `LocalBackend` calls this directly; remote backends never invoke `localOp` at all |

Because these are mutually exclusive per topology, recording is automatically server-side wherever a client/server split exists, with no extra plumbing.

**`Loggable`** (`morph::model::Loggable::{Yes,No}`) is a strong-typed opt-out on the existing `BRIDGE_REGISTER_ACTION` macro (an optional 4th argument; no separate registration macro). Default is `Yes` — every action is recorded unless explicitly marked `Loggable::No` (typically pure queries like `GetAccount`/`ListAccounts`). Hand-written `ActionTraits` specialisations that predate this member (as used in several tests) are unaffected: `morph::model::detail::actionLoggable<A>()` defaults to `Yes` when the member is absent, via a `HasLoggableFlag` concept exactly like `ActionValidator`'s `HasValidate`.

**`ActionLogPolicy<A>::coalesce`** (default `false`) decides whether repeated executions of the same action against the same entity should collapse to the latest occurrence at a checkpoint, or whether every occurrence is a distinct, permanent fact. This matters because a form driving one action per edit can fire the same action many times in a row — without coalescing, every keystroke-driven re-fire would become a permanent log entry. `false` is correct for anything resembling a business event (a deposit); `true` is for drafts/settings where only the final value matters.

**`SessionLog`** (`journal.hpp`) is where coalescing actually happens. It keeps full, uncoalesced history in memory (the raw material for `undoLast()`), and `checkpoint(durableSink)` reduces everything appended since the last checkpoint by `(modelType, entityKey, actionType)` — keeping only the latest entry where `coalesce == true`, every entry otherwise — before forwarding the reduced set to the real sink. `undoLast()` needs no inverse operations: it drops the most recent entry and calls `journal::replay()` over what remains, reusing the same `ActionDispatcher`/`ModelRegistryFactory` `RemoteServer` already relies on for dispatch. This is not a workaround — a model's entire state genuinely is "initial state plus its ordered actions replayed," so reconstructing it by replay is the direct statement of that fact, not a special case.

**Replay/undo never records into the live default log.** `journal::replay()` builds the reconstructed holder through `ModelRegistryFactory::create`, which (like every factory-built model) auto-attaches the process-wide default action log. Before replaying, `replay()` immediately **detaches** it (`attachActionLog(nullptr, {})`), so re-running the recorded actions does not re-record each one into the live sink — which would corrupt the very audit trail being read from. The suppression is scoped to the replay pass only: once replay finishes, the reconstructed instance becomes the live model, and any new action executed on it goes through the normal dispatch path (with the log attached) and is recorded as a new entry appended after the surviving entries — e.g. journal `[A, B, C]`, undo `C`, execute `D` leaves `[A, B, D]`.

**Remote-mode per-instance identity** (`RemoteServer::setLogProvider`) is the advanced escape hatch for when the global default isn't granular enough: `RemoteServer` owns the actual model instances behind any remote/simulated-remote client, so it is the only place able to attach a *different* log (or a specific `entityKey`) to a *specific* instance. `HandlerBinding::contextKey` (client-side) travels through the `register` wire envelope's `contextKey` field; if a `LogProvider` is installed, `RemoteServer` calls it with `(modelType, contextKey)` and attaches whatever `IActionLog` it returns (or nothing, if it returns `nullptr` or no `contextKey` was sent) before the instance ever executes an action — overriding whatever the global default would have attached.

**Transactional outbox (opt-in)**: a model that also owns its own durable store can avoid the log and the store's committed state silently diverging by writing its own outbox row inside its own transaction, calling `IModelHolder::setOutboxManaged(true)` to suppress the automatic append, and draining that outbox through a `journal::OutboxRelay` into the real sink — see `docs/spec/journal/journal.md`'s "Transactional outbox (opt-in)" section. `examples/bank` still demonstrates the un-opted-in two-independent-writes behavior this closes for models that adopt the pattern.

### SyncWorker

`morph::offline::SyncWorker` drains an `IOfflineQueue` on reconnect. The caller supplies a `ReplayFunction` (`bool(const std::string& payload)`) that knows how to process each item — the framework has no knowledge of what replay means (insert to DB, POST to API, etc.).

- Returns `true` → `markDone` called, item removed.
- Returns `false` or throws → item left in queue for the next `run()`.
- `stop()` signals the current `run()` to abort after the current item. Resets at the start of each `run()`, so it is one-shot.
- Concurrent `run()` calls are serialised by an internal mutex.

### ReconnectCoordinator

`morph::offline::ReconnectCoordinator` sequences the steps that must happen, in
order, when connectivity returns: `tryReconnect()` → `activatePrimary()` →
`bindContext()` → `replay()`. All side effects are injected via `Deps`
callables — the class contains only the retry loop, the strict ordering
guarantee (each step waits for the previous), and the abort checks
(`shouldContinue()` is polled before each attempt and again before replay). It
performs no I/O and owns no thread; `onOnline()` / `onOffline()` run
synchronously on the calling thread and are mutually serialised by an internal
mutex (mirroring `SyncWorker::run()`). `onOnline()` returns a `ReconnectOutcome`
(`Reconnected` / `GaveUp` after `maxAttempts` / `Aborted`); retry tuning lives in
`ReconnectCoordinatorConfig` (`maxAttempts`, `retryDelay`). `tryReconnect` and
`shouldContinue` throwing are treated as a failed attempt / "do not continue"
respectively.

### Conflict Resolution — a domain concern, not a framework concern

Conflict resolution during offline-to-online sync belongs entirely in the model. The framework's role is to fire `onBackendChanged()` on the new model instance and then step back. How the model handles that notification is its own business.

**What the framework guarantees:**
- `onBackendChanged()` fires exactly once per `switchBackend()` call.
- It fires on the **new** backend's model instance (not the old one).
- It fires after all handlers are re-registered — `execute()` calls issued from within `onBackendChanged()` will reach the new backend.
- Each `switchBackend()` creates a fresh model instance via the registered factory. If the factory captures dependencies by reference, the new instance shares the same queue, resolver, and domain services as the old one.

## Thread safety

| Component | Guarantee |
|---|---|
| `Model::execute` | Never called concurrently for the same `ModelId` (strand). |
| `Completion<T>` / `CompletionState<T>` | Fully mutex-protected; callbacks always marshal to the supplied executor. |
| `Bridge` | Handler list protected by mutex; register/deregister safe from any thread. |
| Logger | Sink and level accesses protected by mutex. |
| `StrandExecutor` | Per-strand mutex + atomic running flag; safe from any thread. |
| `Bridge::switchBackend` | Holds bridge mutex while staging + committing; re-registration and notification are atomic with respect to new `execute` calls. Exception-safe: a registration failure rolls back and leaves the old backend and all `currentId`s untouched (no-op). Outgoing-backend cancellation runs after the mutex is released. |

## Error propagation

```
Model::execute(action) throws
  └─ LocalBackend's strand task catches via try/catch
       └─ CompletionState::setException(current_exception())
            └─ .onError(fn) handler posted to GUI executor
                 └─ fn receives exception_ptr; caller rethrows to inspect
```

If the `Completion` is abandoned (no `.onError` attached, or no callback executor to deliver it on), the destructor logs the exception through the orphan logger. Non-`std::exception` types are logged as "unknown exception".

Task exceptions on the executors themselves are handled independently: `ThreadPoolExecutor` and `StrandExecutor` catch and log every task throw via `morph::log`, and `MainThreadExecutor::runFor` catches `std::exception`. A throwing task therefore never kills a worker or stalls a strand — see "Executors" above.

## Adding a new model and actions

1. Define the model struct with `execute` overloads:

```cpp
struct MyAction { int x = 0; };

struct MyModel {
    int execute(const MyAction& a) { return a.x * 2; }
};
```

2. Specialise traits and register (in a `.cpp` that owns the model):

```cpp
#include <morph/registry.hpp>

BRIDGE_REGISTER_MODEL (MyModel,  "MyModel")
BRIDGE_REGISTER_ACTION(MyModel, MyAction, "MyAction")
```

3. Use from the GUI (same code for local and remote):

```cpp
morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
morph::bridge::BridgeHandler<MyModel> handler{bridge, &guiExecutor};

handler.execute(MyAction{21})
    .then([](int result) { /* runs on GUI thread */ })
    .onError([](std::exception_ptr e) { /* runs on GUI thread */ });
```

## Keyed, shareable model instances

A `BridgeHandler` normally registers its own model instance. A model that
declares a **primary key** can instead have its instances shared: handlers that
name the same key reach one instance, through a directory the *server* owns, so
the sharing spans clients and not merely handlers.

The key is declared beside the registrations, never inside the model class:

```cpp
BRIDGE_MODEL_KEY(AccountModel, LoadAccount, &LoadAccount::id);  // key type deduced
BRIDGE_KEY_FROM(CloseAccount, &CloseAccount::id);               // also carries it
```

`BRIDGE_MODEL_KEY` appears once per model — it specialises
`ModelKeyTraits<Model>`, which cannot be repeated — and every other action
naming the same entity uses `BRIDGE_KEY_FROM`. Actions with neither declaration
are *keyless*, which is the common case: they run against whichever instance
their handler is already attached to.

### Behavior

| Aspect | Default |
|---|---|
| **Opt-in** | `BridgeHandler<M, AllowShared>`. Plain `BridgeHandler<M>` keeps a private instance and never enters the directory. |
| **Attachment** | Automatic: executing a keyed action attaches, or re-points, the handler to that key's instance, constructing one only if none is live. |
| **Re-pointing** | A keyed action naming a different key moves the *handler*. Instances never change identity, so a key always maps to one instance. |
| **Lifetime** | Refcounted across every attachment, including across connections. The instance dies when the last one goes. |
| **Ownership** | A shared instance is recorded with no owner principal — per-instance ownership and cross-client sharing are mutually exclusive. |
| **Creating actions** | `BRIDGE_MODEL_KEY_FROM_RESULT` takes the key from the reply and promotes the instance the action ran on, so nothing it built is stranded. |

Full design, including the wire additions (`primary`/`shared` fields and the
`attach`/`assign`/`instances` kinds) and the connection-scope refcount, is in
`docs/spec/core/shared_instances.md`.

## Instance subscriptions

The framework offers two complementary surfaces for talking to a model:

1. **One-shot**: `handler.execute(action) -> Completion<R>`. The action is built
   at the call site and dispatched in one call.
2. **Observing**: `handler.subscribe<R>(cb)` fires whenever an `R` is produced
   on *the instance this handler is attached to* — by this handler, by another
   handler sharing that instance, or by another screen entirely. Suitable for a
   view that renders some model state and must stay current when anything
   changes it.

A subscription names the **result/state type**, not an action. The subscriber
describes what it renders rather than what somebody else must call to produce
it, so adding an action that also yields an `R` never breaks an existing
subscriber.

### API

```cpp
morph::bridge::BridgeHandler<AccountModel, morph::bridge::AllowShared> screen{bridge, guiExec};
morph::bridge::BridgeHandler<AccountModel, morph::bridge::AllowShared> sidebar{bridge, guiExec};

screen.attach(42);
sidebar.attach(42);                       // same instance

screen.subscribe<AccountInfo>([](AccountInfo info) { renderBalance(info); });

sidebar.execute(Deposit{.amountMinor = 5000});
// -> screen's callback runs: it never had to know Deposit exists
```

### Behavior

| Aspect | Default |
|---|---|
| **Keying** | On the result type `R`. Any action producing an `R` notifies. |
| **Scope** | The instance the handler is currently attached to. Matched at publish time, so a subscription follows a re-pointed handler. |
| **Subscriber cardinality** | One callback per `(handler, R)`. `subscribe<R>(cb)` replaces any prior callback. |
| **Echo** | The originating handler is notified too — no "was this mine" bookkeeping in subscribers. |
| **Failures** | A failed action notifies nobody. |
| **Ordering** | Per instance, guaranteed by that instance's strand. Nothing is guaranteed between instances. |
| **Durability** | None. Best-effort and unbuffered: no replay, no cursor, no coalescing. |
| **Callback thread** | Always the executor passed at handler construction. |


## Exact values, units, and schema-driven forms

Three headers extend actions from "any aggregate" to *self-describing*
aggregates a client can build its GUI from a runtime: `rational.hpp` (exact
numbers), `quantity.hpp` (unit-tagged, optionally-empty values) and
`forms.hpp` (JSON Schema generation). Nothing else in the framework includes
them — they are an opt-in layer that composes with registration, validators,
and the wire. `examples/forms` demonstrates the whole loop with two
renderers: a self-contained HTML page and a Qt Quick client
(`MORPH_BUILD_FORMS_QML=ON`), both driven purely by the generated schemas.

### `morph::math::Rational` — exact numbers on the wire

A trivially-copyable `numerator/denominator` pair (`int64_t`) plus a
`DecimalPlaces` strong type. Arithmetic is exact and reduces to canonical
form; binary operations propagate the wider precision; comparison ignores
precision entirely. Fallible operations (`operator/`, `fromFloat`) return
`std::expected<Rational, RationalError>`, and mixed expressions containing an
`expected` or a floating-point operand evaluate to `expected` with
left-to-right error short-circuiting.

On the wire a Rational is `{"num":617,"den":50,"dp":2}`. The Glaze codec
routes every read through the canonicalising constructor: a non-canonical
payload (`1234/100`) lands reduced, a hostile one (`den == 0`, out-of-range
`dp`) is clamped rather than asserted — wire input is untrusted by design.

### `morph::units::Quantity<U>` — one kind of empty, units as types

```cpp
enum class Unit : std::uint16_t { scalar, kg, m3, kg_per_m3 };

template <> struct morph::units::UnitTraits<Unit> {
    static constexpr morph::units::UnitMeta meta(Unit) noexcept { /* id, display, decimals */ }
};
consteval Unit operator/(Unit lhs, Unit rhs) { /* kg / m3 -> kg_per_m3, else throw */ }

using Mass    = morph::units::Quantity<Unit::kg>;
using Volume  = morph::units::Quantity<Unit::m3>;
// Mass{...} / Volume{...} deduces Quantity<Unit::kg_per_m3> at compile time.
```

Design decisions, in order:

- **One kind of empty.** The blank state ("not entered", "not measured")
  lives *inside* the quantity as `std::optional<Rational>`; action structs
  never wrap a `Quantity` in another `std::optional`. Whether a field may
  still be empty at submit time is field *metadata* (below), not a second
  wrapper type. Empty propagates through arithmetic (spreadsheet/SQL-NULL
  semantics); division by zero also yields empty.
- **Units are types, defined by the application.** morph ships no unit enum.
  The app supplies its own enum, a `UnitTraits` specialisation (schema id,
  display text, default decimals) and a `consteval` algebra; `Quantity`'s
  `operator*` / `operator/` deduce result units from that algebra, and
  unsupported combinations fail to compile at the call site. Unit ids are
  protocol vocabulary: append enumerators, never renumber or rename.
- **Units never travel.** The wire payload is just the nullable Rational
  (`glz::meta` unwraps the member), so a client cannot send a mismatched
  unit. Units appear in generated schemas (`ExtUnits`) and in C++ types only.
- **Declared precision lives in the type; actual precision is runtime
  data.** `Quantity<U, Decimals>`'s second argument is the field's declared
  decimal count — defaulted from `UnitTraits`, overridable per field
  (`Quantity<Unit::m3, 4>`) — and feeds `x-decimalPlaces` and `fromDouble`.
  The value's actual precision is the Rational's runtime tag: it
  max-propagates through arithmetic and is adjustable at run time —
  `withDecimalPlaces` retags for display and leaves the value alone, while
  `roundedToDecimalPlaces` / `atDeclaredPrecision` re-round the value exactly
  (`math::roundToDecimalPlaces`, half away from zero by default) so storage and
  display agree. Same-unit quantities convert freely across declared precisions;
  computed temporaries carry the unit default.

### `morph::forms` — schemas for auto-built GUIs

`schemaJson<A>()` wraps Glaze's `write_json_schema` (which contributes types,
`$defs`, and any per-field metadata declared via `glz::json_schema<A>`) and
closes the gaps a form renderer needs:

- **`required`** — derived, not declared twice: a member is required unless
  it is a `std::optional<...>` or named in the action's
  `static constexpr std::array optionalFields{...}` opt-out list.
- **`x-decimalPlaces`** — on every `Quantity` property, from `UnitTraits`, so
  the client knows the input step.
- **`x-order`** — the member's declaration index on every property, so field
  layout survives schema round-trips through order-losing DOMs.

`allRequiredEngaged(action)` is the matching readiness predicate ("every
required empty-capable field is engaged" — `Quantity`, `Choice`, `Timestamp`,
or any type with a `hasValue()`) intended as the body of the action's
`validate()` — which the existing `ActionValidator` resolution picks up
automatically. One declaration then drives the schema's `required` array, the
client-side submit gate, and the server-side readiness check.

### `morph::time::Timestamp` and `morph::forms::Choice` — dates and combo boxes

Two further field types follow the same one-kind-of-empty pattern:

- **`Timestamp`** (over `morph::time::DateTime`, adapted from LASTRADA
  `Toolbox/Chrono.hpp`): a UTC instant travelling as a strict ISO-8601 string
  (`"2026-07-05T14:30:00.000Z"`). The parser is hand-rolled (no locale, no
  `std::chrono::parse`) so behaviour is identical across standard libraries,
  and — unlike the clamping `Rational` codec — a malformed timestamp is a
  JSON **read error**: there is no meaningful clamp for a mistyped instant.
  Schemas carry the standard `"format": "date-time"` annotation.
- **Unit switching** (`UnitTraits<E>::relations`): a unit system may
  declare convertible entry units per canonical unit with exact rational
  ratios (grams -> kilograms as `{g, 1, 1000}`). They surface as
  `x-unitAlternatives` in the schema; renderers offer a unit selector and
  recalculate the entered value exactly on switch, and payloads always carry
  the canonical unit — the model never sees display units. The alternatives
  list is **derived from the same `relations`** that drive `convert` — there is
  no separate `alternatives` declaration to keep in sync.
- **`Choice<T, "ListSamples", "id", "name">`**: declares in the type that the
  field is *not free input* — its options are the rows returned by executing
  the named action (itself just a registered action, typically
  `Loggable::No`). The schema carries `x-optionsAction`/`x-optionValue`/
  `x-optionLabel`; renderers execute the options action, build a combo box
  from the rows, and submit the selected `valueField` as the payload. On the
  wire a `Choice` is its bare nullable value — options metadata never
  travels. The demo's HTML page resolves options at emit time (a static page
  cannot fetch); the QML client fetches them live over the same in-process
  wire it submits on.

## Header map

The headers are grouped into per-sub-domain subdirectories that mirror the
namespaces. Design specs live under `docs/spec/<sub-domain>/` with the same
folder names. One exception: `include/morph/version.hpp` sits directly under
`include/morph/`, with no subdirectory — it is cross-cutting library
metadata rather than part of any one sub-domain, so it is documented in the
top-level `docs/spec/VERSIONING.md` instead of a `docs/spec/<sub-domain>/`
folder.

### Library headers (`include/morph/`)

#### `core/` — async core, registry, bridge, backends, wire

| Header | Responsibility |
|---|---|
| `core/logger.hpp` | `LogLevel`, log configuration and level helpers; internals in `morph::log::detail` |
| `core/executor.hpp` | `IExecutor`, `ThreadPoolExecutor`, `MainThreadExecutor` (`morph::exec::`) |
| `core/strand.hpp` | `ModelId`, `ModelIdHash`, `StrandExecutor` — serialises tasks per model (`morph::exec::detail::`) |
| `core/completion.hpp` | `CompletionState<T>` (detail) + `Completion<T>` (public) — result handle |
| `core/model.hpp` | `IModelHolder`, `ModelHolder<T>`, `ModelFactory`, `IBackendChangedSink`, `BackendChangedNotifiable` — type-erased model storage; `IModelHolder::attachActionLog`/`hasActionLog`/`recordIfAttached` (`morph::model::detail::`) |
| `core/registry.hpp` | `ModelTraits<>`, `ActionTraits<>`, `ActionValidator<>`, `ActionLogPolicy<>`, `Loggable` (public) + `ActionDispatcher` (also tracking each action's `coalesce` policy), `ModelRegistryFactory`, `defaultDispatcher()`, `defaultRegistry()`, `ParseError`, `registerModelOnce`, `registerActionOnce`, `actionLoggable<A>()` (detail). Registration macros `BRIDGE_REGISTER_MODEL`, `BRIDGE_REGISTER_ACTION` (optional 4th `Loggable` argument), `BRIDGE_REGISTER_VALIDATOR` are defined here at file scope. |
| `core/backend.hpp` | `LocalBackend` (public) + `ActionCall`, `IBackend` (detail), including the non-breaking `registerModelWithContext()` default method |
| `core/remote.hpp` | `RemoteServer` (with `setLogProvider()`), `SimulatedRemoteBackend` (`morph::backend::`) |
| `core/bridge.hpp` | `Bridge`, `BridgeHandler<M>` (public) + `HandlerBinding` (carrying `contextKey`), `MemberPointerTraits` (detail) |
| `core/wire.hpp` | `Envelope`, `encode`/`decode`, `kMaxEnvelopeBytes` (`morph::wire::`) — the JSON wire envelope and length-bounded framing between any client and `RemoteServer` |

#### `journal/` — ordered, replayable action log

| Header | Responsibility |
|---|---|
| `journal/action_log.hpp` | `LogEntry`, `IActionLog`, `InMemoryActionLog`, `toJson`/`fromJson`, `SerializationError`, `setActionLog`, `defaultActionLog`, `ScopedActionLog` (`morph::journal::`) — the durable-sink interface and the process-wide default sink, with zero dependency on `core/model.hpp`/`core/registry.hpp` |
| `journal/journal.hpp` | `SessionLog`, `replay()` (`morph::journal::`) — full-fidelity session log with `checkpoint()` coalescing and `undoLast()`, built on `action_log.hpp` + the existing `ActionDispatcher`/`ModelRegistryFactory` |
| `journal/file_action_log.hpp` | `FileActionLog` (`morph::journal::`) — append-only NDJSON `IActionLog`, `flush()` fsyncs |

#### `offline/` — connectivity + replay

| Header | Responsibility |
|---|---|
| `offline/network_monitor.hpp` | `NetworkMonitorConfig`, `NetworkMonitor` — background probe thread, online/offline state machine |
| `offline/offline_queue.hpp` | `IOfflineQueue`, `QueueItem`, `InMemoryOfflineQueue` — durable write queue abstraction |
| `offline/sync_worker.hpp` | `SyncWorker`, `SyncResult` — drains offline queue on reconnect via caller-supplied replay |
| `offline/reconnect_coordinator.hpp` | `ReconnectCoordinator`, `ReconnectOutcome`, `ReconnectCoordinatorConfig` — sequences reconnect → activate → bind → replay on connectivity return; all side effects injected via `Deps` |

#### `session/` — per-call context + authentication

| Header | Responsibility |
|---|---|
| `session/session.hpp` | `Context`, `IAuthorizer`, `AllowAllAuthorizer`, `allowAllAuthorizer`, `current` (`morph::session::`) — per-call session bag carried through the bridge to the model, plus the server-side authorization seam; internals in `morph::session::detail` (`ScopedContext`, thread-local current context) |
| `session/session_auth.hpp` | `SessionToken`, `TokenIssuer`, `TokenVerifier`, `SigningAuthorizer`, `MacFunction`, `hmacSha256`, `AuthError` (`morph::session::`) — opt-in signed bearer tokens and a verifying `IAuthorizer`; self-contained reference HMAC-SHA256 in `morph::session::detail`. See `docs/spec/security.md`. |

#### `forms/` — JSON-Schema generation for auto-built GUIs

| Header | Responsibility |
|---|---|
| `forms/forms.hpp` | `schemaJson<A>()`, `allRequiredEngaged()`, `EmptyCapableField` (`morph::forms::`) — JSON Schema per action with derived `required`, `x-decimalPlaces`, `x-order`, `x-options*` |
| `forms/choice.hpp` | `Choice<T, "ListX", "id", "name">`, `FixedString`, `isChoice` (`morph::forms::`) — a field whose options are the result of executing the named action; surfaces as `x-optionsAction`/`x-optionValue`/`x-optionLabel` in schemas, renders as a combo box |

#### `util/` — exact values, units, time

| Header | Responsibility |
|---|---|
| `util/rational.hpp` | `Rational`, `DecimalPlaces`, `RationalError` (`morph::math::`) — exact int64 rational arithmetic with a decimal-precision tag; Glaze wire codec (`{"num","den","dp"}`, canonicalised on read) and `std::formatter` |
| `util/quantity.hpp` | `Quantity<U>`, `UnitMeta`, `UnitTraits` (`morph::units::`) — unit-tagged optional value over `Rational`; units are application enum NTTPs, schemas get `ExtUnits` automatically. See `docs/spec/util/quantity_type.md` for the full design. |
| `util/datetime.hpp` | `DateTime`, `Timestamp` (`morph::time::`) — UTC instant (ms precision) with a strict ISO-8601 wire codec (malformed input is a read *error*) and the optionally-empty field wrapper; schemas carry `"format": "date-time"` |

### Qt integration headers (`include/morph/qt/`)

| Header | Responsibility |
|---|---|
| `qt_executor.hpp` | `QtExecutor` — posts callables via `QMetaObject::invokeMethod` |
| `qt_websocket_backend.hpp` | `QtWebSocketBackend` — WebSocket client; implements `IBackend` |
| `qt_websocket_server.hpp` | `QtWebSocketServer` — QObject server; forwards messages to `RemoteServer` |

## Known limitations

### `RemoteServer` must be heap-allocated

`RemoteServer::handle()` captures `shared_from_this()` to prevent a use-after-free if the worker pool outlives the server. This means `RemoteServer` **must** be created via `std::make_shared<morph::backend::RemoteServer>(...)`. Constructing it on the stack and calling `handle()` will throw `std::bad_weak_ptr` at runtime.

### `NetworkMonitor` callbacks must not block

Callbacks (`onOffline`, `onOnline`) run directly on the probe thread. A blocking call inside a callback will delay or prevent subsequent probes, and a blocking `stop()` call from within a callback will self-deadlock on the thread join. `stop()` detects this case and detaches instead of joining; the monitor thread completes its current iteration and exits, and the destructor spin-waits until it does. The intent is that callbacks should be short — typically just setting an atomic flag or posting to an executor.

### `Bridge::switchBackend` must not be called from `onBackendChanged`

`switchBackend` holds `Bridge::_mtx` for its entire duration and calls `notifyBackendChanged()` while still holding it. `onBackendChanged()` is invoked from inside that call. If an `onBackendChanged()` implementation calls `switchBackend` or `registerHandler` / `deregisterHandler` (which also acquire `_mtx`), the thread will self-deadlock. `executeVia` is safe to call from `onBackendChanged()` because it uses a lock-free snapshot of the backend.

### A null callback executor drops the callback (but not the orphan log)

The `cbExec` pointer on `CompletionState` should be set before `setValue` / `setException` is called (or before `attachThen` / `attachOnError` if the state is already ready). If `cbExec` is null when a callback would fire, that callback is silently dropped — no error is raised. This is enforced by the `Completion<T>` constructor, which accepts an `IExecutor*`; the hazard only arises when using `CompletionState` directly (an internal type).

An abandoned **error**, however, is not silenced by a null executor: `onErrAttached` (the flag that suppresses the destructor's orphan log) is only set when an executor actually exists to deliver the handler. With a null executor the error handler is never posted, so `onErrAttached` stays `false` and `~CompletionState` still logs the exception through the orphan logger rather than losing it.

### `MainThreadExecutor::runFor` does not drain on timeout

If `runFor(timeout)` returns because the timeout expired rather than because the queue emptied, any remaining tasks stay enqueued. A subsequent `runFor` call will process them. This is intentional — `runFor` is a pump, not a flush.

## Versioning & compatibility

morph is `0.1.0` and pre-1.0: per [Semantic Versioning](https://semver.org/)'s
own rule for major version `0`, any release may still change anything without
a major bump. The semantic-versioning, stable-surface, and deprecation-window
commitment morph makes **starting at 1.0** is fully specified in
`docs/spec/VERSIONING.md` — this section is only a
pointer, not a substitute. In short: the per-topic public namespaces above
(everything outside a `detail` namespace) are the stable surface; because
morph is header-only there is no ABI to preserve, so the promise is *source*
compatibility, checked against both a symbol's signature and its `docs/spec/`
documented behavior.

## Key design decisions

| Decision | Rationale |
|---|---|
| Header-only library | Zero build-system friction; include and use. |
| Per-topic public namespaces with per-topic `detail::` | Minimal public surface — callers see only what they need; internals are clearly walled off. |
| `StrandExecutor` per `ModelId` | Parallelism across models; serial within one model — model authors write single-threaded code. |
| `Completion<T>` not `std::future<T>` | Callbacks marshal to a specific executor; futures do not. |
| `IBackend` in `detail::` | Users never type the interface — they construct concrete backends and let conversion happen implicitly. |
| `HandlerBinding` with atomic `currentId` | Handlers survive backend replacement without re-registering from application code. |
| `LogLevel : uint8_t` | Minimises storage; 5 levels fit in one byte. |
| Glaze for JSON | Reflects aggregate types automatically; no hand-written serialisation per action. |
| `CompletionState<T>` internal only | Keeps the public API free of state-handling machinery; implementation can change without breaking callers. |
| JSON `Envelope` wire protocol | Self-describing and forward-compatible (unknown keys ignored); carries a `callId` so async WebSocket replies can be correlated back to pending `Completion` objects. |
| Subscriptions keyed on the result type, scoped to the instance | A subscriber is a renderer: it names the state it draws, not the actions that produce it, so a new action never breaks it. Scoping to the instance is what lets two screens on one shared model see each other's work without a query-invalidation vocabulary. |
| `ActionValidator<A>` is action-typed, not model-typed | Different actions on the same model have different readiness requirements; pinning the predicate to the action keeps GUI code oblivious to model internals. |
| `set<auto FieldPtr>(value)` over `set<Action>(&Action::f, value)` | Member-pointer NTTP encodes both the action type and the field type; the call site stays terse without losing type safety. |
| `Rational` wire codec canonicalises on read | Wire input is untrusted; every deserialised value passes the reducing constructor, so invariants hold no matter what a client sends. |
| One optionality: empty state inside `Quantity` | Drafts and lab data genuinely have "no value yet" with the unit still known; a second `std::optional` wrapper would split one concept across two types. |
| Units are enum NTTPs with app-defined algebra | Mixing units is a compile error and result units are deduced, while morph stays domain-agnostic — the application owns the enum, metadata, and algebra. |
| `required` derived from types + one opt-out list | The same declaration drives the schema, the client submit gate, and `validate()` — required-ness cannot drift between server and GUI. |
| Combo-box options declared as an action reference (`Choice<T, "ListX">`) | Option lists are living data, so the single source is the action that serves them; the schema only carries the *reference*, and every renderer resolves it through the same dispatch seam as submits. |
| Strict (non-clamping) datetime codec | `Rational` clamps hostile input because any int pair still denotes a value; a malformed timestamp denotes nothing — rejecting the read beats fabricating an epoch. |
