# morph Architecture

## Overview

`morph` is a typed, asynchronous bridge between a GUI thread and business-object models. Models may live in-process (local mode) or in a remote server process (remote mode). The GUI code is identical in both cases — only the backend implementation changes.

The framework is header-only (C++23, namespace `morph`), depends on Glaze for JSON reflection, and optionally integrates with Qt 6 via a separate target.

## Namespace map

The public surface is split per topic so callers always know whether a name is part of the stable API or an implementation detail.

| Namespace | Purpose | Public symbols |
|---|---|---|
| `morph::` | Macros only at this level | `BRIDGE_REGISTER_MODEL`, `BRIDGE_REGISTER_ACTION`, `BRIDGE_REGISTER_VALIDATOR` (at file scope, but specialise `morph::model::*` templates) |
| `morph::log` | Configurable logging | `LogLevel`, `setLogger`, `setLogLevel`, `getLogLevel`, `logDebug`, `logInfo`, `logWarn`, `logError` |
| `morph::exec` | Executor primitives | `IExecutor`, `ThreadPoolExecutor`, `MainThreadExecutor` |
| `morph::async` | Async result handle | `Completion<T>` |
| `morph::model` | Model & action traits | `ModelTraits<>`, `ActionTraits<>`, `ActionValidator<>` |
| `morph::backend` | Pluggable backends | `LocalBackend`, `RemoteServer`, `SimulatedRemoteBackend` |
| `morph::bridge` | Bridge between handler and backend | `Bridge`, `BridgeHandler<M>` |
| `morph::offline` | Connectivity + replay | `NetworkMonitor`, `NetworkMonitorConfig`, `IOfflineQueue`, `QueueItem`, `InMemoryOfflineQueue`, `SyncWorker`, `SyncResult` |
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

## Header map

### Core headers (`include/morph/`)

| Header | Responsibility |
|---|---|
| `logger.hpp` | `LogLevel`, log configuration and level helpers; internals in `morph::log::detail` |
| `executor.hpp` | `IExecutor`, `ThreadPoolExecutor`, `MainThreadExecutor` (`morph::exec::`) |
| `strand.hpp` | `ModelId`, `ModelIdHash`, `StrandExecutor` — serialises tasks per model (`morph::exec::detail::`) |
| `completion.hpp` | `CompletionState<T>` (detail) + `Completion<T>` (public) — result handle |
| `model.hpp` | `IModelHolder`, `ModelHolder<T>`, `ModelFactory`, `IBackendChangedSink`, `BackendChangedNotifiable` — type-erased model storage (`morph::model::detail::`) |
| `registry.hpp` | `ModelTraits<>`, `ActionTraits<>`, `ActionValidator<>` (public) + `ActionDispatcher`, `ModelRegistryFactory`, `defaultDispatcher()`, `defaultRegistry()`, `ParseError`, `registerModelOnce`, `registerActionOnce` (detail). Registration macros `BRIDGE_REGISTER_MODEL`, `BRIDGE_REGISTER_ACTION`, `BRIDGE_REGISTER_VALIDATOR` are defined here at file scope. |
| `backend.hpp` | `LocalBackend` (public) + `ActionCall`, `IBackend` (detail) |
| `remote.hpp` | `RemoteServer`, `SimulatedRemoteBackend` (`morph::backend::`) |
| `bridge.hpp` | `Bridge`, `BridgeHandler<M>` (public) + `HandlerBinding`, `MemberPointerTraits` (detail) |
| `network_monitor.hpp` | `NetworkMonitorConfig`, `NetworkMonitor` — background probe thread, online/offline state machine |
| `offline_queue.hpp` | `IOfflineQueue`, `QueueItem`, `InMemoryOfflineQueue` — durable write queue abstraction |
| `sync_worker.hpp` | `SyncWorker`, `SyncResult` — drains offline queue on reconnect via caller-supplied replay |

### Qt integration headers (`include/morph/qt/`)

| Header | Responsibility |
|---|---|
| `qt_executor.hpp` | `QtExecutor` — posts callables via `QMetaObject::invokeMethod` |
| `qt_websocket_backend.hpp` | `QtWebSocketBackend` — WebSocket client; implements `IBackend` |
| `qt_websocket_server.hpp` | `QtWebSocketServer` — QObject server; forwards messages to `RemoteServer` |

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
                 └─ serialize action → backend::RemoteServer::handle (5-part text protocol)
                      └─ ActionDispatcher → StrandExecutor → Model::execute
                           └─ serialize result → Completion<T>::then → GUI executor
```

**Qt WebSocket mode** — model lives in a separate process:

```
GUI thread (Qt process)                         Server process
  └─ bridge::BridgeHandler<M>::execute(action)
       └─ bridge::Bridge::executeVia<M, A>
            └─ qt::QtWebSocketBackend::execute
                 └─ assign callId, send JSON  ──► qt::QtWebSocketServer::handle
                                                        └─ backend::RemoteServer::handle (6-part protocol)
                                                             └─ ActionDispatcher → StrandExecutor → Model::execute
                 ◄── JSON reply (ok|callId|result) ──────────────────────────────
            └─ resolve pending Completion
       └─ async::Completion<T>::then callback → qt::QtExecutor → GUI thread
```

The GUI sees the same `morph::async::Completion<T>` in all three modes.

## Wire protocol

`RemoteServer::handle` accepts JSON envelopes (`morph::wire::Envelope`). The `kind`
field is the discriminator:

| `kind` | Direction | Required fields | Meaning |
|---|---|---|---|
| `"register"` | client → server | `typeId` | Register a model instance; server replies `ok` with `modelId` |
| `"deregister"` | client → server | `modelId` | Destroy model instance; server replies `ok` |
| `"execute"`   | client → server | `callId`, `modelId`, `modelType`, `actionType`, `body`, `session` (optional) | Dispatch an action |
| `"ok"`        | server → client | `callId`, plus `body` (execute result) or `modelId` (register reply) | Success |
| `"err"`       | server → client | `callId` (echoed), `message` | Failure |

All envelopes round-trip through Glaze JSON, so the protocol is self-describing,
escaping-safe, and easy to extend (add a field, ignore unknowns).

The `session` field is a `morph::session::Context`. The server runs every
incoming `execute` envelope through its configured `IAuthorizer`; a `false`
return causes the server to reply with `err|unauthorized` (callId echoed). The
default authorizer permits everything.

## Component detail

### Executors

All concurrency runs through `morph::exec::IExecutor::post(fn)`:

- **`ThreadPoolExecutor`** — fixed N worker threads, MPMC queue; exceptions are swallowed and logged.
- **`MainThreadExecutor`** — single-threaded queue with `runFor(timeout)` drain; used in non-Qt tests to pump the "GUI" thread.
- **`QtExecutor`** — posts via `QMetaObject::invokeMethod(Qt::QueuedConnection)`; safe from any thread; drops silently if the target object is deleted.

### StrandExecutor

`morph::exec::detail::StrandExecutor` guarantees that all tasks for the same `ModelId` are serialised while tasks for different models are parallelised. Internally keeps one `std::queue` and a `running` flag per model; tasks are dispatched to the underlying `IExecutor` one at a time.

### Completion<T>

Move-only public result handle (`morph::async::Completion<T>`). Internally backed by `morph::async::detail::CompletionState<T>` (mutex-protected value + error + two callback slots). Key invariant: callbacks are always delivered via the `IExecutor` supplied at construction, so the GUI thread is never blocked and callbacks always fire on the right thread.

If a `Completion` is destroyed with an unhandled error (no `.onError` was attached), `CompletionState::~CompletionState` logs the exception through the configured logger.

### Registry & type erasure

`morph::model::ModelTraits<M>` and `morph::model::ActionTraits<A>` are specialised by registration macros. They provide:
- String type IDs for protocol routing.
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

**`atomic<uint64_t> currentId`:** Every call to `executeVia` reads `binding->currentId` to find out which backend-assigned ID to use. The value is an atomic so it can be updated during a backend switch without holding the bridge mutex for the duration of every execute call. A value of `0` is the sentinel for "not bound" — `executeVia` returns an immediate error in that case.

**Backend switching:** `Bridge::switchBackend(newBackend)` acquires the bridge mutex, re-registers every live `HandlerBinding` on the new backend (writing the new `ModelId` atomically into `binding->currentId`), replaces the active backend, then calls `notifyBackendChanged()`. `LocalBackend` forwards this to every live model that implements `IBackendChangedSink` (detected at compile time via the `BackendChangedNotifiable` concept). `SimulatedRemoteBackend` is a no-op — its models live inside `RemoteServer`.

### Logger

`morph::log` provides a global, mutex-protected, replaceable sink (`std::function<void(LogLevel, std::string_view)>`). `LogLevel` is a `uint8_t`-backed enum (`debug < info < warn < error < off`). All framework internals route through `morph::log::detail::log(level, msg)`; call `morph::log::setLogger` at startup to redirect to spdlog, Qt logging, or a test spy.

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

`drain()` returning items without removing them is deliberate — items survive a crash between `drain()` and `markDone()`. A SQL-backed implementation (not in this repository) can persist items across process restarts by storing them in a table with a UNIQUE constraint on the payload.

`InMemoryOfflineQueue` implements the interface with a `std::deque` protected by a mutex. It does not deduplicate.

### SyncWorker

`morph::offline::SyncWorker` drains an `IOfflineQueue` on reconnect. The caller supplies a `ReplayFunction` (`bool(const std::string& payload)`) that knows how to process each item — the framework has no knowledge of what replay means (insert to DB, POST to API, etc.).

- Returns `true` → `markDone` called, item removed.
- Returns `false` or throws → item left in queue for the next `run()`.
- `stop()` signals the current `run()` to abort after the current item. Resets at the start of each `run()`, so it is one-shot.
- Concurrent `run()` calls are serialised by an internal mutex.

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
| `Bridge::switchBackend` | Holds bridge mutex for full duration; re-registration and notification are atomic with respect to new `execute` calls. |

## Error propagation

```
Model::execute(action) throws
  └─ LocalBackend's strand task catches via try/catch
       └─ CompletionState::setException(current_exception())
            └─ .onError(fn) handler posted to GUI executor
                 └─ fn receives exception_ptr; caller rethrows to inspect
```

If the `Completion` is abandoned (no `.onError` attached), the destructor logs the exception. Non-`std::exception` types are logged as "unknown exception".

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

## Subscriptions and fielded actions

The framework offers two complementary surfaces for invoking actions from the GUI:

1. **One-shot**: `handler.execute(action) → Completion<R>`. The full action is built in the GUI and sent in a single call. Suitable for actions that fire on a button click ("delete this order", "submit form").
2. **Fielded / reactive**: `handler.subscribe<A>(cb)` + `handler.set<&A::field>(value)`. Field values stream into a client-side draft, a per-action validator decides when the draft is ready, and the framework dispatches `model.execute(draft)` and pushes the result to the subscriber. Suitable for forms where each widget edits one field and the GUI should respond live as the user types.

### API

```cpp
// Per-action validator — template specialisation, no Model coupling.
template <typename A>
struct morph::model::ActionValidator {
    static bool ready(const A&) noexcept { return true; }   // default: one-shot
};

// User specialises (or uses the macro) for actions with fielded readiness.
BRIDGE_REGISTER_VALIDATOR(FormAction, [](const FormAction& a) {
    return a.a != 0.0 && a.b != 0.0 && a.c != 0.0;
})
```

```cpp
morph::bridge::BridgeHandler<FormModel> handler{bridge, &guiExec};

handler.subscribe<FormAction>([](double sum) { renderTotal(sum); });

handler.set<&FormAction::a>(3.0);
handler.set<&FormAction::b>(5.0);
handler.set<&FormAction::c>(7.0);   // validator passes → execute → callback fires

handler.unsubscribe<FormAction>();
handler.reset<FormAction>();
```

### Behavior

| Aspect | Default |
|---|---|
| **Validator default** | `ActionValidator<A>::ready` returns `true` for any action without a specialisation. First `set<>` triggers a fire. |
| **Re-fire** | Every `set<>` that lands a `ready()==true` state dispatches the action again — live recomputation. The draft persists between fires. |
| **Draft persistence** | Drafts survive successive fires, `unsubscribe`, and `Bridge::switchBackend`. Destroyed with the handler or via `reset<A>()`. |
| **In-flight coalescing** | If patches land while a previous execute is in flight, exactly one re-fire is queued for when it completes — running with the latest draft snapshot. Further patches during the same flight collapse into that single pending re-fire. Matches typical reactive-UI behaviour. |
| **Subscriber cardinality** | One subscriber per `(handler, Action type)`. `subscribe<A>(cb)` replaces any prior callback. |
| **No-subscriber fire** | If `set<>` triggers an execute but no subscriber is installed, the action still runs and the result is silently dropped. |
| **Subscription thread** | Callbacks always run on the executor passed at handler construction (`guiExec`). |

## Known limitations

### `RemoteServer` must be heap-allocated

`RemoteServer::handle()` captures `shared_from_this()` to prevent a use-after-free if the worker pool outlives the server. This means `RemoteServer` **must** be created via `std::make_shared<morph::backend::RemoteServer>(...)`. Constructing it on the stack and calling `handle()` will throw `std::bad_weak_ptr` at runtime.

### `NetworkMonitor` callbacks must not block

Callbacks (`onOffline`, `onOnline`) run directly on the probe thread. A blocking call inside a callback will delay or prevent subsequent probes, and a blocking `stop()` call from within a callback will self-deadlock on the thread join. `stop()` detects this case and detaches instead of joining; the monitor thread completes its current iteration and exits, and the destructor spin-waits until it does. The intent is that callbacks should be short — typically just setting an atomic flag or posting to an executor.

### `Bridge::switchBackend` must not be called from `onBackendChanged`

`switchBackend` holds `Bridge::_mtx` for its entire duration and calls `notifyBackendChanged()` while still holding it. `onBackendChanged()` is invoked from inside that call. If an `onBackendChanged()` implementation calls `switchBackend` or `registerHandler` / `deregisterHandler` (which also acquire `_mtx`), the thread will self-deadlock. `executeVia` is safe to call from `onBackendChanged()` because it uses a lock-free snapshot of the backend.

### `CompletionState` requires a non-null executor before callbacks fire

The `cbExec` pointer on `CompletionState` must be set before `setValue` / `setException` is called (or before `attachThen` / `attachOnError` if the state is already ready). If `cbExec` is null when a callback would fire, the callback is silently discarded — no error is raised. This is enforced by the `Completion<T>` constructor, which accepts an `IExecutor*`; the hazard only arises when using `CompletionState` directly (an internal type).

### `MainThreadExecutor::runFor` does not drain on timeout

If `runFor(timeout)` returns because the timeout expired rather than because the queue emptied, any remaining tasks stay enqueued. A subsequent `runFor` call will process them. This is intentional — `runFor` is a pump, not a flush.

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
| 6-part Qt wire protocol | Carries a `callId` so async WebSocket replies can be correlated back to pending `Completion` objects. |
| Client-side drafts for fielded actions | Avoids new wire messages, server-side draft state, and a server push channel. Patches never leave the client; only the full action is sent when the validator passes. |
| `ActionValidator<A>` is action-typed, not model-typed | Different actions on the same model have different readiness requirements; pinning the predicate to the action keeps GUI code oblivious to model internals. |
| `set<auto FieldPtr>(value)` over `set<Action>(&Action::f, value)` | Member-pointer NTTP encodes both the action type and the field type; the call site stays terse without losing type safety. |
