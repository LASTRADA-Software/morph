# The `morph::backend` types — design

`morph::backend` provides the execution backends that own model instances and
dispatch actions against them. The abstraction spans several header files:

- **`backend.hpp`** — `ActionCall`, `IBackend`, error types, `LocalBackend`.
- **`remote.hpp`** — `RemoteServer`, `SimulatedRemoteBackend`.
- **`qt/qt_websocket_backend.hpp`** / **`qt/qt_websocket_server.hpp`** (namespace
  `morph::qt`) — `QtWebSocketBackend` (the client-side `IBackend` over a real
  WebSocket transport) and `QtWebSocketServer` (the transport in front of a
  `RemoteServer`). These are the concrete remote transport that gives
  `DisconnectedError`, `setReconnectHandler`, and the reconnect lifecycle their
  meaning; `SimulatedRemoteBackend` is the in-process stand-in for the same shape.
- **`net/socket_backend.hpp`** / **`net/socket_server.hpp`** (namespace
  `morph::net`, opt-in via the CMake option `MORPH_BUILD_NET`, off by default) —
  `SocketBackend` and `SocketServer`, a Qt-free reference transport that speaks
  the same RFC 6455 WebSocket framing as the Qt transport above, over raw POSIX
  (BSD) sockets. Wire-interoperable with `QtWebSocketBackend`/`QtWebSocketServer`
  — both sides round-trip the same `wire::Envelope`.

`Bridge` (in `bridge.hpp`) holds one active backend at a time and can swap it
atomically via `Bridge::switchBackend()`. Every backend follows the same
contract: register and deregister models, dispatch actions, cancel pending work,
and react to backend changes.

## Contents

- [The dispatch struct — `ActionCall`](#the-dispatch-struct--actioncall)
- [The abstract interface — `IBackend`](#the-abstract-interface--ibackend)
- [Connect/disconnect notifications](#connectdisconnect-notifications)
- [Asynchronous registration — `registerModelAsync`](#asynchronous-registration--registermodelasync)
- [Error types](#error-types)
- [`LocalBackend` — in-process execution](#localbackend--in-process-execution)
- [`RemoteServer` — server-side message handler](#remoteserver--server-side-message-handler)
- [Server-side observability](#server-side-observability)
- [`LimitPolicy` — opt-in resource limits](#limitpolicy--opt-in-resource-limits)
- [Connection scopes](#connection-scopes)
- [`SimulatedRemoteBackend` — adapter for testing](#simulatedremotebackend--adapter-for-testing)
- [`QtWebSocketBackend` — client-side WebSocket transport](#qtwebsocketbackend--client-side-websocket-transport)
- [`QtWebSocketServer` — server-side WebSocket transport](#qtwebsocketserver--server-side-websocket-transport)
- [`SocketBackend` / `SocketServer` — raw-socket WebSocket transport](#socketbackend--socketserver--raw-socket-websocket-transport)
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
| `registerModelAsync(typeId, factory, contextKey, onRegistered, onError)` | Optional non-blocking counterpart to `registerModelWithContext`. Returns `false` by default (no async path); `Bridge::registerHandler()` prefers this when it returns `true` and falls back to the synchronous call otherwise. See [Asynchronous registration](#asynchronous-registration--registermodelasync). |
| `deregisterModel(mid)` | Removes the model identified by `mid`. |
| `execute(mid, call, cbExec)` | Dispatches `call` against the model identified by `mid`. Returns a `Completion<std::shared_ptr<void>>`. |
| `notifyBackendChanged()` | Called by `Bridge::switchBackend()` after all handlers are re-registered. |
| `cancelPending(exc)` | Resolves every still-pending completion with `exc`. Called on the outgoing backend during `switchBackend()` and in `Bridge`'s destructor. After this call, any later `setValue`/`setException` on those states is a no-op. |
| `setReconnectHandler(handler)` | Installs a callback invoked when the backend reconnects to its peer. Fires only on the *second and later* connects, never the first — used by `Bridge` to re-register handlers after a drop. Used by backends with transport (e.g. `QtWebSocketBackend`). Default implementation is a no-op. |
| `setConnectHandler(handler)` | Installs a callback invoked on every successful connect, including the first — the complementary hook `setReconnectHandler` deliberately skips (see [Connect/disconnect notifications](#connectdisconnect-notifications)). Default implementation is a no-op. |
| `setDisconnectHandler(handler)` | Installs a callback invoked whenever the transport drops, before any reconnect is scheduled. Default implementation is a no-op. |
| `setSession(session)` | Installs the `session::Context` stamped onto every control envelope (`register`, `registerShared`, `attach`, `assign`, `deregister`) this backend subsequently builds. Pushed by `Bridge::setDefaultSession()` and `Bridge::switchBackend()`. Default implementation is a no-op. See [Session propagation to control envelopes](#session-propagation-to-control-envelopes). |

## Connect/disconnect notifications

`setReconnectHandler` exists so `Bridge` can re-register every live
`HandlerBinding` after a transport drop and re-establish; it deliberately
fires only on the *second and later* connects — on the first connect there
is nothing yet to re-register. That leaves two gaps a UI reflecting live
connection state needs closed:

- **First connect.** `waitForConnected()` (where a concrete backend offers
  one, e.g. `QtWebSocketBackend`) answers this, but it blocks the calling
  thread. On a browser/WASM target that hangs the page outright; even on
  desktop it means blocking startup on a network round-trip.
- **Disconnect.** There was no hook at all: a client learned the socket
  dropped only indirectly, when a later action failed.

`setConnectHandler`/`setDisconnectHandler` close both, on `IBackend` itself
(not only on `QtWebSocketBackend`) with the same no-op-default pattern
`setReconnectHandler` already established — a UI observing connection state
shouldn't have to downcast to a concrete backend type to do it, and a
backend with no meaningful connection state (`LocalBackend`) simply never
invokes either. `setConnectHandler`'s callback fires on *every* successful
connect, first included; `setDisconnectHandler`'s fires whenever the
transport drops, **before** any reconnect is scheduled, so an observer sees
the disconnected state even when a retry follows immediately (an instant
successful reconnect must not look, from the UI's perspective, like nothing
happened). Both are invoked on the backend's own thread, and `nullptr`
clears either — matching `setReconnectHandler`'s existing contract exactly.
Purely additive: `setReconnectHandler` keeps its current semantics, and
every existing embedder is unaffected.

`QtWebSocketBackend` is currently the only backend that overrides either:
its `connected`/`disconnected` `QWebSocket` signal slots invoke
`_connectHandler`/`_disconnectHandler` (if installed) at the same points
they already invoke `_reconnectHandler`/schedule a reconnect — see that
section below.

## Session propagation to control envelopes

`Bridge::executeVia` stamps `Bridge::defaultSession()` onto the `ActionCall`
passed to `execute()` (see [bridge.md](bridge.md)), so `execute` envelopes
always carry the current session. Control messages — `register`,
`registerShared` (`registerModelShared`), `attach` (`attachModel`), `assign`
(`assignPrimary`), and `deregister` (`deregisterModel`) — are different: each
is built directly inside the concrete backend, which has no other route to
the `Bridge`'s session. Before `IBackend::setSession` existed, every one of
these envelopes carried a default-constructed (empty, unauthenticated)
`session::Context` regardless of what `Bridge::setDefaultSession()` held, so
`RemoteServer::authorizeRegister` could never see a caller's identity and the
owner principal it records at `register` time was always empty — degrading
`IAuthorizer::authorizeInstance`'s ownership check to allow-all for every
instance a `Bridge` registered (see [session.md](../session/session.md)).

`Bridge` calls `IBackend::setSession` in two places: once from its
constructor (with the just-constructed, typically empty, default session) and
again every time `setDefaultSession()` installs a new one; `switchBackend()`
also calls it on the incoming backend, **before** phase 1's
per-binding re-registration loop runs, so every `register`/`registerShared`
envelope built while re-registering handlers on the new backend already
carries the current session. A wire-backed backend that overrides
`setSession` — `SimulatedRemoteBackend`, `SocketBackend`, `QtWebSocketBackend`
— stores the session and reads it back into every control envelope's
`session` field it subsequently builds. `LocalBackend` does not override
`setSession`: the local path never serialises a `Context` onto a wire
envelope, so there is nothing to stamp.

## Asynchronous registration — `registerModelAsync`

`registerModel`/`registerModelWithContext` are synchronous: a backend whose
registration requires a round-trip can only implement that by blocking the
calling thread until the reply arrives — `QtWebSocketBackend` does this via a
nested `QEventLoop` in `sendSync`. On a WASM main thread, Qt refuses to spin a
nested loop at all (`WaitForMoreEvents is not supported on the main thread
without asyncify`), so that blocking call aborts the page — the very first
`registerModel` a WASM client makes.

`IBackend::registerModelAsync(typeId, factory, contextKey, onRegistered,
onError)` is the optional non-blocking counterpart. A backend that offers one
sends the request and returns `true` immediately, then invokes exactly one of
`onRegistered(ModelId)` / `onError(message)` once the reply arrives, on the
backend's own thread — unless the backend is destroyed first, in which case
neither fires. The default implementation returns `false` without calling
either callback.

`Bridge::registerHandler()` (both overloads — the default-factory template and
the pre-built-binding overload) prefers this path: it adds the binding to
`_handlers` and calls `registerModelAsync` *before* acquiring `Bridge::_mtx`
for the callback (a synchronous callback invocation would otherwise
self-deadlock re-acquiring the lock). If it returns `false`, `registerHandler`
falls back to the synchronous `registerModelWithContext`, exactly as before
this feature existed. If it returns `true`, the binding is returned **unbound**
(`currentId == 0`) — `executeVia` fails fast with "handler not bound" for any
call made before `onRegistered` fires, so a caller using the async path must
wait for registration (e.g. gate its UI on it) rather than fire an action
immediately after constructing the handler.

**Staleness guard.** The success callback captures a `weak_ptr<IBackend>`
pinned to the backend the request was issued against, plus the Bridge's
`liveness()` token (the same pattern `installReconnectHandler` uses). Before
applying the received `ModelId`, it checks the liveness token (skip if the
Bridge is gone) and compares the pinned backend against `loadBackend()` (skip
if a `switchBackend()` already moved past this registration — that call's own
re-registration loop already gave the binding a fresh id on the new backend,
which a stale reply must not overwrite).

**Scope.** Only the plain (non-shared) registration path uses this — a
`BridgeHandler`'s initial construction. `registerModelShared`/`attachModel`
(shared/keyed handlers) and the re-registration `switchBackend()`/the
reconnect handler perform after a backend swap remain synchronous; giving
those an async path too is a larger change to `Bridge`'s locking model, left
for a future issue if it proves necessary.

`QtWebSocketBackend` is the one backend that currently overrides this, gated
by `QtWebSocketBackendConfig::asyncRegistrationEnabled` (default `false` — see
its own section below).

**Queueing before the first connect.** A `registerModelAsync` call made before
the socket has finished connecting is **queued**, not failed — this is exactly
the ordering a single-threaded WASM client must use, since it can never block
waiting for the connection to settle (a `BridgeHandler` constructed the moment
the backend is wired up, before the first `connected` signal). The queued
request is sent, in FIFO order, the moment `connected` fires next (the first
connect included, before the reconnect handler runs) — no protocol change: a
call-id is assigned only at send time, same as the immediate path. If the
socket is torn down (destroyed, or disconnects) before ever connecting, the
queue is drained by `cancelPending`, which still invokes each queued request's
`onError` exactly once, exactly like an in-flight (already-sent) registration
would. See `QtWebSocketBackend`'s own section below.

## Error types

Four exception types are thrown into in-flight `Completion`s:

| Type | Trigger | Purpose |
|---|---|---|
| `BackendChangedError` | `Bridge::switchBackend()` runs | GUI can retry on the new backend or surface a "backend changed" message. |
| `BridgeDestroyedError` | `Bridge` is destroyed | In-flight completions are cancelled because the bridge is gone. |
| `DisconnectedError` | Transport drops mid-call (e.g. WebSocket disconnect) | Framework retries the call on reconnect if the backend supports it; otherwise the GUI's `.onError(...)` runs. |
| `TimeoutError` | Server-side `LimitPolicy::executeTimeout` elapses | Distinguishes a bounded-wait timeout from any other `err` reply, so callers can retry or surface a specific "request timed out" message. |

## `LocalBackend` — in-process execution

`LocalBackend` is the concrete in-process backend. It owns a
`StrandExecutor` (wrapping the `IExecutor&` worker pool, typically a
`ThreadPoolExecutor`) and a `ModelId → shared_ptr<IModelHolder>` map.

**Lifecycle:**
- `registerModel` — atomically increments a counter, locks the registry mutex,
  calls the factory, records the new id in `_changeAware` if the holder's
  `isBackendChangeAware()` is `true`, stores the holder, returns the new
  `ModelId`.
- `deregisterModel` — locks the registry mutex, erases the entry from both
  `_models` and `_changeAware`.
- `execute` — looks up the holder under the registry lock; if `mid` is unknown
  it immediately resolves the completion with
  `std::runtime_error("model not found: id=<n>")`. Otherwise it tracks the
  completion in the pending list, posts `localOp` on the model's strand
  (serialised per-model), sets up a `ScopedContext` (from `call.session`) before
  calling `localOp`, and returns the `Completion`. The strand task also emits
  `executeLatencyMs`/`executeInFlight`/`executeErrors` and calls
  `beginSpan`/`endSpan` around `localOp` — see [observability.md](observability.md).
  Both `registerModel` and `deregisterModel` emit `registerCount`/`deregisterCount`.
- `cancelPending` — snapshots the pending list under the pending mutex, delivers
  `exc` to every still-live state.
- `notifyBackendChanged` — under `_regMtx`, looks up only the models recorded in
  `_changeAware` (populated at registration from
  `IModelHolder::isBackendChangeAware()` — a compile-time answer per model type,
  no `dynamic_cast`); then, outside the lock, **posts** `holder->onBackendChanged()`
  (the `IModelHolder` base virtual) onto each such model's strand (the holder
  captured by `shared_ptr`). Cost is O(change-aware models), not O(all models).
  Delivery is asynchronous and serialised against that model's `execute` tasks;
  it never runs under `_regMtx` or `Bridge::_mtx`, so a sink that re-enters the
  bridge cannot deadlock.
- `setReconnectHandler`/`setConnectHandler`/`setDisconnectHandler` — no-op (no transport to (dis)connect).
- `setSession` — not overridden (the default no-op stands): the local path never serialises a `Context` onto a wire envelope, so there is nothing to stamp.

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
| `register` | `typeId`, `[contextKey]` | `ok` with `modelId` (body empty) | Authenticates the caller (`_authorizer->authenticate(env.session)`), stamping the verified principal onto `env.session.principal` (clearing it when unauthenticated) exactly as `execute` does, then consults `_authorizer->authorizeRegister(env.session, typeId)` — a `false` reply is `err "unauthorized"` and **no instance is created**. Only then creates the model via the `ModelRegistryFactory` and records the (already-verified) principal as its owner. Empty `typeId` → `err "register requires a typeId"` (checked before authorization). If `contextKey` is non-empty, consults the `LogProvider` (if set) and, when it returns a non-null log, calls `holder->attachActionLog(log, contextKey)`. The assigned `modelId` is an **opaque** (non-sequential) value — see below. |
| `deregister` | `modelId` | `ok` or `err` | Consults `authorizeInstance` against the recorded owner (denied → `err "unauthorized"`); otherwise erases the model and its owner entry from the registry. |
| `execute` | `modelId`, `modelType`, `actionType`, `body`, `session` | `ok` with `body` or `err` | See the execute flow below. |
| `hello` | `protocolVersion` | `ok` with `body` = `ProtocolRange`, or `err "protocol version unsupported"` | Protocol-version negotiation, exchanged once per connection before any `register`/`execute`. Carries no `session` and is not authorized — orthogonal to `IAuthorizer`. See [wire.md](wire.md#protocol-version-negotiation). |

**Execute flow (`dispatchExecute`).** In order:

1. **Authorize.** `_authorizer->authorize(env.session, env.modelType, env.actionType)`.
   Denied → `err "unauthorized"` (with the request's `callId`), no dispatch.
2. **Authenticate / make the principal authoritative.** After `authorize`
   succeeds, the server calls `_authorizer->authenticate(env.session)`. If it
   returns a value, the server **overwrites** `env.session.principal` with that
   verified principal *before* building the `ScopedContext`. So model code that
   reads `session::current()->principal` on the remote path sees the identity the
   authorizer extracted from a valid token, not the client's asserted claim
   (`Context::principal` is untrusted wire input on its own). If `authenticate`
   returns `nullopt` — a non-verifying authorizer, including the default
   `AllowAllAuthorizer`, or a token that passed `authorize` but expired in the
   window before `authenticate` — the server **clears** `env.session.principal`
   to the empty string. The client's unverified claim is therefore never
   presented to the model as authoritative: the worst case is an empty
   principal, never an attacker-chosen one (this closes the TOCTOU divergence
   and the authorize-only passthrough — see security.md). This is the only place
   the principal is made authoritative; the verifying implementation lives in
   `SigningAuthorizer` (`session_auth.hpp`, cross-ref security.md). The rewrite
   happens on the calling/pool thread, before the strand task is posted.
3. **Look up the model.** Under `_regMtx`, find `env.modelId` and read its
   recorded owner. Missing → `err "model not found"` (with `callId`), no dispatch.
   (Note: the remote message is the bare string `"model not found"`, without the
   id — unlike the `LocalBackend` path, which resolves the completion with
   `std::runtime_error("model not found: id=<n>")`.)
4. **Per-instance authorize.** `authorize` above saw only the model *type*; this
   step consults `_authorizer->authorizeInstance(env.session, env.modelType,
   env.actionType, modelId, owner)` with the target instance id and its recorded
   owner. Denied → `err "unauthorized"` (with `callId`), no dispatch. The default
   hook allows all, so behaviour is unchanged unless an ownership-enforcing
   authorizer overrides it; `env.session` already carries the verified principal
   stamped in step 2, so the hook compares the recorded owner against it.
5. **Dispatch on the strand.** Posts to the model's strand a task that installs a
   `ScopedContext` from the (now possibly rewritten) `env.session`, calls
   `dispatch(modelType, actionType, *holder, body)` on the server's dispatcher,
   and replies `ok` with the serialised result. Any `std::exception` thrown by
   the dispatch is caught on the strand and returned as `err exc.what()` with the
   `callId`. The strand task **captures `shared_from_this()`** so the server
   (and therefore its `_dispatcher` reference member) stays alive until the task
   runs and its reply is delivered. `handle()`'s pool task only holds the server
   alive until it enqueues onto the strand; without the self-capture the last
   external `shared_ptr` could drop first, leaving the dispatcher dangling (a
   use-after-free) or the reply lost so a client `Completion` hangs forever. The
   task reads the dispatcher via `self->_dispatcher`, never a bare reference
   capture. See concurrency_and_lifetimes.md.

Any envelope that fails to decode produces `err` carrying the decode
exception's message. An unrecognised `kind` produces `err "unknown envelope
kind: <kind>"`. Any `std::exception` thrown while handling a decoded envelope is
caught and returned as an `err` reply carrying `exc.what()` and the request's
`callId`.

**Opaque model ids.** `RemoteServer` assigns each new instance's id by running
an internal monotonic counter through `detail::OpaqueIdGenerator` — a keyed,
4-round Feistel permutation over the 64-bit space, keyed once at construction
from `std::random_device`. The Feistel structure guarantees the mapping is a
bijection, so two different counter values never collide (uniqueness holds for
the server's whole lifetime, short of the practically-unreachable 2^64
wraparound); the per-round secret keys are what make the *output* opaque
rather than merely "scrambled" — an attacker who observes one id cannot invert
a public, unkeyed mixing function to recover the counter and predict the next
one. `ModelId`'s reserved sentinel `0` ("unbound", see `strand.hpp`) is
actively skipped: `RemoteServer` draws a fresh counter value and re-permutes
if the result is ever `0` (a 1-in-2^64 event for a random key). Ids remain
plain `std::uint64_t` on the wire — the `Envelope` and its `modelId` field are
unchanged; only the assigned *values* are no longer sequential. This is
defence-in-depth, not a substitute for `authorizeInstance`: a caller who
independently learns a valid id (from its own register, or a leak) can still
target it, so per-instance ownership remains the actual authorization
boundary — see [security.md](../security.md).

**`handle(msg, reply)`** — asynchronous entry point. Posts to the worker pool,
calls `dispatchMessage` which decodes, dispatches by `kind`, and calls `reply`
exactly once. A three-argument overload, `handle(msg, reply, cid)`, additionally
attributes any `register` in `msg` to a connection scope — see "Connection
scopes" below.

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

**`health()` / `setHealthHandler(handler)`** — a readiness snapshot and an
optional state-change callback, detailed in [observability.md](observability.md).
`health()` returns `HealthStatus{ready, liveModels, inFlight}`: `liveModels`
from the registry (same mutex as `register`/`deregister`/`execute`), `inFlight`
from `_inFlightExecutes` — the same atomic counter `LimitPolicy::maxInFlightExecutes`
enforces, `drainedWithin()` (below) waits on, and the `executeInFlight` metric
reports. `ready` starts `true` and is flipped to `false`, once and for good, by
`beginShutdown()` (below) — there is no un-shutdown. `setHealthHandler` fires
immediately with the current snapshot, and again with the post-shutdown
snapshot when `beginShutdown()` runs.

**Metrics and tracing.** `dispatchMessage`'s `register`/`deregister` branches
emit `registerCount`/`deregisterCount`; `dispatchExecute`'s admit/complete
points emit `executeInFlight`, and its strand task emits
`executeLatencyMs`/`executeErrors` and calls `beginSpan`/`endSpan` around the
`ActionDispatcher::dispatch` call. All are no-ops unless a sink is installed
via `morph::observe::setMetricSink`/`setTraceSink` — see
[observability.md](observability.md).

### Protocol-version negotiation

`RemoteServer::setSupportedVersionRange(min, max)` sets the inclusive
`{min, max}` protocol-version range this server advertises in reply to
`"hello"` (thread-safe, same pattern as `setLogProvider`/`setLimitPolicy`).
Defaults to `{kProtocolVersion, kProtocolVersion}` — this build's single
supported version — so an unconfigured server's behavior only changes for
clients that opt into sending `"hello"` in the first place. Throws
`std::invalid_argument` if `min > max`. See [wire.md](wire.md#protocol-version-negotiation)
for the full negotiation story, including how `SimulatedRemoteBackend` and
`QtWebSocketBackend` each expose an opt-in `negotiateProtocolVersion()` built
on their existing synchronous control path.

### `LimitPolicy` — opt-in resource limits

`RemoteServer::setLimitPolicy(LimitPolicy)` installs an optional, connection-agnostic
resource policy (thread-safe, same pattern as `setLogProvider`). Every field
defaults to `0` ("unbounded"), so an unconfigured server's behavior is unchanged:

| Field | Default | Enforcement |
|---|---|---|
| `executeTimeout` | `0` (disabled) | A timer arms when `execute` dispatches to the model's strand. If it fires first, the server replies `err "timeout"` and the eventual strand result (if the model finishes later) is discarded via a shared once-flag — `handle()`'s reply-exactly-once contract holds regardless of which path resolves first. The model keeps running to completion on its strand; morph never interrupts `Model::execute`. |
| `maxLiveModels` | `0` (unbounded) | Checked under `_regMtx` before `register` constructs a new instance; over the cap → `err "too many models"`. The check and the eventual insert are two separate critical sections (to avoid constructing an instance that will be rejected), so a burst of concurrent registers can overshoot the cap by a small, bounded amount — a soft, defense-in-depth limit, not a hard invariant. |
| `maxInFlightExecutes` | `0` (unbounded) | An atomic counter, incremented when `execute` is admitted for dispatch (before the strand task is posted) and decremented when its reply is sent (success, exception, or timeout — whichever resolves the call first); over the cap → `err "server busy"`, no dispatch. |

A server-side execute timeout surfaces to a caller as `morph::backend::TimeoutError`
(alongside `BackendChangedError`/`BridgeDestroyedError`/`DisconnectedError`) rather
than a generic `std::runtime_error`, on both `SimulatedRemoteBackend` and
`QtWebSocketBackend`.

The background timer that enforces `executeTimeout` is `detail::TimeoutScheduler` —
a single dedicated thread per `RemoteServer` (mirroring `NetworkMonitor`'s
condition-variable wait loop), lazily started by `setLimitPolicy` the first time
`executeTimeout` is configured, so a server that never uses the feature pays no
extra thread.

### Connection scopes

`RemoteServer` can optionally attribute registered models to a
transport-assigned connection, so a transport can reclaim every model a
connection created when that connection goes away. `ConnectionId`
(`morph::backend::ConnectionId`) is a `std::uint64_t` alias; `0` is reserved
and means *unscoped* — the meaning the two-argument `handle()` and
`handleInline()` always have. Scoping is strictly opt-in; enabling it changes
nothing for a caller that never uses it.

- `openConnection()` returns a fresh non-zero `ConnectionId` and opens an empty
  scope for it. Call once per accepted transport connection.
- The scoped `handle(msg, reply, cid)` overload attributes any `register` (or
  register-or-attach `attach`) decoded from `msg` to `cid`'s scope: the
  `ModelId` is recorded in a `cid → (ModelId → count)` map, next to
  `_models`/`_owners`/the shared-instance directory under the same `_regMtx`,
  so scope membership can never desync from instance existence. The count
  lets one connection hold more than one reference to the same shared
  instance (e.g. two handlers on one connection attaching the same key)
  without either reference leaking the other's release.
- A `deregister` releases exactly the reference **the requesting connection**
  holds — decrementing `cid`'s own scope entry for that `ModelId`, using the
  `cid` the deregister call itself carries, never whichever connection
  happened to attach the instance last. A shared instance can have several
  owning connections at once; crediting the release to the wrong one would
  either strand a reference no one will ever decrement, or let one
  connection's deregister silently consume another's hold.
- `closeConnection(cid)` erases every model still recorded in `cid`'s scope
  (`_models`, `_owners`, and the per-instance connection entry) exactly as the
  `deregister` path does, then drops the scope itself. Passing `0`, an
  unknown `cid`, or a `cid` already closed is a no-op — idempotent by
  construction.
- **`closeConnection` does not consult `IAuthorizer`.** It is server-side
  housekeeping triggered by the transport observing its own connection close,
  not a caller-attributed action — synthesising a `deregister` envelope
  instead would need a session/token to pass `authorizeInstance`, which an
  ownership-enforcing authorizer would rightly reject, and would require the
  transport to parse every `register` reply to learn which ids it owns. Only
  in-process transport code can reach `closeConnection`, and the transport is
  already inside the server's trust boundary (see security.md).
- Cleanup never races a running `execute`: `dispatchExecute` copies the
  instance's `shared_ptr<IModelHolder>` into the strand task before dispatch,
  so an in-flight action keeps the holder alive until its task completes;
  `closeConnection` only removes the registry's reference, preventing *new*
  lookups (see concurrency_and_lifetimes.md).
- A `register` that arrives *after* its scope was closed is refused with
  `err "connection closed"` and no instance is retained. `handle()` posts to
  the worker pool while `closeConnection` runs synchronously on the transport's
  disconnect callback, so a client that registers and immediately drops its
  socket genuinely interleaves the two. The scope is looked up, never
  default-created: recreating it would strand that model (and every later one
  on the dead id) in a scope nothing closes a second time — an unbounded leak
  that, with `LimitPolicy::maxLiveModels` set, wedges the server permanently at
  `err "too many models"`.
- `SimulatedRemoteBackend` keeps using the unscoped path (its "connection" is
  the process itself) — it is unaffected by connection scopes.

### Graceful shutdown (`beginShutdown()` / `drainedWithin()`)

`beginShutdown()` enters shutdown: every subsequent `register` and `execute`
envelope is rejected with `err "server shutting down"` (checked once, at the
top of `dispatchMessage`, before any other validation — including the
shutdown check happening before authorization or registry lookups run);
`deregister` (and any other envelope kind) is still served so clients can
tear down cleanly during the drain window. Idempotent, and irreversible —
there is no un-shutdown; a restarted service constructs a fresh
`RemoteServer`. `beginShutdown()` also flips `health()`'s `ready` to `false`
and, if a handler is installed via `setHealthHandler()`, re-invokes it with
the post-shutdown snapshot — the mechanism that lets an orchestrator stop
routing to a server that is draining.

`drainedWithin(deadline)` blocks the calling thread (via a condition
variable, not a busy poll) until every in-flight `execute` has delivered its
reply, or `deadline` elapses, returning `true`/`false` accordingly.
"In-flight" is the same `_inFlightExecutes` counter `LimitPolicy::maxInFlightExecutes`
gates and `health()`'s `inFlight` field reads (one counter, never
double-counted): incremented when `dispatchExecute` admits a call for
dispatch (before posting to the model's strand) and decremented — waking any
`drainedWithin()` waiter once it reaches zero — right before its reply is
sent, on every resolving path (`ok`, `err`, or a `LimitPolicy::executeTimeout`
firing first).

The standard sequence an operator (or `QtWebSocketServer::closeGracefully`,
below) follows is `beginShutdown()` then `drainedWithin(deadline)`: new work
fails fast while old work finishes, and once drained the existing teardown
rules ([concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md)) apply
trivially, because every queue is already empty. morph never preempts a
running action to force a drain — a model that can run unboundedly long
bounds itself; the deadline bounds the *caller's wait*, not the model.

## Server-side observability

`morph::log` exists and both `RemoteServer` and `QtWebSocketServer` have
access to it, but several outcomes a client cannot distinguish from each
other used to produce no server-side record at all — the operator questions
"did the request arrive? was it rejected? how many clients are connected?
why did that one drop?" had no server-side answer. Four points now log, each
a one-line call at a point the code already reaches:

- **`RemoteServer::dispatchMessage` — undecodable envelope (`logError`).** A
  client that swallows its own error (or is malformed precisely because it
  is confused) previously left no trace of a request that never dispatched
  at all. Logs the connection id, the exception text, the byte count, and a
  **truncated** (256-byte) prefix of the raw payload. The payload is the
  most useful field for diagnosing *why* a client sent something malformed —
  and the most likely to contain application data, hence truncated rather
  than logged in full; server logs should already be treated as
  operationally sensitive (they also carry exception text and, on other
  lines, connection ids), and this is not a general redaction mechanism.
- **`RemoteServer::dispatchMessage` — one line per successfully-decoded
  request (`logDebug`).** `dispatchMessage` is the one place every kind
  funnels through, so it is the natural spot: connection id, `kind`,
  `callId`, `typeId`/`modelId`/`modelType`/`actionType` (whichever the kind
  populates), and the body's byte count. Deliberately omits the session
  principal (personal data in many deployments; attribution is still
  possible after the fact by correlating `callId`/connection id with the
  journal or action log where authentication is configured) and the
  request body itself (already covered, truncated, on the decode-failure
  path above — logging every successful body by default would be far higher
  volume and duplicate what the action log already records for `execute`).
  At `debug` because of that volume; a deployment that wants a quieter
  default raises `morph::log::setLogLevel` to `info` or higher (the
  library's own default minimum level is `debug` — see `logger.hpp` — so
  this line *does* appear unless a deployer has already opted into a
  quieter stream).
- **`QtWebSocketServer::onNewConnection` — connection refused by
  `maxConnections` (`logWarn`).** To the client this looks exactly like the
  server being down; the operator previously had no way to learn the cap
  was hit, which is the one piece of information that would explain the
  symptom. Names the current live count and the configured cap.
- **`QtWebSocketServer::onNewConnection`/`onDisconnected` — connect and
  disconnect (`logInfo`).** Neither was recorded before, so there was no way
  to reconstruct how many clients were live, or why one went away. Connect
  logs the connection id and the live count (including the new connection);
  disconnect logs the connection id, the live count (after removal), and the
  WebSocket close code + reason — captured from the socket before it is
  torn down, since those are the useful part of "why did that one drop?".

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
- `negotiateProtocolVersion` sends a `"hello"` via `handleInline` and classifies
  the reply with `wire::interpretHelloReply` — opt-in, not called automatically
  (see [wire.md](wire.md#protocol-version-negotiation)).
- `execute` serialises the action via `call.serializeAction()`, builds an
  `execute` envelope, calls `handle()` (asynchronous), and returns a `Completion`
  that resolves when the server's reply is deserialised via
  `call.deserializeResult()`.
- `notifyBackendChanged` is a no-op — models live in the `RemoteServer`, not
  locally.
- `cancelPending` snapshots and resolves pending completions, same pattern as
  `LocalBackend`.
- `setSession` stores the session (guarded by its own mutex); every
  subsequently built `register`/`registerShared`/`attach`/`assign`/`deregister`
  envelope's `session` field is set from it before the `handleInline` call —
  see [Session propagation to control envelopes](#session-propagation-to-control-envelopes).

**Connection scope.** The default constructor, `SimulatedRemoteBackend(RemoteServer&)`,
carries `ConnectionId{0}` — the server's "unscoped" sentinel — on every call it
makes, exactly as before connection scopes existed. A second constructor,
`SimulatedRemoteBackend(RemoteServer&, ConnectionId)`, takes a `ConnectionId`
obtained from `server.openConnection()` and threads it through every
`register`/`registerShared`/`attach`/`assign`/`instances`/`deregister`/`execute`
call this backend makes (via the three-argument `RemoteServer::handle`/
two-argument `handleInline(msg, cid)` overloads) — the in-process equivalent of
what `QtWebSocketServer`/`morph::net::SocketServer` give a real transport
connection (see "Connection scopes" above). This lets a test construct several
independently-scoped simulated clients against one `RemoteServer` and exercise
connection-scoped state deterministically: two such backends sharing a key
reach one instance and their `deregisterModel`/`closeConnection` release only
their own reference, and `closeConnection(cid)` reclaims exactly what that one
backend registered. The backend does **not** call `closeConnection` on its own
destruction — unlike a real socket there is no single unambiguous "this
connection is gone" moment to hook here — so a caller that wants the scope
reclaimed calls `server.closeConnection(cid)` explicitly (or lets the server
itself be destroyed).

## `QtWebSocketBackend` — client-side WebSocket transport

`morph::qt::QtWebSocketBackend` is the concrete `IBackend` that talks to a
`RemoteServer` over a real WebSocket (`ws://` or `wss://`). It owns a
`QWebSocket` and opens the connection to `serverUrl` in its constructor. It holds
**no local model objects** — every model lives on the server, exactly like
`SimulatedRemoteBackend`, but across an actual socket instead of an in-process
call.

**Threading.** Single-threaded: must be constructed and used on the Qt event
loop thread. `execute`, the `QWebSocket` signal slots, and the reconnect timer
all run on that one thread, so `_connected`, `_nextCallId`, and the reconnect
state need no locking. Only `_pending` (the callId → completion map) is guarded
by `_pendingMtx`, because `cancelPending` can be called from `Bridge` /
`~Bridge` on another thread.

**Control operations are synchronous; execute is asynchronous.**
- `registerModel` — sends a `register` envelope via `sendSync`, which pumps a
  **nested `QEventLoop`** on the Qt thread until the reply arrives, then decodes
  it. `ok` → returns the server-assigned `ModelId`; a non-`ok` reply throws
  `std::runtime_error("register failed: " + message)`. If `sendSync` throws
  (see below — the socket is not connected, or disconnects while the reply is
  outstanding), `registerModel` wraps it as
  `std::runtime_error("register failed: <what>")` (so a lost connection surfaces
  as `"register failed: disconnected"`) rather than propagating the raw error or
  hanging. The `factory` argument is ignored (model construction is delegated to
  the server, as with all remote backends). `registerModelWithContext` is
  **not** overridden — the default drops the `contextKey`, so this transport
  does not carry a context key to the server's `LogProvider`.

  `sendSync` itself is hardened against a disconnect mid-call. Before parking the
  nested loop it checks `_connected` and throws `"disconnected"` up front if the
  socket is already down, and it rejects a **reentrant** call (a second sync send
  while one is already parked) with an error rather than clobbering the single
  `_syncLoop` pointer. The `disconnected` slot, if a sync loop is parked, clears
  `_pendingReply` and quits the loop, so a register whose reply never arrives
  unblocks and reports failure instead of freezing the Qt thread forever; when
  the parked loop returns with an empty `_pendingReply`, `sendSync` throws
  `"disconnected"`. See concurrency_and_lifetimes.md.

  **`registerModelAsync` is the non-blocking alternative**, opt-in via
  `QtWebSocketBackendConfig::asyncRegistrationEnabled` (default `false`, so
  every existing embedder keeps `registerModel`'s synchronous behavior
  unchanged). A call made before the socket has finished connecting is queued
  (`_queuedRegistrations`) rather than failed, and flushed — each entry
  assigned a call-id and sent, in FIFO order — from the `connected` slot, the
  first connect included, before `_connectHandler`'s reconnect-handler
  counterpart runs. If the backend is destroyed (or the socket disconnects)
  before that queue is ever flushed, `cancelPending` drains it and still
  invokes each entry's `onError` exactly once. See [Asynchronous
  registration](#asynchronous-registration--registermodelasync).
- `deregisterModel` — **fire-and-forget**, not synchronous: if `_connected`, it
  sends a `deregister` envelope and returns immediately without waiting for the
  ack; if disconnected, it does nothing. This deliberately avoids a nested
  `QEventLoop` during a destructor, which can trip Qt asserts. An undelivered or
  lost `deregister` no longer leaks the model indefinitely when the server side
  is a `QtWebSocketServer`: its connection scope reclaims every model this
  client registered at the next disconnect (see "Connection scopes" and
  Limitations).
- `execute` — if not connected, resolves the returned `Completion` immediately
  with `DisconnectedError`. Otherwise it assigns a monotonic `callId`
  (`++_nextCallId`), records the completion state + `deserializeResult` +
  `cbExec` in `_pending[callId]`, serialises the action, and sends the `execute`
  envelope. The `Completion` resolves when the reply with the matching `callId`
  arrives.

**Reply framing / callId multiplexing.** `onTextMessage` decodes each incoming
frame and routes it by `callId`:
- A **non-zero `callId`** is checked against `_pending` first (an async
  `execute` reply): the backend pops the matching `PendingExecute`; `ok` →
  `deserialize(body)` into the completion's value (deserialisation exceptions
  become the completion's error), any other kind → `std::runtime_error(message)`
  into the completion's error. If not found there, `_pendingRegistrations` is
  checked next (an async `registerModelAsync` reply — same `callId`
  counter/namespace as `execute`, separate map because the reply shape differs):
  `ok` → `onRegistered(ModelId{modelId})`, any other kind → `onError(message)`.
  A `callId` matching **neither** map (e.g. a late reply for an
  already-cancelled call) is dropped silently.
- A **`callId == 0`** frame is a synchronous control reply (`register` when
  `asyncRegistrationEnabled` is `false`, or `attach`/`assign`/`instances`/
  `deregister`); it is stored in `_pendingReply` and quits the parked nested
  `QEventLoop`. A frame that fails to decode is also routed to the parked sync
  waiter (as the raw string) so the blocked `sendSync` unblocks with an error
  rather than hanging.

Because `execute` replies are matched on `callId`, concurrent in-flight execute
calls are supported; `RemoteServer`/`QtWebSocketServer` echo the request `callId`
in the reply (see wire.md).

**Reconnect lifecycle.** Configured by `QtWebSocketBackendConfig` (aliased as
`QtWebSocketBackend::Config`):

| Field | Default | Meaning |
|---|---|---|
| `reconnectEnabled` | `true` | Whether to auto-reconnect after an unsolicited disconnect. |
| `initialReconnectDelay` | `500 ms` | Delay before the first reconnect attempt. |
| `maxReconnectDelay` | `30 s` | Upper bound on the exponential backoff. |
| `backoffMultiplier` | `2.0` | Multiplier applied to the delay after each failed attempt. |

The state machine:
- On **`connected`**: sets `_connected`, resets the backoff delay to
  `initialReconnectDelay`, quits any parked sync loop. Fires `_connectHandler`
  (if installed) unconditionally — every successful connect, first included.
  It then fires the `_reconnectHandler` **only on subsequent connects**
  (`_everConnected` was already true) — never on the first connect, because
  initial handler registration is driven by the `BridgeHandler` constructors,
  not the reconnect path. See [Connect/disconnect notifications](#connectdisconnect-notifications).
- On **`disconnected`**: clears `_connected`, then fires `_disconnectHandler`
  (if installed) — **before** the reconnect scheduling below, so an observer
  sees the disconnected state even when a retry follows immediately. Then
  immediately calls `cancelPending(DisconnectedError{})`, resolving every
  in-flight execute with `DisconnectedError`. If not shutting down,
  `reconnectEnabled`, and the socket had *ever* connected, it schedules a
  reconnect with the current backoff delay, then multiplies the delay by
  `backoffMultiplier` (capped at `maxReconnectDelay`) for the next attempt. A
  connection that never succeeded the first time is **not** retried.
- `attemptReconnect` re-opens the socket; if it fails, `QWebSocket` fires
  `disconnected` again and the cycle repeats with the grown backoff.

`Bridge` installs a `_reconnectHandler` (via `setReconnectHandler`) that
re-registers every live `HandlerBinding` so model ids stay valid after the
server assigns fresh ones on the new connection (cross-ref bridge.md).
`setConnectHandler`/`setDisconnectHandler` are independent of that — an
application installs them directly on the backend (not through `Bridge`) to
drive its own connection-state UI.

**`setSession(session)`** stores the session in `_session` (this backend is
single-threaded — Qt event loop thread only — so no lock is needed); every
subsequently built `register`/`registerShared`/`attach`/`assign`/`deregister`
envelope's `session` field is set from it before sending. See
[Session propagation to control envelopes](#session-propagation-to-control-envelopes).

**`waitForConnected(timeoutMs = 5000)`** pumps the Qt event loop until the socket
connects or the timeout elapses; returns the current `_connected` flag. Intended
to be called once after construction on the Qt thread.

**`negotiateProtocolVersion()`** sends a `"hello"` synchronously — the same
nested-`QEventLoop` path `sendSync` uses for `registerModel` — and classifies
the reply via `wire::interpretHelloReply`. Opt-in: intended to be called once,
after `waitForConnected()` returns `true` and before any
`registerModel`/`execute` call, but nothing enforces that ordering and nothing
calls it automatically. Throws `std::runtime_error` if the server explicitly
rejects the version or if `sendSync` fails (not connected, or a disconnect
mid-call). See [wire.md](wire.md#protocol-version-negotiation).

**TLS.** Pass a `QSslConfiguration` to enable `wss://`. Build it with
`tlsVerifyingConfig()` (CA-verified, the recommended production default) or
`tlsPinnedConfig(cert)` (pinned-certificate, for self-signed deployments) —
both in `qt_tls.hpp`. `tlsInsecureNoVerify()` (`QSslSocket::VerifyNone`)
disables peer verification and is for local development and tests only (see
[security.md](../security.md), "Transport security"). A plain `ws://` client
against a `wss://` server never connects (and vice versa).

**SSL-less Qt builds (`QT_NO_SSL`, including the standard Qt-for-WebAssembly
configuration).** Both `qt_websocket_backend.hpp`/`.cpp` (client) and
`qt_websocket_server.hpp`/`.cpp` (server) guard every `QSslConfiguration` use
behind `#ifndef QT_NO_SSL`: the `tls` constructor parameter (`_tls` member on
the client) does not exist at all when Qt itself was configured without SSL
(that type isn't provided in that configuration, so there's no value to
accept or ignore — the constructor's arity itself changes). The server
additionally guards `QWebSocketServer::SecureMode`, which Qt also omits under
`QT_NO_SSL`: such a server always constructs in `NonSecureMode`, and
`listen()`'s plaintext-exposure guard (see above) treats it as `hasTls =
false` unconditionally. Both files compile into the same `morph_qt_impl`
target (`CMakeLists.txt`), so both had to be fixed together — a build is only
SSL-less-Qt-compatible as a whole if every translation unit in the target is.
`wss://` still works on such a build regardless: in a WASM/browser
deployment the browser terminates TLS before Qt's `QWebSocket` ever sees the
connection, so the only thing genuinely unavailable is configuring TLS from
C++ (client certificates, pinning). `qt_tls.hpp`'s helpers
(`tlsVerifyingConfig`/`tlsPinnedConfig`/`tlsInsecureNoVerify`) are a separate,
opt-in header that still requires SSL support to compile — nothing calls it
unless it asks for TLS configuration explicitly, so this is unaffected by
`QT_NO_SSL` in practice. Verified by a `try_compile()` guard
(`tests/qt/CMakeLists.txt`) that forces `QT_NO_SSL` against this project's
normal SSL-enabled Qt, compiling both the client and the server (plus a
manually-generated moc translation unit for the server's `Q_OBJECT` vtable)
into one executable: Qt's own `<QSslConfiguration>` header self-guards on the
identical macro regardless of how Qt was actually built, so this reliably
reproduces (and catches a regression of) the same failure an actual
SSL-less Qt hits, without needing one.

**Destruction.** The destructor sets `_shuttingDown`, stops the reconnect timer,
disconnects all `QWebSocket` signals (so no slot touches members mid-teardown),
`abort()`s the socket (TCP RST, no close handshake), then calls `cancelPending`
again as a safety net (in case the owner did not run it first — e.g. the backend
was used outside a `Bridge`), and finally drains the Qt event queue so the socket's
internal state machine settles before its `QObject` destructor runs.

## `QtWebSocketServer` — server-side WebSocket transport

`morph::qt::QtWebSocketServer` is a `QObject` that fronts a `RemoteServer` with a
real listening socket. It does not own the `RemoteServer` — it holds
`RemoteServer& _server` by reference, so the server's owning `shared_ptr` must
outlive the transport (see Lifetime & ownership).

**Connection scope.** `QtWebSocketServer` opts every client into `RemoteServer`'s
connection scope end to end, so a client crash or dropped socket reclaims its
models instead of leaking them:
- **Accept** (`onNewConnection`) calls `_server.openConnection()` and stores the
  returned `ConnectionId` in the client's `ClientState` (keyed by `QWebSocket*`
  in `_clients`, alongside the rate-limit/handshake bookkeeping below).
- **Message** (`onTextMessage`) looks up the sender's `ClientState` and forwards
  through the scoped `_server.handle(msg, reply, cid)` overload instead of the
  two-argument one, so any `register` in the frame is attributed to that
  connection.
- **Disconnect** (`onDisconnected`) calls `_server.closeConnection(cid)` before
  removing the socket from `_clients` — the step that reclaims every model the
  connection registered.
- **Shutdown** (`close()`, and the destructor that calls it) calls
  `closeConnection` for every remaining client before aborting its socket, so an
  orderly server stop also reclaims every client's instances.

**Observability.** `onNewConnection` logs at `morph::log::LogLevel::info` once
a connection is admitted (connection id, live count including the new one);
a connection refused for being over `cfg.maxConnections` logs at `warn`
instead (naming the live count and the configured cap) before the socket is
closed. `onDisconnected` logs at `info` (connection id, live count after
removal, `QWebSocket::closeCode()`/`closeReason()` captured before teardown).
See [Server-side observability](#server-side-observability).

**Flow.** `listen()` binds to the requested TCP port on `cfg.bindAddress`
(`QtWebSocketServerConfig`, default `QHostAddress::LocalHost` — today's
behavior, unchanged) and starts accepting — unless `cfg.bindAddress` is
non-loopback, no TLS configuration was passed to the constructor, and
`cfg.allowPlaintextExposure` is `false`, in which case `listen()` refuses: it
returns `false` without binding and logs at `morph::log::LogLevel::error` (see
[security.md](../security.md), "Transport security"). `port()` returns the
bound port (useful when constructed with port `0` to let the OS assign a free
one); `close()` (and the destructor) stops accepting, reclaims every remaining
client's connection scope, and aborts/`deleteLater`s every client socket. Each
accepted `QWebSocket` is tracked in `_clients` together with its `ConnectionId`;
on its `disconnected` signal both the scope and the socket are reclaimed and
removed.

**Message handling.** For every text frame from a client, `onTextMessage` calls
the scoped `RemoteServer::handle(msg, reply, cid)` (asynchronous — dispatched to
the server's worker pool). The reply callback captures a `QPointer<QWebSocket>`
(a *weak* handle) and marshals the send back onto the Qt thread via
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`: the reply is produced on
a pool thread but `QWebSocket::sendTextMessage` must run on the Qt thread. If the
client socket was destroyed before the reply is ready, the `QPointer` is null and
the reply is silently dropped. A malformed frame produces an `err` reply from the
`RemoteServer` and does not disconnect the client or affect other clients.

**Multi-client / concurrency.** One server serves many clients; each client
registers its own model instances on the shared `RemoteServer`, so per-client
model state is isolated. Because `handle()` posts to the pool, replies for
different clients (and different calls) can be produced concurrently on separate
pool threads and are each marshalled back to their originating socket.

**TLS.** Constructing with a `QSslConfiguration` puts the `QWebSocketServer` into
`SecureMode` (`wss://`); without one it runs in `NonSecureMode`.

**Graceful shutdown (`closeGracefully(deadline)`).** The transport-level
counterpart to `RemoteServer::beginShutdown()`/`drainedWithin()`: it calls
`QWebSocketServer::pauseAccepting()` (no new connections), then
`beginShutdown()` on the `RemoteServer` (new `register`/`execute` now fail
fast on every existing connection), then waits up to `deadline` for
`drainedWithin()` — pumping the Qt event loop while it waits so the reply
callbacks `onTextMessage` already queued via `QMetaObject::invokeMethod`
actually run. Because `drainedWithin()`'s in-flight count can reach zero a
moment before that queued reply callback has actually flushed the bytes over
the socket, `closeGracefully` pumps a short additional settle window (bounded
by whatever is left of `deadline`) before proceeding, so a reply that just
landed is not closed out from under. It then sends every still-connected
client a real close frame (`CloseCodeGoingAway`, reason `"server shutting
down"`) instead of an abort, pumps the event loop again for the remainder of
`deadline` to let that handshake flush, and finally calls the existing
`close()` for whatever `deadline` did not leave time to finish gracefully
(which also reclaims each remaining client's connection scope, same as
`close()` always has). `deadline` bounds the whole sequence from the moment
`closeGracefully` is called: a drain that used the full budget leaves no time
for the close handshake before the hard stop, while a drain that finishes
early leaves the remaining budget for it. Returns `true` if the drain
finished within `deadline`, `false` if the hard stop had to reclaim
stragglers. Purely additive and opt-in: a server that never calls it behaves
exactly as today, and `close()` itself is unchanged.

**Resource limits.** `QtWebSocketServerConfig` (aliased `QtWebSocketServer::Config`,
declared outside the class for the same "fully-parsed-before-default-argument"
reason as `QtWebSocketBackendConfig`) bounds per-connection resource usage:

| Field | Default | Enforcement |
|---|---|---|
| `maxConnections` | `0` (unbounded) | A connection accepted beyond this count is closed immediately in `onNewConnection`, before any signal is wired or the socket is tracked. Logged at `morph::log::LogLevel::warn`, naming the live count and the cap — see [Server-side observability](#server-side-observability). |
| `maxMessageBytes` | `wire::kMaxEnvelopeBytes` | Checked against the UTF-8 byte length of every incoming frame before it reaches `RemoteServer::handle()`; an oversized frame gets an immediate `err` reply and is never dispatched. The reply carries the rejected call's `callId`, recovered by `wire::detail::peekCallId`'s bounded prefix scan since the frame is deliberately never decoded. A zeroed `callId` would not merely fail to resolve the execute — `0` is the client's synchronous-reply discriminator, so it would resume an unrelated parked `register`/`deregister` with another call's reply. |
| `messagesPerSecond` | `0` (unbounded) | A per-connection token bucket (capacity = `messagesPerSecond`, refilled continuously). A frame that finds an empty bucket is dropped silently — not replied to, not queued. |
| `handshakeTimeout` | `0` (disabled) | A one-shot timer per connection; if no frame arrives before it fires, the socket is closed. Cancelled on the first frame. Because `QWebSocketServer::newConnection()` only fires after the WS (and TLS, in `SecureMode`) opening handshake completes, this in practice bounds time-to-first-frame after that point, not the handshake itself. |
| `idleTimeout` | `0` (disabled) | A shared ~1-second housekeeping sweep closes any connection whose last frame is older than `idleTimeout`; the actual close can lag the configured value by up to the sweep interval. |
| `bindAddress` | `QHostAddress::LocalHost` | The address `listen()` binds to (see "Flow" above). |
| `allowPlaintextExposure` | `false` | Deliberate opt-out of the exposure guard: set `true` only to knowingly serve plaintext on a non-loopback `bindAddress` (see "Flow" above). |

## `SocketBackend` / `SocketServer` — raw-socket WebSocket transport

`morph::net::SocketBackend` (`include/morph/net/socket_backend.hpp`) and
`morph::net::SocketServer` (`include/morph/net/socket_server.hpp`) are the
Qt-free reference transport: they speak the same RFC 6455 WebSocket framing as
`QtWebSocketBackend`/`QtWebSocketServer` — plaintext `ws://` only, no TLS — over
raw POSIX (BSD) sockets instead of `QWebSocket`/`QWebSocketServer`. The module
is header-only, gated behind the CMake option `MORPH_BUILD_NET` (default
`OFF`; Linux/macOS only — see Limitations), and depends on nothing but `morph`
itself: the HTTP/1.1 Upgrade handshake (`Sec-WebSocket-Key`/
`Sec-WebSocket-Accept`, via a hand-rolled SHA-1 + base64) and the masked/
unmasked text-frame codec are implemented from scratch in
`include/morph/net/detail/` (`sha1.hpp`, `base64.hpp`, `ws_handshake.hpp`,
`ws_frame.hpp`, `tcp_socket.hpp`). Because both transports round-trip the same
`wire::Envelope`, a `SocketBackend` client and a `QtWebSocketServer`
interoperate (and vice versa) with no protocol changes on either side.

**Reconnect handlers run on their own thread.** `SocketBackend` invokes the
handler installed by `setReconnectHandler` (which `Bridge` uses to re-register
its models) from a dedicated handler thread, not inline from the I/O thread's
connect path. A reconnect handler is expected to issue synchronous control
calls, and those park on `_syncCv` waiting for a reply only the I/O thread's
read loop can deliver — run inline, before that read loop starts, the wait
blocks the one thread able to satisfy it and the transport deadlocks with no
timeout to break it. Requests coalesce: a reconnect arriving while a handler is
still running re-runs it once afterwards rather than queueing. A handler that
throws is caught and logged, so the next reconnect still finds the thread
waiting. `QtWebSocketBackend` has no equivalent need — its `sendSync` runs a
nested `QEventLoop` that keeps pumping the socket.

**Threading — the one deliberate difference from the Qt transport.**
`QtWebSocketBackend` is pinned to the Qt event loop and uses a nested
`QEventLoop` for its synchronous `registerModel`; `SocketBackend` instead owns
a dedicated I/O thread and uses `std::condition_variable`s for the same
synchronous control op, so it needs no GUI event loop at all. A consequence is
that, unlike `QtWebSocketBackend`, `SocketBackend` may safely be driven from
multiple threads concurrently — `registerModel`/`execute`/`deregisterModel`/
`cancelPending` are all internally synchronized; there is no single "owning"
thread. `SocketServer` mirrors this: one accept thread plus one thread per
accepted connection, in place of Qt's event-loop-driven socket signals. One
consequence worth calling out for tests/embedders: because neither side of
`morph::net` needs a Qt event loop, blocking calls like
`SocketBackend::waitForConnected()`/the synchronous `registerModel` genuinely
block the calling thread with no event-loop pumping of their own — fine
against a `SocketServer` peer (which needs no pumping either), but calling
them directly from the one thread that owns a `QCoreApplication` a
*`QtWebSocketServer`* peer depends on would starve that peer's own
accept/handshake machinery. See
`tests/net_qt_interop/test_net_qt_interop.cpp`'s `waitForConnectedPumpingQt`/
`makeHandlerPumpingQt` helpers for the pattern such a caller needs (poll with
a short timeout while pumping `QCoreApplication::processEvents()`).

**`SocketBackend` — client-side `IBackend`.** Implements every `IBackend`
method with the same observable semantics as `QtWebSocketBackend`:
`registerModel` is synchronous (parks on a condition variable instead of a
nested event loop; a register whose reply never arrives unblocks with
`"register failed: disconnected"` rather than hanging, the same hardening
`QtWebSocketBackend::sendSync` applies); `deregisterModel` is fire-and-forget
(same trade-off; an undelivered or lost `deregister` against a `SocketServer`
peer no longer leaks the model, because `SocketServer` now participates in
`RemoteServer`'s connection-scope contract exactly as `QtWebSocketServer`
does — see below); `execute` assigns a monotonic `callId`, is
fully asynchronous, and supports concurrent in-flight calls matched by
`callId` exactly like the Qt transport. `setSession` stores the session
under its own mutex (`_sessionMtx`, guarding the one field it protects — this
backend is driven from multiple threads, unlike the single-threaded Qt
transport) and every subsequently built `register`/`registerShared`/
`attach`/`assign`/`deregister` envelope's `session` field is set from it
before sending — see
[Session propagation to control envelopes](#session-propagation-to-control-envelopes).
Reconnect is configured by
`SocketBackendConfig` (aliased `SocketBackend::Config`), with the same four
fields and defaults as `QtWebSocketBackendConfig` (`reconnectEnabled`,
`initialReconnectDelay`, `maxReconnectDelay`, `backoffMultiplier`) plus one new
field, `connectTimeout` (default 5 s), bounding the initial/reconnect TCP
connect attempt. `waitForConnected(timeout = 5000ms)` blocks the calling
thread on a condition variable until connected or the timeout elapses — the
non-Qt equivalent of pumping the Qt event loop. The constructor takes a
`ws://` URL string (`wss://` throws immediately — see Limitations) and starts
the I/O thread; the thread connects, performs the RFC 6455 handshake, and then
reads framed messages until told to shut down.

**`SocketServer` — server-side transport.** Fronts a `RemoteServer` by
reference — the same non-owning-reference lifetime rule as `QtWebSocketServer`
applies (see Lifetime & ownership). `listen()` binds `127.0.0.1:port` (`0`
lets the OS assign a free port, exactly like the Qt transport) and spawns an
accept thread; each accepted connection gets its own thread that performs the
server-side handshake, then reads framed text messages and calls the
**scoped** `RemoteServer::handle(msg, reply, cid)`.

**Connection scope.** `SocketServer` opts every client into `RemoteServer`'s
connection scope end to end, matching `QtWebSocketServer`:

- **Accept** mints a `ConnectionId` via `_server.openConnection()` and stores
  it on the per-connection state.
- **Dispatch** passes that id to the three-argument `handle()`, so every
  `register` on the connection is attributed to its scope.
- **Teardown** calls `closeConnection(cid)` from a scope guard in the client
  thread, so it runs however the loop exits — failed handshake, peer close,
  read error, or `close()` (which joins those threads). `closeConnection` is
  idempotent, so the overlap during shutdown is harmless.

Without this the raw-socket transport leaked every model it ever registered:
each one outlived its connection with nothing able to reclaim it.

The `reply` callback (which runs on a
`RemoteServer` worker-pool thread) writes back to the originating connection
under a per-connection write mutex; if the connection closed before the reply
is ready, a `weak_ptr` check drops the write silently — the same behavior
`QtWebSocketServer`'s `QPointer` gives. `close()` (also run by the destructor)
is idempotent: it shuts down the listening socket and every client socket
(unblocking their threads' blocked reads/accepts), then joins every thread it
started, so destruction leaves no dangling threads. It marks each connection
closed and then calls `shutdownBoth()` **without** taking that connection's
write mutex: a client thread blocked in `sendAll` against a stalled peer holds
that mutex for as long as the send is stuck, and the shutdown is precisely what
unblocks it — waiting for the lock first would block `close()` (and therefore
the destructor) indefinitely, with no timeout. `shutdownBoth()` is documented
safe to call from any thread for exactly this purpose.

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
  `SimulatedRemoteBackend` and every transport front (`QtWebSocketServer`,
  `morph::net::SocketServer`).**
  `SimulatedRemoteBackend`, `QtWebSocketServer`, and `SocketServer` all store
  `RemoteServer& _server` — a non-owning reference — and forward client
  messages through it. If the server's owning shared_ptr is released while
  such an adapter still references it, subsequent calls dereference a
  dangling reference. The `handle()` path is self-protecting for tasks
  already *in flight* (each captures a shared_ptr copy), but the reference
  member is not — the caller must keep the server alive for the adapter's
  whole lifetime. (`QtWebSocketBackend`/`SocketBackend`, by contrast, hold no
  `RemoteServer` reference: they are clients that reach the server only over
  the socket.)
- **Pending strand tasks capture shared_ptr copies, so model destruction
  mid-flight is safe.** Both backends' `execute` strand tasks capture the model
  `holder` by `shared_ptr` copy (and `RemoteServer`'s also captures the reply
  callback and the moved `Envelope`). A `deregisterModel` that erases the map
  entry while a task is queued or running only drops the *map's* reference; the
  in-flight task holds its own, so the holder stays alive until the task
  completes. `RemoteServer`'s pool tasks additionally keep the server itself
  alive via `shared_from_this()`. `closeConnection` erases the same map entries
  as an explicit `deregister`, so the same guarantee covers it: it never races a
  running `execute` into use-after-free, only prevents *new* lookups.

## Failure modes

| Situation | Local (`LocalBackend`) | Remote (`RemoteServer` / `SimulatedRemoteBackend`) |
|---|---|---|
| `register` with an unregistered `typeId` | N/A — the local factory closure constructs the instance directly; there is no registry lookup and no type-id failure. | `ModelRegistryFactory::create(typeId)` fails → the catch in `dispatchMessage` replies `err "unknown model type: <typeId>"`. Remote registration therefore requires the model to have been macro-registered with `BRIDGE_REGISTER_MODEL`. `SimulatedRemoteBackend::registerModelWithContext` turns that `err` into a thrown `std::runtime_error("register failed: unknown model type: <typeId>")`. |
| `register` with an empty `typeId` | N/A | `err "register requires a typeId"`. |
| `execute` against an unknown model id | Completion resolves with an **untyped** `std::runtime_error("model not found: id=<n>")`. | `err "model not found"` (bare, no id); `SimulatedRemoteBackend` surfaces it as a thrown/`onError` `std::runtime_error("model not found")`. |
| Action handler throws | Caught on the strand; completion resolves with the thrown exception. | Caught on the strand; `err exc.what()` reply, which the client re-throws into the completion. |
| Envelope fails to decode | N/A | `err <decode exception message>` (no `callId` echoed — it couldn't be parsed). |
| Unrecognised `kind` | N/A | `err "unknown envelope kind: <kind>"`. |

Over the WebSocket transport (`QtWebSocketBackend`) the same server-side rows
apply, plus transport-level failures the in-process backends cannot hit:

| Situation | `QtWebSocketBackend` |
|---|---|
| `execute` while the socket is disconnected | Completion resolves immediately with `DisconnectedError`. |
| Socket drops with execute calls in flight | The `disconnected` slot calls `cancelPending(DisconnectedError{})`, resolving every pending completion with `DisconnectedError`. `Bridge` may retry on reconnect. |
| Reply arrives for an unknown/cancelled `callId` | Dropped silently. |
| `register` reply is `err` (e.g. unknown model type) | `registerModel` throws `std::runtime_error("register failed: " + message)`. |
| Malformed reply frame while a sync waiter is parked | The raw frame is handed to the parked `sendSync` loop so it unblocks rather than hanging; decode then fails there. |

`morph::net::SocketBackend` gives the same guarantees over its own transport
(condition-variable waits in place of the nested `QEventLoop`):

| Situation | `SocketBackend` |
|---|---|
| `execute` while the socket is disconnected | Completion resolves immediately with `DisconnectedError`. |
| Socket drops with execute calls in flight | The I/O thread's disconnect handling calls `cancelPending(DisconnectedError{})`, resolving every pending completion with `DisconnectedError`. |
| Reply arrives for an unknown/cancelled `callId` | Dropped silently. |
| `register` reply is `err` (e.g. unknown model type) | `registerModel` throws `std::runtime_error("register failed: " + message)`. |
| `register` reply never arrives (never connected, or disconnects mid-call) | The parked `sendSync` wait wakes on disconnect and throws `"disconnected"`, wrapped as `"register failed: disconnected"` — never hangs. |

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

Over the WebSocket transport the split is different again:

| Callable | Runs on (`QtWebSocketBackend`) |
|---|---|
| `serializeAction` | The **Qt event-loop thread** — `execute` invokes it while building the envelope. |
| `deserializeResult` | The **Qt event-loop thread** — invoked in `onTextMessage` when the matching reply frame arrives. |
| `localOp` | Never invoked (no local models). |

`QtWebSocketBackend` is single-threaded (Qt event loop). `QtWebSocketServer`
receives frames on the Qt thread, hands them to `RemoteServer::handle` (which
runs on the server pool / model strand as above), and marshals the reply *back*
onto the Qt thread before `sendTextMessage`.

`morph::net::SocketBackend` splits the same callables across its own I/O
thread instead of the Qt thread:

| Callable | Runs on (`SocketBackend`) |
|---|---|
| `serializeAction` | The **calling thread** — `execute` invokes it while building the envelope, before handing the frame to the I/O thread's write path. |
| `deserializeResult` | The **I/O thread** — invoked when the matching reply frame arrives. |
| `localOp` | Never invoked (no local models). |

Unlike `QtWebSocketBackend`, `SocketBackend`'s `execute`/`registerModel`/
`deregisterModel` may themselves be called from any thread — there is no
single owning event-loop thread to violate. `morph::net::SocketServer`
receives frames on its own per-connection thread, hands them to
`RemoteServer::handle` (server pool / model strand, as above), and writes the
reply back on whichever thread produces it (serialized per connection by a
write mutex) — there is no separate marshalling step because there is no GUI
thread to marshal onto.

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
| `setReconnectHandler` | `virtual void setReconnectHandler(const function<void()>&)` | Default: no-op. Fires only on the second and later connects. |
| `setConnectHandler` | `virtual void setConnectHandler(const function<void()>&)` | Default: no-op. Fires on every successful connect, first included. |
| `setDisconnectHandler` | `virtual void setDisconnectHandler(const function<void()>&)` | Default: no-op. Fires whenever the transport drops, before any reconnect is scheduled. |
| `setSession` | `virtual void setSession(session::Context)` | Default: no-op. Stamped onto every control envelope (`register`/`registerShared`/`attach`/`assign`/`deregister`) subsequently built. See [Session propagation to control envelopes](#session-propagation-to-control-envelopes). |

### Error types

| Type | Base | Message |
|---|---|---|
| `BackendChangedError` | `std::runtime_error` | `"backend changed before completion resolved"` |
| `BridgeDestroyedError` | `std::runtime_error` | `"bridge destroyed before completion resolved"` |
| `DisconnectedError` | `std::runtime_error` | `"transport disconnected before completion resolved"` |
| `TimeoutError` | `std::runtime_error` | `"execute timed out on the server"` |

### `LocalBackend`

| Method | Notes |
|---|---|
| `explicit LocalBackend(IExecutor& workerPool)` | Constructs with a strand around `workerPool`. |
| `registerModel(typeId, factory)` | Atomically increments `_nextId`, stores the holder under `_regMtx`; also records the id in `_changeAware` when the holder is backend-change-aware. `typeId` is accepted for interface compatibility but not used. |
| `deregisterModel(mid)` | Erases from `_models` and `_changeAware` under `_regMtx`. |
| `notifyBackendChanged()` | Looks up the models recorded in `_changeAware` under `_regMtx`, then posts `onBackendChanged()` (the `IModelHolder` base virtual — no `dynamic_cast`) onto each such model's strand (outside the lock). Cost is O(change-aware models). |
| `execute(mid, call, cbExec)` | Posts `call.localOp` on the model's strand with `ScopedContext`. Returns a `Completion`. |
| `cancelPending(exc)` | Snapshots `_pending`, delivers `exc` to each live state. |

### `RemoteServer`

| Method | Notes |
|---|---|
| `RemoteServer(workerPool, dispatcher, registry)` | Allow-all authorizer. |
| `RemoteServer(workerPool, authorizer, dispatcher, registry)` | Custom authorizer; null → allow-all. |
| `handle(msg, reply)` | Async: posts to pool, decodes, dispatches, calls `reply` once. Unscoped (`cid == 0`). Thread-safe. |
| `handle(msg, reply, cid)` | Like `handle(msg, reply)`, additionally attributing any `register` in `msg` to connection `cid`'s scope. `cid == 0` behaves exactly like the two-argument overload. Thread-safe. |
| `handleInline(msg)` | Sync: runs `dispatchMessage` on the calling thread and returns the reply JSON; intended for `register`/`deregister` only. **Rejects `execute`** — returns an `err` reply without dispatching, because an `execute` reply is produced asynchronously after this call returns. Unscoped. |
| `openConnection()` | Returns a fresh non-zero `ConnectionId` and opens an empty scope for it. Thread-safe. |
| `closeConnection(cid)` | Erases every model still recorded in `cid`'s scope (as `deregister` would) and drops the scope. `cid == 0`, unknown, or already-closed is a no-op — idempotent. Bypasses `IAuthorizer` by design. Thread-safe. |
| `setLogProvider(provider)` | Installs a `LogProvider`; `nullptr` clears. Thread-safe. |
| `setLimitPolicy(policy)` | Installs a `LimitPolicy`; thread-safe. All-zero (default) reproduces pre-existing behavior. |
| `setSupportedVersionRange(min, max)` | Sets the inclusive protocol-version range advertised on `hello`. Defaults to `{kProtocolVersion, kProtocolVersion}`. Throws `std::invalid_argument` if `min > max`. Thread-safe. |
| `health()` | `[[nodiscard]] HealthStatus health() const` — snapshot of readiness/liveModels/inFlight. Cheap; safe from any thread. See [observability.md](observability.md). |
| `setHealthHandler(handler)` | `void setHealthHandler(std::function<void(const HealthStatus&)>)` — fires immediately with the current status, and again whenever readiness changes (currently only `beginShutdown()` triggers a change); `nullptr` clears without firing. |
| `beginShutdown()` | Enters shutdown: subsequent `register`/`execute` envelopes get `err "server shutting down"`; `deregister` still served. Idempotent, irreversible. Flips `health().ready` to `false` and re-invokes any installed health handler. |
| `drainedWithin(deadline)` | `[[nodiscard]] bool drainedWithin(std::chrono::milliseconds deadline)` — blocks (condition-variable wait, not a poll) until every in-flight `execute` has replied or `deadline` elapses. Returns `true`/`false` accordingly. |

### `SimulatedRemoteBackend`

| Method | Notes |
|---|---|
| `explicit SimulatedRemoteBackend(RemoteServer& server)` | References the server. |
| `registerModel(typeId, factory)` | Delegates to `registerModelWithContext(typeId, {}, {})`. |
| `registerModelWithContext(typeId, factory, contextKey)` | Sends `register` envelope via `handleInline`. `factory` ignored. |
| `deregisterModel(mid)` | Sends `deregister` envelope via `handleInline`. |
| `negotiateProtocolVersion()` | Opt-in: sends `hello` via `handleInline`, classifies the reply via `wire::interpretHelloReply`. Throws on an explicit version rejection. |
| `execute(mid, call, cbExec)` | Serialises, sends `execute` via `handle`, returns `Completion` that resolves on reply. |
| `notifyBackendChanged()` | No-op. |
| `cancelPending(exc)` | Snapshots `_pending`, delivers `exc` to each live state. |

### `QtWebSocketBackendConfig` (`QtWebSocketBackend::Config`)

| Member | Type | Default |
|---|---|---|
| `reconnectEnabled` | `bool` | `true` |
| `initialReconnectDelay` | `std::chrono::milliseconds` | `500 ms` |
| `maxReconnectDelay` | `std::chrono::milliseconds` | `30 s` |
| `backoffMultiplier` | `double` | `2.0` |
| `asyncRegistrationEnabled` | `bool` | `false` — opts in to `registerModelAsync` (see [Asynchronous registration](#asynchronous-registration--registermodelasync)); `false` keeps every embedder on `registerModel`'s synchronous behavior. |

### `QtWebSocketBackend` (namespace `morph::qt`)

| Method | Notes |
|---|---|
| `QtWebSocketBackend(serverUrl, dispatcher = defaultDispatcher(), registry = defaultRegistry(), tls = nullopt, cfg = Config{})` | Opens the socket to `serverUrl` in the constructor. `dispatcher`/`registry` params are accepted but unused (models live on the server). `tls` non-null → `wss://`. `tls` is not declared at all when Qt is built with `QT_NO_SSL` (see above). |
| `registerModelAsync(typeId, factory, contextKey, onRegistered, onError)` | Returns `false` immediately unless `cfg.asyncRegistrationEnabled` is `true`. Otherwise: assigns a fresh `callId` (the same counter `execute` uses), records the callbacks in `_pendingRegistrations[callId]`, sends `register` with that `callId`, and returns `true`. `onRegistered`/`onError` fire later from `onTextMessage` (or from `cancelPending` on a disconnect) — never synchronously from this call. |
| `waitForConnected(timeoutMs = 5000)` | Pumps the Qt loop until connected or timeout; returns `_connected`. |
| `negotiateProtocolVersion()` | Opt-in: sends `hello` synchronously (same nested-`QEventLoop` path as `registerModel`), classifies the reply via `wire::interpretHelloReply`. Throws on an explicit version rejection or a `sendSync` failure. |
| `registerModel(typeId, factory)` | Synchronous via nested `QEventLoop`; `factory` ignored. Throws on `err` reply. |
| `deregisterModel(mid)` | **Fire-and-forget** — sends only if connected, does not wait for the ack. |
| `execute(mid, call, cbExec)` | Assigns a `callId`, sends `execute`, returns a `Completion`. Immediate `DisconnectedError` if not connected. |
| `notifyBackendChanged()` | No-op. |
| `cancelPending(exc)` | Drains `_pending` under `_pendingMtx`, delivers `exc` to each state. |
| `setReconnectHandler(handler)` | Stores the handler; invoked on the Qt thread after every *subsequent* connect. `nullptr` clears. |
| `setConnectHandler(handler)` | Stores the handler; invoked on the Qt thread after every successful connect, first included. `nullptr` clears. |
| `setDisconnectHandler(handler)` | Stores the handler; invoked on the Qt thread whenever the socket drops, before reconnect scheduling. `nullptr` clears. |

### `QtWebSocketServerConfig` (namespace `morph::qt`)

| Member | Type | Default |
|---|---|---|
| `maxConnections` | `std::size_t` | `0` (unbounded) |
| `maxMessageBytes` | `std::size_t` | `wire::kMaxEnvelopeBytes` |
| `messagesPerSecond` | `std::size_t` | `0` (unbounded) |
| `handshakeTimeout` | `std::chrono::milliseconds` | `0` (disabled) |
| `idleTimeout` | `std::chrono::milliseconds` | `0` (disabled) |
| `bindAddress` | `QHostAddress` | `QHostAddress::LocalHost` |
| `allowPlaintextExposure` | `bool` | `false` |

`listen()` refuses (returns `false`, logs at `morph::log::LogLevel::error`) when
`bindAddress` is not loopback, no TLS configuration was passed to the
constructor, and `allowPlaintextExposure` is `false`. Loopback binds and any
bind with a TLS configuration are unaffected — this is a new, additive guard,
not a behavior change to the existing loopback-only default.

### `QtWebSocketServer` (namespace `morph::qt`)

| Method | Notes |
|---|---|
| `QtWebSocketServer(server, port = 0, tls = nullopt, cfg = QtWebSocketServerConfig{}, parent = nullptr)` | Fronts `RemoteServer& server`. `tls` non-null → `SecureMode`. `cfg` bounds per-connection resources (see above). Does not start listening. |
| `listen()` | Binds to `cfg.bindAddress:port` and starts accepting; returns success. Refuses (returns `false`, logs at error level) a non-loopback `cfg.bindAddress` with no `tls` and `cfg.allowPlaintextExposure == false`. |
| `port()` | Bound port (OS-assigned when constructed with `0`). |
| `close()` | Stops accepting; calls `closeConnection` for every remaining client (reclaiming its models) before aborting and `deleteLater`ing its socket. Also run by the destructor. |
| `closeGracefully(deadline)` | Opt-in graceful stop: pause accepting, `beginShutdown()`, wait up to `deadline` for the drain (pumping the event loop, plus a short settle window for a reply that just landed), send real close frames (`CloseCodeGoingAway`) to survivors, then `close()` for stragglers. Returns whether the drain finished before `deadline`. |

### `SocketBackendConfig` (`morph::net::SocketBackend::Config`)

| Member | Type | Default |
|---|---|---|
| `reconnectEnabled` | `bool` | `true` |
| `initialReconnectDelay` | `std::chrono::milliseconds` | `500 ms` |
| `maxReconnectDelay` | `std::chrono::milliseconds` | `30 s` |
| `backoffMultiplier` | `double` | `2.0` |
| `connectTimeout` | `std::chrono::milliseconds` | `5 s` |

### `SocketBackend` (namespace `morph::net`)

| Method | Notes |
|---|---|
| `explicit SocketBackend(serverUrl, cfg = Config{})` | Parses `serverUrl` (`ws://` only — throws immediately on `wss://`) and starts the I/O thread, which connects asynchronously. |
| `waitForConnected(timeout = 5000ms)` | Blocks the calling thread on a condition variable until connected or the timeout elapses; returns the current connected state. |
| `registerModel(typeId, factory)` | Synchronous via a parked condition variable; `factory` ignored. Throws on `err` reply or disconnect. Thread-safe, but only one such call may be in flight at a time. |
| `deregisterModel(mid)` | **Fire-and-forget** — sends only if connected, does not wait for the ack. |
| `execute(mid, call, cbExec)` | Assigns a `callId`, sends `execute`, returns a `Completion`. Immediate `DisconnectedError` if not connected. Thread-safe; supports concurrent in-flight calls from multiple threads. |
| `notifyBackendChanged()` | No-op. |
| `cancelPending(exc)` | Drains the pending map, delivers `exc` to each state. |
| `setReconnectHandler(handler)` | Stores the handler; invoked on the I/O thread after every *subsequent* connect. `nullptr` clears. |

### `SocketServerConfig` (`morph::net::SocketServer::Config`)

| Member | Type | Default |
|---|---|---|
| `backlog` | `int` | `64` |

### `SocketServer` (namespace `morph::net`)

| Method | Notes |
|---|---|
| `SocketServer(server, port = 0, cfg = Config{})` | Fronts `RemoteServer& server`. Does not start listening. |
| `listen()` | Binds `127.0.0.1:port` and spawns the accept thread; returns success. |
| `port()` | Bound port (OS-assigned when constructed with `0`), or `0` before `listen()` succeeds. |
| `close()` | Stops accepting, shuts down and joins every client thread and the accept thread. Idempotent; also run by the destructor. |

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
| `setConnectHandler`/`setDisconnectHandler` on `IBackend`, not only `QtWebSocketBackend` | Same no-op-default pattern as `setReconnectHandler` | Connection state is a property of any transport-backed backend; a UI observing it shouldn't have to downcast to a concrete backend type. A purely local backend has no meaningful connection state, so the base-class hook is simply inert for it — no behavior change, matching the existing `setReconnectHandler` precedent exactly. |
| `setDisconnectHandler` fires before reconnect scheduling | Ordering choice, not incidental | An instant successful reconnect must not look, from an observer's perspective, like nothing happened — the disconnected state must be visible even when the very next thing that happens is a fresh `connected`. |
| Strand-per-model | `StrandExecutor` serialises actions per `ModelId` | Actions against the same model run sequentially; different models can run in parallel. No global lock on the pool. |
| Overwrite `session.principal` on remote execute | `authenticate()` result replaces the client claim before dispatch | The client-asserted `Context::principal` is untrusted; a verifying authorizer makes the token-derived identity authoritative so `session::current()->principal` inside a model is trustworthy. Non-verifying authorizers return `nullopt` and change nothing. |
| Opaque model ids | Monotonic counter run through a keyed 4-round Feistel permutation (`detail::OpaqueIdGenerator`), key drawn from `std::random_device` at construction | Guarantees uniqueness (Feistel networks are bijections for any round function) while making ids unguessable without the key; self-contained, no external crypto dependency — same posture as the reference HMAC-SHA256 in `session_auth.hpp`. |
| WebSocket `deregisterModel` is fire-and-forget | Send-only, no nested event loop | A synchronous deregister would need a nested `QEventLoop`, which is typically driven from a destructor (`~BridgeHandler`) and can trip Qt asserts. A lost/undelivered deregister no longer leaks indefinitely: `QtWebSocketServer`'s connection scope reclaims the model at the next disconnect (see Limitations). |
| Connection-scoped cleanup bypasses `IAuthorizer` | `closeConnection` never calls `authorize`/`authorizeInstance`/`authenticate` | It is server housekeeping triggered by the transport's own connection-close event, not a caller action; synthesising a `deregister` envelope would need a token to pass ownership checks and would require the transport to learn ids by parsing replies — recording the owning connection at register time is simpler and cannot desync. |
| `callId`-multiplexed replies | `execute` replies carry a non-zero `callId`; control replies carry `0` | Lets `QtWebSocketBackend` run many concurrent async executes over one socket and match each reply to its `Completion`, while still supporting the parked-nested-loop synchronous `register` path (which uses `callId == 0`). |
| Reconnect handler skipped on first connect | Fired only when `_everConnected` was already true | The initial handler registration is driven by `BridgeHandler` constructors; firing the reconnect handler on the very first connect would double-register. |
| No reconnect for never-connected sockets | `disconnected` schedules a retry only if `_everConnected` | A socket that never reached the server (bad URL / refused) fails fast via `waitForConnected` returning false, rather than backing off forever. |
| Server reply marshalled to the Qt thread | `QMetaObject::invokeMethod(..., QueuedConnection)` with a `QPointer` | `RemoteServer::handle` produces the reply on a pool thread, but `QWebSocket::sendTextMessage` must run on the Qt thread; the weak `QPointer` drops the reply cleanly if the client disconnected meanwhile. |
| `executeTimeout` implementation | A dedicated, lazily-started background thread (`detail::TimeoutScheduler`) per `RemoteServer`, not a per-call thread | `IExecutor` has no delayed-post primitive and `RemoteServer` is transport-agnostic (cannot assume Qt's `QTimer`). One thread amortizes across every timed call; it is only started the first time `executeTimeout` is actually configured, so a server that never uses the feature pays no cost. |
| `messagesPerSecond` algorithm | Per-connection token bucket, capacity = rate, continuous refill, drop (not close) on empty | Simplest correct rate limiter; allows a legitimate one-second burst without penalizing an otherwise well-behaved client. Dropping (vs. closing) keeps a transient burst from taking down the connection — pair with `LimitPolicy::executeTimeout` if bounded caller-side waiting is also needed. |
| Graceful shutdown drains via a shared in-flight counter, not a new `IExecutor::waitIdle` | `RemoteServer` counts its own accepted-but-unreplied executes rather than adding a general drain API to `IExecutor`/`StrandExecutor` | The drain condition morph can define precisely — "every accepted execute has replied" — lives at the server layer, where the work is counted; executor.md's "no graceful drain / `waitIdle`" limitation is deliberately left as-is for raw executor users. |
| Backend-change-awareness captured at registration | `IModelHolder::isBackendChangeAware()` (compile-time answer per model type) + `LocalBackend::_changeAware`, maintained by `registerModel`/`deregisterModel` | Replaces a per-`notifyBackendChanged`-call `dynamic_cast` sweep over every live model with a virtual query done once at registration, and a lookup restricted to the models that actually opted in. No RTTI dependency; cost is O(change-aware models) instead of O(all models) under `_regMtx`. No change to the model-facing contract (`IBackendChangedSink`, `BackendChangedMixin`) or to when/where `onBackendChanged()` runs. |
| `morph::net`'s I/O model | A dedicated I/O thread + `std::condition_variable`, instead of the Qt event loop | Lets `SocketBackend`/`SocketServer` run with no GUI event loop and no Qt dependency, and — as a side effect — lets `SocketBackend` be driven safely from multiple threads (`QtWebSocketBackend` cannot be, since it is pinned to one event-loop thread). |
| `morph::net` frame/handshake implementation | Hand-rolled RFC 6455 (SHA-1 + base64 + HTTP Upgrade + frame codec), not a third-party library | The spec's own interop requirement (a `morph::net` client/server must talk to the real Qt transport and vice versa) rules out a bespoke non-WebSocket framing; hand-rolling avoids adding a dependency to keep morph's default build dependency-free, and RFC 6455's core (handshake + frame codec, including fragment reassembly) is a small, bounded surface. |
| `WsFrameReader` reassembles fragments | Accumulates continuation frames and returns only the completed message | Fragmentation is not an exotic case: a peer fragments whenever a message exceeds its outgoing frame size, and Qt's `QWebSocket` defaults that to 512 KiB. Rejecting fragments broke interop with the transport this project ships, for every payload past that size. Control frames interleaved between fragments pass through untouched, and the reassembled total is bounded by `wire::kMaxEnvelopeBytes` so a stream of tiny continuations cannot grow the buffer without limit. |
| `SocketBackend` runs reconnect handlers on a dedicated thread | Not inline from the I/O thread's connect path | A reconnect handler re-registers models via the synchronous control path, which waits for a reply only the I/O thread's read loop can deliver. Inline, that wait blocks the very thread that would satisfy it, deadlocking the transport with no timeout. |

## Cross-references

| Spec | Relationship |
|---|---|
| bridge.md | `Bridge` owns one `IBackend` and swaps it via `switchBackend()`; `BridgeHandler`/`HandlerBinding` carry the `contextKey` that reaches `registerModelWithContext`. `executeVia` builds the `ActionCall`. |
| session.md | `Context`, `IAuthorizer::authorize`/`authenticate`/`authorizeInstance`/`authorizeRegister`, `ScopedContext`, `session::current()`. The principal-overwrite contract is specified there and enforced here. |
| security.md | Threat model for `RemoteServer`: authorization coverage, the untrusted client principal, and what `register`/`deregister` do *not* check. |
| wire.md | `Envelope`, `encode`/`decode`, `makeOk`/`makeErr`/`makeRegister`/`makeDeregister`, and the `kind` discriminator the server switches on. |
| registry.md | `ModelRegistryFactory::create` (remote model construction, `BRIDGE_REGISTER_MODEL`), `ActionDispatcher::dispatch` (the remote execute call site), and the `Loggable` policy. |
| completion.md | `Completion<shared_ptr<void>>` returned by `execute`, the `CompletionState` the backends track for `cancelPending`, and `cbExec` callback delivery. |
| offline.md | `NetworkMonitorConfig` (the sibling struct whose declaration-order rationale `QtWebSocketBackendConfig` mirrors) and the disconnect/reconnect story the `QtWebSocketBackend` transport participates in. |
| executor.md | `IExecutor` / `ThreadPoolExecutor` (the server worker pool); `qt/qt_executor.hpp`'s `QtExecutor` is the `cbExec` a Qt host uses to deliver completion callbacks onto the Qt thread, while a `morph::net::SocketBackend` host uses a plain `ThreadPoolExecutor`/`MainThreadExecutor` instead — no Qt event loop required. |
| observability.md | The `morph::observe` metrics/trace seam wrapping `RemoteServer`/`LocalBackend` dispatch, and `RemoteServer::health()`/`setHealthHandler()`. |
| testing_strategy.md | `fuzz_dispatch_execute` fuzzes `RemoteServer::handle`/`dispatchMessage` directly; the soak test (`test_soak_switch_backend.cpp`) cycles `switchBackend` between `LocalBackend` and `SimulatedRemoteBackend` under load; the load benchmark (`bench_dispatch_latency.cpp`) baselines dispatch throughput/latency; the adversarial run (`test_qt_websocket_adversarial.cpp`) drives a hostile client against `QtWebSocketServer` and exercises the default (unconfigured) `LimitPolicy`/`QtWebSocketServerConfig`. |

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
- **`register` authorization and id opacity are both opt-in.** `RemoteServer`
  assigns model ids by running a monotonic counter through a keyed 64-bit
  Feistel permutation (`detail::OpaqueIdGenerator`), so ids are no longer
  sequential/trivially guessable — but this narrows *enumeration*, it does not
  replace authorization: a caller who independently learns a valid id can
  still target it. `register` is now gated by the optional
  `IAuthorizer::authorizeRegister` hook, consulted after authentication and
  before instance creation; its **default allows everything**, so an
  unconfigured server still lets any reachable client create instances of any
  known type. `execute` and `deregister` remain gated by the optional
  `authorizeInstance` hook (also allow-all by default) in addition to
  `execute`'s type-level `authorize` step. A hardened multi-tenant deployment
  overrides `authorizeRegister` *and* `authorizeInstance`. See security.md.
- **Connection-scoped cleanup is opt-in, and only `QtWebSocketServer` uses it
  among the shipped transports.** `RemoteServer` reclaims a connection's models
  automatically only when the transport participates in the scope contract
  (`openConnection` / the scoped `handle(msg, reply, cid)` / `closeConnection`
  — see above). `QtWebSocketServer` opts in end to end, so a WebSocket client
  crash or drop now reclaims its models instead of leaking them. A transport
  that never calls `openConnection`/`closeConnection` — or that keeps using the
  unscoped two-argument `handle()`/`handleInline()` — gets none of this:
  `SimulatedRemoteBackend` deliberately stays on the unscoped path (its
  "connection" is the process itself), so its models still live until an
  explicit `deregister` or process exit, unchanged from before this feature.
  `morph::net::SocketServer` opts in exactly as `QtWebSocketServer` does, so a
  `SocketBackend` client that disconnects without an explicit
  `deregisterModel` has its models reclaimed rather than leaked. Deregistration
  therefore remains the caller's responsibility only for a path that does not
  go through a scope-aware transport.
- **WebSocket transport is single-threaded and Qt-bound.** `QtWebSocketBackend`
  must live on the Qt event loop thread; there is no way to drive it from a
  plain worker thread, and `waitForConnected` / the synchronous `register` path
  both pump nested `QEventLoop`s on that thread. Completion callbacks reach the
  GUI only if `cbExec` (typically `QtExecutor`) posts back to the Qt loop.
  `morph::net::SocketBackend` does not have this limitation (see above) — but a
  test or app that mixes a `SocketBackend`/`SocketServer` with a
  `QtWebSocketServer`/`QtWebSocketBackend` peer on the *same* thread that owns
  the `QCoreApplication` must still pump Qt events while any blocking
  `morph::net` call is outstanding, or the Qt-side peer starves (see the
  `SocketBackend`/`SocketServer` Threading note above).
- **`morph::net` is Linux/macOS only.** `TcpSocket` is a thin wrapper over
  POSIX `sys/socket.h`; `MORPH_BUILD_NET=ON` on Windows produces a
  `message(WARNING ...)` and builds nothing. Windows/Winsock2 support is
  documented future work, not implemented today.
- **`morph::net` has no TLS.** `SocketBackend`/`SocketServer` speak plaintext
  `ws://` only; `parseWsUrl` throws immediately on a `wss://` URL. A `wss://`
  variant needing a TLS library (e.g. OpenSSL) is future work.
- **No fragmented WebSocket frames.** `WsFrameReader` throws on `FIN=0` or a
  `CONTINUATION` opcode. This is not a practical limitation for morph's own
  traffic — a `wire::Envelope` is always one JSON line, and the frame format's
  64-bit extended-length field already covers up to `wire::kMaxEnvelopeBytes`
  in a single frame — but a `SocketBackend`/`SocketServer` cannot talk to an
  arbitrary third-party WebSocket peer that fragments its messages.
- **`SocketServer` joins its per-connection threads at `close()`/destruction,
  not eagerly per-disconnect.** A client that disconnects naturally leaves its
  finished-but-unjoined thread handle in an internal list until the whole
  server is closed; this bounds resource growth by the server's lifetime, not
  by connection churn — acceptable for a reference transport (see the
  connection-scoping bullet above) but worth knowing before running a very
  long-lived `SocketServer` under heavy connection churn.
- **`SocketBackend`'s destructor can block up to `Config::connectTimeout`.**
  If destruction races an in-flight (re)connect attempt, the TCP connect phase
  is bounded by `connectTimeout`, but the handshake read that follows a
  successful TCP connect has no separate timeout in this reference
  implementation — a peer that completes the TCP handshake but never speaks
  (or never finishes) the WebSocket Upgrade leaves the I/O thread, and
  therefore the destructor's join, waiting for the OS to notice. This is an
  accepted, documented limitation of the reference implementation, not a bug
  to route around: production code that needs a hard bound on teardown time
  should not construct a `SocketBackend` against an untrusted or unreliable
  peer without an external watchdog.
- **Graceful shutdown never preempts a running action.** `beginShutdown()`,
  `drainedWithin()`, and `closeGracefully()` only stop new work from arriving
  and wait for old work to finish; a model whose action runs longer than the
  caller's `deadline` still finishes on its strand after `drainedWithin`
  returns `false` (and after `closeGracefully`'s hard stop reclaims the
  connection). This is intentional — morph never interrupts a strand task —
  but it does mean a model with no self-imposed bound can make
  `closeGracefully` always hit its hard stop.