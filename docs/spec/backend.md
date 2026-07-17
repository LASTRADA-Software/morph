# The `morph::backend` types — design

`morph::backend` provides the execution backends that own model instances and
dispatch actions against them. The namespace is split into two header files:

- **`backend.hpp`** — `ActionCall`, `IBackend`, error types, `LocalBackend`.
- **`remote.hpp`** — `RemoteServer`, `SimulatedRemoteBackend`.

`Bridge` (in `bridge.hpp`) holds one active backend at a time and can swap it
atomically via `Bridge::switchBackend()`. Every backend follows the same
contract: register and deregister models, dispatch actions, cancel pending work,
and react to backend changes.

## Contents

- [The dispatch struct — `ActionCall`](#the-dispatch-struct--actioncall)
- [The abstract interface — `IBackend`](#the-abstract-interface--ibackend)
- [Error types](#error-types)
- [`LocalBackend` — in-process execution](#localbackend--in-process-execution)
- [`RemoteServer` — server-side message handler](#remoteserver--server-side-message-handler)
- [`SimulatedRemoteBackend` — adapter for testing](#simulatedremotebackend--adapter-for-testing)
- [Lifetime & ownership](#lifetime--ownership)
- [Failure modes](#failure-modes)
- [Thread context](#thread-context)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Cross-references](#cross-references)
- [Limitations](#limitations)

## The dispatch struct — `ActionCall`

`detail::ActionCall` bundles everything needed to dispatch one action. It has
three callables — one for each execution path — so the same `ActionCall` can be
used locally or serialised for a remote round-trip:

| Field | Type | Purpose |
|---|---|---|
| `modelTypeId` | `std::string` | String id of the target model type (from `ModelTraits`). |
| `actionTypeId` | `std::string` | String id of the action type (from `ActionTraits`). |
| `serializeAction` | `std::function<std::string()>` | Serialises the action to JSON. Only called on the remote path. |
| `deserializeResult` | `std::function<std::shared_ptr<void>(std::string_view)>` | Deserialises a JSON reply into the opaque result. Only called on the remote path. |
| `localOp` | `std::function<std::shared_ptr<void>(IModelHolder&)>` | Executes the action directly against a model holder. Only called on the local path. |
| `session` | `morph::session::Context` | Session context. Local backends thread it through a thread-local before invoking `localOp`; remote backends serialise it into the wire envelope. |

## The abstract interface — `IBackend`

`detail::IBackend` is the abstract interface every backend implements. `Bridge`
holds a `unique_ptr<IBackend>` and delegates all model operations to it.

| Method | Purpose |
|---|---|
| `registerModel(typeId, factory)` | Registers a new model instance, returns its opaque `ModelId`. |
| `registerModelWithContext(typeId, factory, contextKey)` | Same as `registerModel`, additionally passes a stable identity (e.g. account id). Default implementation drops `contextKey` and forwards to `registerModel` — correct for `LocalBackend` where the factory closure already captures identity. `SimulatedRemoteBackend` overrides to carry `contextKey` across the wire. |
| `deregisterModel(mid)` | Removes the model identified by `mid`. |
| `execute(mid, call, cbExec)` | Dispatches `call` against the model identified by `mid`. Returns a `Completion<std::shared_ptr<void>>`. |
| `notifyBackendChanged()` | Called by `Bridge::switchBackend()` after all handlers are re-registered. |
| `cancelPending(exc)` | Resolves every still-pending completion with `exc`. Called on the outgoing backend during `switchBackend()` and in `Bridge`'s destructor. After this call, any later `setValue`/`setException` on those states is a no-op. |
| `setReconnectHandler(handler)` | Installs a callback invoked when the backend reconnects to its peer. Used by backends with transport (e.g. `QtWebSocketBackend`). Default implementation is a no-op. |

## Error types

Three exception types are thrown into in-flight `Completion`s:

| Type | Trigger | Purpose |
|---|---|---|
| `BackendChangedError` | `Bridge::switchBackend()` runs | GUI can retry on the new backend or surface a "backend changed" message. |
| `BridgeDestroyedError` | `Bridge` is destroyed | In-flight completions are cancelled because the bridge is gone. |
| `DisconnectedError` | Transport drops mid-call (e.g. WebSocket disconnect) | Framework retries the call on reconnect if the backend supports it; otherwise the GUI's `.onError(...)` runs. |

## `LocalBackend` — in-process execution

`LocalBackend` is the concrete in-process backend. It owns a
`StrandExecutor` (wrapping the `IExecutor&` worker pool, typically a
`ThreadPoolExecutor`) and a `ModelId → shared_ptr<IModelHolder>` map.

**Lifecycle:**
- `registerModel` — atomically increments a counter, locks the registry mutex,
  calls the factory, stores the holder, returns the new `ModelId`.
- `deregisterModel` — locks the registry mutex, erases the entry.
- `execute` — looks up the holder under the registry lock; if `mid` is unknown
  it immediately resolves the completion with
  `std::runtime_error("model not found: id=<n>")`. Otherwise it tracks the
  completion in the pending list, posts `localOp` on the model's strand
  (serialised per-model), sets up a `ScopedContext` (from `call.session`) before
  calling `localOp`, and returns the `Completion`.
- `cancelPending` — snapshots the pending list under the pending mutex, delivers
  `exc` to every still-live state.
- `notifyBackendChanged` — iterates all models, calls `onBackendChanged()` on
  every holder that implements `IBackendChangedSink`.
- `setReconnectHandler` — no-op (no transport to reconnect).

Each model instance gets its own strand so actions are serialised per-model
without a global lock on the pool.

## `RemoteServer` — server-side message handler

`RemoteServer` receives JSON envelopes (`morph::wire::Envelope`) from any
transport and executes the corresponding model operations via an
`ActionDispatcher`. Authorization is delegated to an `IAuthorizer` that defaults
to allow-all. It derives from `std::enable_shared_from_this<RemoteServer>`.

**Must be heap-allocated via `std::make_shared`.** `handle()` captures
`shared_from_this()` to prevent use-after-free when the worker pool outlives the
server.

**Wire format.** All requests and replies are JSON `morph::wire::Envelope`. The
`kind` field discriminates three message types:

| `kind` | Request fields | Reply | Notes |
|---|---|---|---|
| `register` | `typeId`, `[contextKey]` | `ok` with `modelId` (body empty) | Creates a model via the `ModelRegistryFactory`. Empty `typeId` → `err "register requires a typeId"`. If `contextKey` is non-empty, consults the `LogProvider` (if set) and, when it returns a non-null log, calls `holder->attachActionLog(log, contextKey)`. |
| `deregister` | `modelId` | `ok` | Erases the model from the registry. |
| `execute` | `modelId`, `modelType`, `actionType`, `body`, `session` | `ok` with `body` or `err` | See the execute flow below. |

**Execute flow (`dispatchExecute`).** In order:

1. **Authorize.** `_authorizer->authorize(env.session, env.modelType, env.actionType)`.
   Denied → `err "unauthorized"` (with the request's `callId`), no dispatch.
2. **Authenticate / make the principal authoritative.** After `authorize`
   succeeds, the server calls `_authorizer->authenticate(env.session)`. If it
   returns a value, the server **overwrites** `env.session.principal` with that
   verified principal *before* building the `ScopedContext`. So model code that
   reads `session::current()->principal` on the remote path sees the identity the
   authorizer extracted from a valid token, not the client's asserted claim
   (`Context::principal` is untrusted wire input on its own). A non-verifying
   authorizer — including the default `AllowAllAuthorizer` — returns `nullopt`
   from `authenticate` and the client-supplied principal is left unchanged. This
   is the only place the principal is made authoritative; the verifying
   implementation lives in `SigningAuthorizer` (`session_auth.hpp`, cross-ref
   security.md). The rewrite happens on the calling/pool thread, before the
   strand task is posted.
3. **Look up the model.** Under `_regMtx`, find `env.modelId`. Missing →
   `err "model not found"` (with `callId`), no dispatch. (Note: the remote
   message is the bare string `"model not found"`, without the id — unlike the
   `LocalBackend` path, which resolves the completion with
   `std::runtime_error("model not found: id=<n>")`.)
4. **Dispatch on the strand.** Posts to the model's strand a task that installs a
   `ScopedContext` from the (now possibly rewritten) `env.session`, calls
   `ActionDispatcher::dispatch(modelType, actionType, *holder, body)`, and replies
   `ok` with the serialised result. Any `std::exception` thrown by the dispatch is
   caught on the strand and returned as `err exc.what()` with the `callId`.

Any envelope that fails to decode produces `err` carrying the decode
exception's message. An unrecognised `kind` produces `err "unknown envelope
kind: <kind>"`. Any `std::exception` thrown while handling a decoded envelope is
caught and returned as an `err` reply carrying `exc.what()` and the request's
`callId`.

**`handle(msg, reply)`** — asynchronous entry point. Posts to the worker pool,
calls `dispatchMessage` which decodes, dispatches by `kind`, and calls `reply`
exactly once.

**`handleInline(msg)`** — synchronous entry point intended for control messages
(`register`, `deregister`) only. It runs `dispatchMessage` directly on the
calling thread instead of posting the message to the worker pool, so it is safe
to call from a thread that *is* the worker pool. It **rejects `execute`** up
front: an `execute` reply is produced asynchronously on the model's strand,
*after* the synchronous call has returned and destroyed the local reply buffer
the deferred callback would write into, so `handleInline` decodes the envelope
first and, if its `kind` is `execute`, returns an `err` reply
(`"handleInline does not support execute (reply is asynchronous)"`) without
dispatching. A malformed envelope falls through to `dispatchMessage`, which
emits the canonical decode-error reply.

**`setLogProvider(provider)`** — installs a `LogProvider` callable consulted on
every `register` envelope whose `contextKey` is non-empty. This is how
`RemoteServer` attaches action logs to model instances created on behalf of
remote clients — the factory closure (which lives on the client side) cannot
capture the server-side log. Thread-safe.

```cpp
using LogProvider = std::function<std::shared_ptr<morph::journal::IActionLog>(
    std::string_view modelType, std::string_view contextKey)>;
```

## `SimulatedRemoteBackend` — adapter for testing

`SimulatedRemoteBackend` implements `IBackend` by forwarding all calls through
a `RemoteServer`. Control operations (`registerModel`, `registerModelWithContext`,
`deregisterModel`) are forwarded synchronously via `RemoteServer::handleInline`;
`execute` is forwarded asynchronously via `RemoteServer::handle` and returns a
`Completion` that resolves on the server's reply. Intended for testing and
in-process simulation of remote execution.

**Key design choices:**
- `registerModel` and `registerModelWithContext` use `handleInline` (synchronous
  control message) — the `factory` argument is ignored because model construction
  is delegated to the server's `ModelRegistryFactory`.
- `deregisterModel` likewise uses `handleInline`.
- `execute` serialises the action via `call.serializeAction()`, builds an
  `execute` envelope, calls `handle()` (asynchronous), and returns a `Completion`
  that resolves when the server's reply is deserialised via
  `call.deserializeResult()`.
- `notifyBackendChanged` is a no-op — models live in the `RemoteServer`, not
  locally.
- `cancelPending` snapshots and resolves pending completions, same pattern as
  `LocalBackend`.

## Lifetime & ownership

The backends hold *references*, not owning pointers, to the resources they run
on. Getting the destruction order wrong is a use-after-free, so the invariants
are:

- **The worker pool must outlive the backend.** Every backend takes an
  `IExecutor& workerPool` by reference (`LocalBackend`, `RemoteServer`) and wraps
  it in a `StrandExecutor`. The pool (typically a `ThreadPoolExecutor`) must be
  destroyed *after* the backend that references it — and, in practice, after the
  `Bridge` that owns the backend. Destroying the pool first leaves the strand
  pointing at freed storage.
- **`RemoteServer` must be created via `std::make_shared`.** It derives from
  `std::enable_shared_from_this<RemoteServer>`; `handle()` captures
  `shared_from_this()` into the pool task. Constructing it on the stack and
  calling `handle()` throws `std::bad_weak_ptr` (see ARCHITECTURE.md
  "RemoteServer must be heap-allocated").
- **The `RemoteServer` shared_ptr must outlive every referencing
  `SimulatedRemoteBackend` (and every transport `QtWebSocketServer` etc.).**
  `SimulatedRemoteBackend` stores `RemoteServer& _server` — a non-owning
  reference — and forwards every `registerModel` / `deregisterModel` / `execute`
  through it. If the server's owning shared_ptr is released while a backend still
  references it, subsequent calls dereference a dangling reference. The `handle()`
  path is self-protecting for tasks already *in flight* (each captures a
  shared_ptr copy), but `SimulatedRemoteBackend`'s reference member is not — the
  caller must keep the server alive for the backend's whole lifetime.
- **Pending strand tasks capture shared_ptr copies, so model destruction
  mid-flight is safe.** Both backends' `execute` strand tasks capture the model
  `holder` by `shared_ptr` copy (and `RemoteServer`'s also captures the reply
  callback and the moved `Envelope`). A `deregisterModel` that erases the map
  entry while a task is queued or running only drops the *map's* reference; the
  in-flight task holds its own, so the holder stays alive until the task
  completes. `RemoteServer`'s pool tasks additionally keep the server itself
  alive via `shared_from_this()`.

## Failure modes

| Situation | Local (`LocalBackend`) | Remote (`RemoteServer` / `SimulatedRemoteBackend`) |
|---|---|---|
| `register` with an unregistered `typeId` | N/A — the local factory closure constructs the instance directly; there is no registry lookup and no type-id failure. | `ModelRegistryFactory::create(typeId)` fails → the catch in `dispatchMessage` replies `err "unknown model type: <typeId>"`. Remote registration therefore requires the model to have been macro-registered with `BRIDGE_REGISTER_MODEL`. `SimulatedRemoteBackend::registerModelWithContext` turns that `err` into a thrown `std::runtime_error("register failed: unknown model type: <typeId>")`. |
| `register` with an empty `typeId` | N/A | `err "register requires a typeId"`. |
| `execute` against an unknown model id | Completion resolves with an **untyped** `std::runtime_error("model not found: id=<n>")`. | `err "model not found"` (bare, no id); `SimulatedRemoteBackend` surfaces it as a thrown/`onError` `std::runtime_error("model not found")`. |
| Action handler throws | Caught on the strand; completion resolves with the thrown exception. | Caught on the strand; `err exc.what()` reply, which the client re-throws into the completion. |
| Envelope fails to decode | N/A | `err <decode exception message>` (no `callId` echoed — it couldn't be parsed). |
| Unrecognised `kind` | N/A | `err "unknown envelope kind: <kind>"`. |

There is **no typed "model not found" exception** on either path — callers that
need to distinguish it from any other `std::runtime_error` have only the message
string to go on, and the local and remote messages differ (see the table). The
typed error hierarchy (`BackendChangedError`, `BridgeDestroyedError`,
`DisconnectedError`) covers only lifecycle/transport cancellation, not
per-call dispatch failures.

## Thread context

An `ActionCall`'s three callables run on three different threads across a remote
round-trip; model and GUI authors must not assume any two share a thread:

| Callable | Runs on |
|---|---|
| `serializeAction` | The **calling / GUI thread** — `SimulatedRemoteBackend::execute` invokes it synchronously while building the envelope, before handing off to the pool. |
| `deserializeResult` | The **reply / pool thread** — invoked inside the `handle()` reply callback when the server's `ok` arrives (for `SimulatedRemoteBackend`, that is a `RemoteServer` worker-pool thread). |
| `localOp` | The **model strand** (`LocalBackend` only) — posted on the per-`ModelId` `StrandExecutor`, serialised against other actions for the same model. Never invoked on the remote path. |

On the server side, `RemoteServer` runs authorize/authenticate and the model
lookup on the pool thread that `dispatchMessage` runs on, then runs
`ActionDispatcher::dispatch` (and the `ScopedContext`) on the model strand.
Completion *callbacks* (`.then`/`.onError`) are delivered via the `cbExec`
executor passed to `execute`, independent of all of the above.

## API reference

### `detail::ActionCall`

| Member | Type | Notes |
|---|---|---|
| `modelTypeId` | `std::string` | Target model type id. |
| `actionTypeId` | `std::string` | Target action type id. |
| `serializeAction` | `std::function<std::string()>` | JSON serialiser; remote path only. |
| `deserializeResult` | `std::function<std::shared_ptr<void>(std::string_view)>` | JSON deserialiser; remote path only. |
| `localOp` | `std::function<std::shared_ptr<void>(IModelHolder&)>` | Direct execution; local path only. |
| `session` | `morph::session::Context` | Session context for the call. |

### `detail::IBackend`

| Method | Signature | Notes |
|---|---|---|
| `registerModel` | `virtual ModelId registerModel(const string&, function<unique_ptr<IModelHolder>()>)` | Pure virtual. |
| `registerModelWithContext` | `virtual ModelId registerModelWithContext(const string&, function<unique_ptr<IModelHolder>()>, string_view)` | Default: drops `contextKey`, calls `registerModel`. |
| `deregisterModel` | `virtual void deregisterModel(ModelId)` | Pure virtual. |
| `execute` | `virtual Completion<shared_ptr<void>> execute(ModelId, ActionCall, IExecutor*)` | Pure virtual. |
| `notifyBackendChanged` | `virtual void notifyBackendChanged()` | Pure virtual. |
| `cancelPending` | `virtual void cancelPending(const exception_ptr&)` | Pure virtual. |
| `setReconnectHandler` | `virtual void setReconnectHandler(const function<void()>&)` | Default: no-op. |

### Error types

| Type | Base | Message |
|---|---|---|
| `BackendChangedError` | `std::runtime_error` | `"backend changed before completion resolved"` |
| `BridgeDestroyedError` | `std::runtime_error` | `"bridge destroyed before completion resolved"` |
| `DisconnectedError` | `std::runtime_error` | `"transport disconnected before completion resolved"` |

### `LocalBackend`

| Method | Notes |
|---|---|
| `explicit LocalBackend(IExecutor& workerPool)` | Constructs with a strand around `workerPool`. |
| `registerModel(typeId, factory)` | Atomically increments `_nextId`, stores the holder under `_regMtx`. `typeId` is accepted for interface compatibility but not used. |
| `deregisterModel(mid)` | Erases from `_models` under `_regMtx`. |
| `notifyBackendChanged()` | Calls `onBackendChanged()` on `IBackendChangedSink` holders. |
| `execute(mid, call, cbExec)` | Posts `call.localOp` on the model's strand with `ScopedContext`. Returns a `Completion`. |
| `cancelPending(exc)` | Snapshots `_pending`, delivers `exc` to each live state. |

### `RemoteServer`

| Method | Notes |
|---|---|
| `RemoteServer(workerPool, dispatcher, registry)` | Allow-all authorizer. |
| `RemoteServer(workerPool, authorizer, dispatcher, registry)` | Custom authorizer; null → allow-all. |
| `handle(msg, reply)` | Async: posts to pool, decodes, dispatches, calls `reply` once. Thread-safe. |
| `handleInline(msg)` | Sync: runs `dispatchMessage` on the calling thread and returns the reply JSON; intended for `register`/`deregister` only. **Rejects `execute`** — returns an `err` reply without dispatching, because an `execute` reply is produced asynchronously after this call returns. |
| `setLogProvider(provider)` | Installs a `LogProvider`; `nullptr` clears. Thread-safe. |

### `SimulatedRemoteBackend`

| Method | Notes |
|---|---|
| `explicit SimulatedRemoteBackend(RemoteServer& server)` | References the server. |
| `registerModel(typeId, factory)` | Delegates to `registerModelWithContext(typeId, {}, {})`. |
| `registerModelWithContext(typeId, factory, contextKey)` | Sends `register` envelope via `handleInline`. `factory` ignored. |
| `deregisterModel(mid)` | Sends `deregister` envelope via `handleInline`. |
| `execute(mid, call, cbExec)` | Serialises, sends `execute` via `handle`, returns `Completion` that resolves on reply. |
| `notifyBackendChanged()` | No-op. |
| `cancelPending(exc)` | Snapshots `_pending`, delivers `exc` to each live state. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Dual-path `ActionCall` | Three callables: `localOp`, `serializeAction`, `deserializeResult` | The same `ActionCall` struct works for both local and remote execution without an `if (isRemote)` branch at the call site — each backend uses the field(s) it needs. |
| `registerModelWithContext` | Virtual with a default that drops `contextKey` | `LocalBackend`'s factory closure already captures identity, so there is nothing to forward. `SimulatedRemoteBackend` overrides to carry `contextKey` across the wire so the server's `LogProvider` can attach an action log. |
| `RemoteServer` heap requirement | `std::enable_shared_from_this` | `handle()` posts to the worker pool capturing `shared_from_this()` — the server must outlive any in-flight message. |
| `handleInline` | Synchronous; caller-restricted to control messages | Safe to call from a worker-pool thread (e.g. from a `BridgeHandler` constructor). It is meant for `register`/`deregister` only; an `execute` envelope is rejected with an `err` reply, because `dispatchExecute` posts to the strand and would reply after `handleInline` returns (writing into an already-destroyed reply buffer). The rejection is now enforced by the code, matching the documented intent. |
| `SimulatedRemoteBackend` factory ignored | Model construction delegated to `RemoteServer`'s `ModelRegistryFactory` | The factory closure lives on the client side; the server owns the actual instances. |
| `cancelPending` snapshots | Weak-ptr snapshot under lock, then resolves outside | Avoids holding the lock while delivering exceptions to each state, preventing deadlock if a callback re-enters the backend. |
| `setReconnectHandler` | Default no-op | Only backends with a transport layer (e.g. `QtWebSocketBackend`) need to react to reconnects. `LocalBackend` and `SimulatedRemoteBackend` never invoke it. |
| Strand-per-model | `StrandExecutor` serialises actions per `ModelId` | Actions against the same model run sequentially; different models can run in parallel. No global lock on the pool. |
| Overwrite `session.principal` on remote execute | `authenticate()` result replaces the client claim before dispatch | The client-asserted `Context::principal` is untrusted; a verifying authorizer makes the token-derived identity authoritative so `session::current()->principal` inside a model is trustworthy. Non-verifying authorizers return `nullopt` and change nothing. |

## Cross-references

| Spec | Relationship |
|---|---|
| bridge.md | `Bridge` owns one `IBackend` and swaps it via `switchBackend()`; `BridgeHandler`/`HandlerBinding` carry the `contextKey` that reaches `registerModelWithContext`. `executeVia` builds the `ActionCall`. |
| session.md | `Context`, `IAuthorizer::authorize`/`authenticate`, `ScopedContext`, `session::current()`. The principal-overwrite contract is specified there and enforced here. |
| security.md | Threat model for `RemoteServer`: authorization coverage, the untrusted client principal, and what `register`/`deregister` do *not* check. |
| wire.md | `Envelope`, `encode`/`decode`, `makeOk`/`makeErr`/`makeRegister`/`makeDeregister`, and the `kind` discriminator the server switches on. |
| registry.md | `ModelRegistryFactory::create` (remote model construction, `BRIDGE_REGISTER_MODEL`), `ActionDispatcher::dispatch` (the remote execute call site), and the `Loggable` policy. |
| completion.md | `Completion<shared_ptr<void>>` returned by `execute`, the `CompletionState` the backends track for `cancelPending`, and `cbExec` callback delivery. |

## Limitations

- **Local and remote are not fully interchangeable.** The GUI-facing API is
  identical, but the two paths construct models differently. `LocalBackend` runs
  the caller-supplied **factory closure**, which can capture arbitrary
  dependencies and need not be default-constructible. `RemoteServer` ignores the
  factory and constructs via the `ModelRegistryFactory`, which requires the model
  to be **default-constructible and macro-registered** (`BRIDGE_REGISTER_MODEL`).
  A model that works locally can therefore fail at remote `register` with
  `err "unknown model type: ..."` (or fail to compile the registration if it is
  not default-constructible). Parity between the two paths is a property of the
  model, not something the framework guarantees.
- **Remote model ids are guessable and register/deregister are unauthorized.**
  `RemoteServer` assigns model ids from a sequential `std::atomic<uint64_t>`
  counter (`_nextId + 1`), so ids are trivially guessable. Only `execute` runs
  through the `IAuthorizer`; `register` and `deregister` are **not** authorized
  at all — any client that can reach the transport can create and destroy model
  instances. See security.md.
- **`notifyBackendChanged` uses RTTI over every model under the registry lock.**
  `LocalBackend::notifyBackendChanged` iterates the entire `_models` map holding
  `_regMtx` and `dynamic_cast`s each holder to `IBackendChangedSink`. This
  depends on RTTI being enabled and its cost scales with the number of live
  models while the registry lock is held. (`SimulatedRemoteBackend`'s override is
  a no-op — its models live in the server.)
- **Stale `SimulatedRemoteBackend` header doc.** The class comment in
  `remote.hpp` still describes a "synchronous request-reply protocol" where "the
  calling thread blocks until the reply arrives via `std::promise`". The code
  does not match: `execute` is **asynchronous** (it calls `handle()` and returns
  a `Completion`), and there is no `std::promise` anywhere in the class — only
  the control operations (`registerModel`/`deregisterModel`) are synchronous, via
  `handleInline`. Treat this spec, not that comment, as authoritative.