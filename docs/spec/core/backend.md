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
  - [Why the callables are function pointers](#why-the-callables-are-function-pointers)
  - [Lifetime contract](#lifetime-contract)
- [The abstract interface — `IBackend`](#the-abstract-interface--ibackend)
- [Connect/disconnect notifications](#connectdisconnect-notifications)
- [Why registration needs a non-blocking path](#why-registration-needs-a-non-blocking-path)
- [The structural registration surface — `bindModel` and `promoteModel`](#the-structural-registration-surface--bindmodel-and-promotemodel)
  - [The delivery executor is the caller's owner](#the-delivery-executor-is-the-callers-owner)
  - [`SynchronousBackendAdapter` — a blocking backend behind a strand](#synchronousbackendadapter--a-blocking-backend-behind-a-strand)
  - [What the bind rule makes of each backend](#what-the-bind-rule-makes-of-each-backend)
- [Error types](#error-types)
- [`LocalBackend` — in-process execution](#localbackend--in-process-execution)
- [`RemoteServer` — server-side message handler](#remoteserver--server-side-message-handler)
  - [Per-model execute ordering](#per-model-execute-ordering)
- [Server-side observability](#server-side-observability)
- [Serving action schemas](#serving-action-schemas)
- [`PayloadCompleteness` — enforcing the action-evolution policy](#payloadcompleteness--enforcing-the-action-evolution-policy)
- [`LimitPolicy` — opt-in resource limits](#limitpolicy--opt-in-resource-limits)
- [Connection scopes](#connection-scopes)
- [`SimulatedRemoteBackend` — adapter for testing](#simulatedremotebackend--adapter-for-testing)
- [`QtWebSocketBackend` — client-side WebSocket transport](#qtwebsocketbackend--client-side-websocket-transport)
- [`QtWebSocketServer` — server-side WebSocket transport](#qtwebsocketserver--server-side-websocket-transport)
- [`SocketBackend` / `SocketServer` — raw-socket WebSocket transport](#socketbackend--socketserver--raw-socket-websocket-transport)
  - [The structural registration surface, natively](#the-structural-registration-surface-natively)
- [Lifetime & ownership](#lifetime--ownership)
- [Failure modes](#failure-modes)
- [Thread context](#thread-context)
- [API reference](#api-reference)
- [`executeInto` — settling the caller's own completion](#executeinto--settling-the-callers-own-completion)
- [Design decisions](#design-decisions)
- [Cross-references](#cross-references)
- [Limitations](#limitations)

## The dispatch struct — `ActionCall`

`detail::ActionCall` bundles everything needed to dispatch one action. It has
three callables — one for each execution path — so the same `ActionCall` can be
used locally or serialised for a remote round-trip:

| Field | Type | Purpose |
|---|---|---|
| `modelTypeId` | `std::string_view` | String id of the target model type (from `ModelTraits`). |
| `actionTypeId` | `std::string_view` | String id of the action type (from `ActionTraits`). |
| `action` | `std::shared_ptr<void>` | Type-erased owner of the action object the two callables below read. |
| `serializeAction` | `std::string (*)(const void* action)` | Serialises `action` to JSON. Only called on the remote path. |
| `deserializeResult` | `std::shared_ptr<void> (*)(std::string_view)` | Deserialises a JSON reply into the opaque result. Only called on the remote path. Never reads the action. |
| `localOp` | `std::shared_ptr<void> (*)(IModelHolder&, void* action)` | Executes `action` directly against a model holder. Only called on the local path. |
| `localOpAsync` | `void (*)(IModelHolder&, std::shared_ptr<void> action, const std::shared_ptr<TaskResumer>&, core::async::StopToken, LocalDone)` | Set instead of `localOp` when the action's handler returns `core::async::Task`: starts it on the model's strand and reports the result or exception through `LocalDone` when the Task completes. Only called on the local path. See [`coroutines.md`](coroutines.md). |
| `stopSource` | `std::shared_ptr<core::async::StopSource>` | Null unless `Bridge::executeVia` armed an execute deadline for a Task handler; the deadline requests stop on it and `LocalBackend` hands its token to the handler. |
| `session` | `morph::session::Context` | Session context. Local backends thread it through a thread-local before invoking `localOp`; remote backends serialise it into the wire envelope. |

There is one member function, `serializeBody()`, which pairs
`serializeAction` with the `action` it reads and throws
`std::runtime_error` if `serializeAction` is null. Every remote backend calls
it rather than invoking the pointer itself, so the borrow is closed in one
place instead of three.

### Why the callables are function pointers

`Bridge::executeVia` builds an `ActionCall` on **every** call, including calls
a `LocalBackend` serves and that therefore never touch `serializeAction` or
`deserializeResult`. The obvious alternative — three `std::function`s, each
capturing a `shared_ptr<Action>` — makes that apparatus cost heap allocations
whichever path the call takes: libstdc++'s small-object buffer is available only to a
trivially copyable target, and a captured `shared_ptr` is not one, so each
stateful callable allocated. Type ids carried as `std::string` copies of
compile-time constants allocate in the same way whenever an id exceeds the
15-character SSO threshold, so they are `string_view`s here.

The behaviour of each callable is a constant of `(Model, Action)`, not of the
call, so it is *addressed* rather than copied: a stateless function pointer,
parameterised on the action it operates on. The action object itself still
needs a home — it is the one genuinely per-call thing — and that home is
`action`, a single `make_shared<Action>`, which the type erasure needed anyway.

**Measured** with `tests/bench/bench_dispatch_allocations.cpp`
(`morph_bench_alloc`, clang 22.1.8 / libstdc++ 16.2.1, Release, 15 processes
per configuration), the `std::function` shape against this one: a local
`Ping -> Pong` round trip costs a median of **16.98** heap allocations per call
(range 16.89–17.05) against **13.79** (range 13.71–14.03), and 1245.7 against
1151.1 bytes per call. The three allocations are the `modelTypeId` string and
the `serializeAction` and `localOp` closure targets.

### Lifetime contract

`serializeAction` and `localOp` **borrow** the action; `action` owns it.
A backend that defers either call beyond the `ActionCall`'s own lifetime must
carry a copy of the `action` handle with it — `LocalBackend::execute` moves it
onto the strand alongside `localOp` for exactly that reason.
`deserializeResult` never reads the action, which is what lets
`SocketBackend` and `QtWebSocketBackend` park it in their pending-reply tables
long after the `ActionCall` is gone.

`modelTypeId` and `actionTypeId` are views, so their referents must outlive the
dispatch. A typed call satisfies this with `ModelTraits<M>::typeId()`, which is
`constexpr` and returns a view of the string literal `BRIDGE_REGISTER_MODEL` was
given. A raw call (`Bridge::executeRaw`) points them into the `RawAction` its
`action` owns, so they live exactly as long as the call. A backend that keeps an
id after the call — in a reply table or a metrics label — copies it; the
in-tree backends copy both into the envelope. A hand-built `ActionCall` must use
a literal or a string that outlives the call, not a temporary.

## The abstract interface — `IBackend`

`detail::IBackend` is the abstract interface every backend implements. `Bridge`
holds a `unique_ptr<IBackend>` and delegates all model operations to it.

| Method | Purpose |
|---|---|
| `registerModel(typeId, factory)` | Registers a new model instance, returns its opaque `ModelId`. |
| `registerModelWithContext(typeId, factory, contextKey)` | Same as `registerModel`, additionally passes a stable identity (e.g. account id). Default implementation drops `contextKey` and forwards to `registerModel` — correct for `LocalBackend` where the factory closure already captures identity. Every backend whose instances live behind a wire protocol carries `contextKey` across: `SimulatedRemoteBackend` and `SocketBackend` override it, and `QtWebSocketBackend` carries it on every `bindModel` (its synchronous verbs refuse). Not cosmetic — `RemoteServer::attachLogIfConfigured` skips the `LogProvider` lookup entirely on an empty `contextKey`, so a wire backend that drops the key leaves the instance with **no** action log rather than a log missing a field. |
| `bindModel(request, cbExec)` | Acquires a model instance and returns a `Completion<ModelId>` delivered on `cbExec`: a private instance, register-or-attach on a key, or a re-point, selected by the request's shape. The only acquire verb `Bridge` calls, with its owner as `cbExec` — see [The structural registration surface](#the-structural-registration-surface--bindmodel-and-promotemodel). |
| `setOwner(affinity)` | Tells the backend the owner its caller (the `Bridge`) runs on; called when the bridge installs it. A backend that keeps state for the bridge's verbs records it and checks, in a debug build, that each verb runs there. Default: ignored. |
| `promoteModel(request, cbExec)` | Files an already-live instance under a key and returns a `Completion<ModelId>` delivered on `cbExec`. The structural counterpart of `assignPrimary`. |
| `instances(typeId, cbExec)` | Lists the live shared keys of `typeId` through a `Completion<vector<string>>` delivered on `cbExec`. What `Bridge::instancesOf` (and so `BridgeHandler::instances()`) asks. Default: answers from `listInstances` and settles before returning. |
| `deregisterModel(mid)` | Removes the model identified by `mid`. |
| `execute(mid, call, cbExec)` | Dispatches `call` against the model identified by `mid`. Returns a `Completion<std::shared_ptr<void>>`. |
| `notifyBackendChanged()` | Called by `Bridge::switchBackend()` after all handlers are re-registered. |
| `cancelPending(exc)` | Resolves every still-pending completion with `exc`. Called on the outgoing backend during `switchBackend()` and in `Bridge`'s destructor. After this call, any later `setValue`/`setException` on those states is a no-op. |
| `setReconnectHandler(handler, exec)` | Installs a callback the backend **posts to `exec`** when it reconnects to its peer — never run on the backend's own thread. Fires only on the *second and later* connects, never the first — used by `Bridge`, which passes its owner, to re-bind its handlers after a drop. Used by backends with transport (`QtWebSocketBackend`, `SocketBackend`). `nullptr` clears it. Default implementation is a no-op. |
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
clears either. The reconnect handler differs on purpose: it re-binds the
bridge's handlers, which is owner work, so the backend posts it to the executor
it was installed with.

`QtWebSocketBackend` is currently the only backend that overrides either:
its `connected`/`disconnected` `QWebSocket` signal slots invoke
`_connectHandler`/`_disconnectHandler` (if installed) at the same points
they already invoke `_reconnectHandler`/schedule a reconnect — see that
section below.

## Session propagation to control envelopes

`Bridge::executeVia` stamps `Bridge::defaultSession()` onto the `ActionCall`
passed to `execute()` (see [bridge.md](bridge.md)), so `execute` envelopes
always carry the current session. Control messages — `register`,
`registerShared` and `attach` (`bindModel` with a key), `assign`
(`assignPrimary`), and `deregister` (`deregisterModel`) — are different: each
is built directly inside the concrete backend, which has no other route to
the `Bridge`'s session except `IBackend::setSession`. Every wire-backed
implementation stamps the session `setSession` last installed onto these
envelopes too, so `RemoteServer::authorizeRegister` sees the caller's
identity and the owner principal it records at `register` time reflects the
registering session — which is what `IAuthorizer::authorizeInstance`'s
ownership check relies on for every instance a `Bridge` registers (see
[session.md](../session/session.md)).

`Bridge` calls `IBackend::setSession` in two places: once from its
constructor (with the just-constructed, typically empty, default session) and
again every time `setDefaultSession()` installs a new one; `switchBackend()`
also calls it on the incoming backend, **before** its
per-binding re-registration loop runs, so every `register`/`registerShared`
envelope built while re-registering handlers on the new backend already
carries the current session. A wire-backed backend that overrides
`setSession` — `SimulatedRemoteBackend`, `SocketBackend`, `QtWebSocketBackend`
— stores the session and reads it back into every control envelope's
`session` field it subsequently builds. `LocalBackend` does not override
`setSession`: the local path never serialises a `Context` onto a wire
envelope, so there is nothing to stamp.

## Why registration needs a non-blocking path

`registerModel`/`registerModelWithContext` are **synchronous**: a backend whose
registration requires a round trip can only implement them by blocking the
calling thread until the reply arrives. On the Qt thread that means a nested
`QEventLoop`, and on a WASM main thread Qt refuses to spin one at all
(`WaitForMoreEvents is not supported on the main thread without asyncify`), so
such a blocking call **aborts the page**; `QtWebSocketBackend` therefore refuses
the synchronous verbs (`std::logic_error`) and registers only through
`bindModel`. And a
`BridgeHandler` constructed inside a running action is on a pool thread that is
not its bridge's owner, so its registration has to be posted rather than made
there. Both reasons lead to one non-blocking acquire verb, `bindModel`, whose
reply is a `Completion` delivered on an executor the caller names.

**Queueing before the first connect.** A non-blocking *private* bind made
before `QtWebSocketBackend`'s socket has finished connecting is **queued**, not
failed — the ordering a single-threaded WASM client must use, since it can never
block waiting for the connection to settle. The queued request is sent, in FIFO
order, the moment `connected` fires next (the first connect included, before
the reconnect handler is posted); if the socket is torn down before ever
connecting, `cancelPending` rejects each queued request's `Completion` exactly
once. A *keyed* bind carries no queue: it is rejected with `"disconnected"`
immediately. See `QtWebSocketBackend`'s own section below.

## The structural registration surface — `bindModel` and `promoteModel`

Two verbs, on `IBackend`, carry every acquire and promote case:

| Verb | Signature | Covers |
|---|---|---|
| `bindModel` | `virtual Completion<ModelId> bindModel(BindRequest, IExecutor& cbExec)` | A private instance, register-or-attach on a key, re-point to a key, give an instance up and bind privately |
| `promoteModel` | `virtual Completion<ModelId> promoteModel(PromoteRequest, IExecutor& cbExec)` | Filing a live instance under a key (`assignPrimary`) |

`BindRequest`'s *shape* — not a verb name — selects the behaviour:

| `primary` | `current` | Meaning |
|---|---|---|
| empty | `ModelId{0}` | Private instance, never enters the shared directory. |
| non-empty | `ModelId{0}` | Register-or-attach on `(typeId, primary)`. |
| non-empty | non-zero | Re-point from `current` to `(typeId, primary)`. |
| empty | non-zero | Give `current` up and bind a private instance instead. |

One verb rather than one per case, because the cases degrade into each other
exactly this way (a register-or-attach with an empty key *is* a private
register; a re-point from nothing *is* a register-or-attach), and because every
case shares the one property that matters: the reply is a `Completion`, and
the caller names the executor it is delivered on. Both request types own their
strings: a bind may outlive the frame that issued it.

### The delivery executor is the caller's owner

`Completion<T>` posts its handlers to the executor it was built with
(`completion.md`). Passing that executor into the verb makes the delivery
thread an argument rather than a property of the backend:

- The continuation runs where `cbExec` says, whatever thread the backend
  settles on.
- `cbExec` is a **reference**, not a pointer: "deliver nowhere" cannot be
  spelled.

`Bridge` passes its owner, so every bind and promote reply is a task on the
owner and touches the bridge's state there — see
[bridge.md](bridge.md#registration-readiness--the-bind-rule). A backend that
settles before returning (`LocalBackend`, `SimulatedRemoteBackend`, the
default) has its outcome read and applied by the call that issued it, so a
handler over it is bound when its constructor returns.
`tests/test_backend_registration_surface.cpp` pins the delivery: a backend
settles from a thread asserted *not* to be the caller's, and the continuation
runs only when the caller's `MainThreadExecutor` is drained, on the draining
thread.

### The default implementations

A backend that overrides nothing gets `IBackend`'s `bindModel`/`promoteModel`.
The default `bindModel` has no shared directory: every shape binds a private
instance through `registerModelWithContext`, and a non-zero `current` is
released once the replacement is acquired, so a throwing acquire never strands
the caller with neither instance. The default `promoteModel` calls
`assignPrimary` and echoes `mid` back, including for `assignPrimary`'s
documented no-op cases, which are not failures. Both settle before returning,
and an exception from the verb they call rejects the `Completion` rather than
propagating, so a caller has one failure channel.

`LocalBackend` and `SimulatedRemoteBackend` override `bindModel` to keep their
shared directories (register, register-or-attach, re-point), still settling
before returning. `QtWebSocketBackend` and `SocketBackend` override it with a
genuinely non-blocking path.

### `SynchronousBackendAdapter` — a blocking backend behind a strand

A decorator (`morph::backend::SynchronousBackendAdapter`) that wraps an
`IBackend` and runs **every** verb of it on one control strand over an executor
named at construction:

```cpp
auto local = std::make_shared<morph::backend::LocalBackend>(pool);
morph::backend::SynchronousBackendAdapter adapter{local, control};
```

The control strand is the wrapped backend's one owner, so a backend that keeps
its state without a lock (`LocalBackend`) stays correct behind it, and the
caller's thread never pays for a blocking control call.

- **It does not make blocking non-blocking.** The wrapped backend still blocks.
  What changes is which thread pays: `bindModel`/`promoteModel` run the wrapped
  backend's own `bindModel`/`promoteModel` on the strand (with
  `inlineExecutor()`, since the strand is where it settles) and settle the
  caller's `Completion`, on the caller's executor, from there. A
  single-threaded WASM main thread has no such executor to offer, which is why
  `QtWebSocketBackend` implements the surface natively instead.
- **The executor is required.** An adapter that ran the call inline when handed
  nothing would be a `bindModel` that blocks on some configurations and not
  others.
- **It belongs to its caller's owner** (`setOwner`, not forwarded — the wrapped
  backend's owner is the strand). `bindModel`, `promoteModel` and
  `cancelPending` keep the list of pending binds there, without a lock.
- **It cancels its own pending binds rather than only the wrapped backend's.**
  A bind settles from a task on the strand, holding a promise the wrapped
  backend never sees, so a plain forward of `cancelPending` would reach none of
  them, and a bind cancelled by `~Bridge` or `switchBackend` would go on to
  resolve successfully afterwards. The adapter keeps a `weak_ptr` to each
  dispatched promise and rejects the live ones first (with the same amortised
  compaction as [`LocalBackend`'s pending
  list](#the-pending-list-and-its-amortised-compaction)), then posts the
  wrapped backend's own `cancelPending` to the strand.
- **It stops a control call the strand has not started yet.** Each dispatched
  task carries a `PendingControl` record — its promise plus an
  `atomic_bool cancelled` — and checks the flag before running; `cancelPending`
  sets it before rejecting. Otherwise a queued bind would still run after the
  caller was told it was cancelled, leaving an instance nothing will ever
  deregister. A task already running cannot be recalled.
- **The synchronous verbs wait for the strand** (`registerModel`,
  `registerModelWithContext`, `assignPrimary`, `listInstances`), so they must
  not be called from the executor's only thread.
- **Ordering and teardown.** Every verb is queued on the one strand, in the
  order issued. `~SynchronousBackendAdapter` waits for every queued and
  in-flight call (`ModelStrands::drain`), so the executor must still be running
  tasks when the adapter is destroyed. The single-threaded WebAssembly build has
  no thread to wait for, and drops the calls still queued.

### Backends with a genuinely non-blocking path

`QtWebSocketBackend` and `SocketBackend` override `bindModel` and settle the
`Completion` when their reply arrives. The request-shape mapping is the same in
both — empty `primary` with a zero `current` is a private `register`; empty
`primary` with a live `current` gives that instance up first and then binds
privately; a non-empty `primary` with a zero `current` is a shared `register`;
with a live one it is an `attach`. They differ in transport and in gating:

- `SocketBackend::bindModel` posts the request to the I/O loop and settles from
  the loop when the reply arrives — always non-blocking. See [The structural
  registration surface, natively](#the-structural-registration-surface-natively).
- `QtWebSocketBackend::bindModel` builds the envelope, assigns a `callId`,
  sends, and returns an unsettled `Completion` that `onTextMessage` settles —
  always non-blocking.

There is one send path per backend (`sendControl` / `sendControlAsync`) and one
pending map, so the "encode before recording the pending entry" invariant and
the `env.session` stamp each exist once. `tests/qt/test_qt_websocket.cpp` pins
the Qt path end to end against a real `RemoteServer` (queue before connect,
register-or-attach, re-point, degrade-to-private, reject on a dead socket,
promote), and `examples/common/testkit/test_wasm_registration_path_native.cpp`
pins it through `Bridge`.

### What the bind rule makes of each backend

> **The bind is a `Completion` delivered on the bridge's owner. A backend that
> can settles it before returning. `execute` dispatches at once when the
> binding's `currentId` is set, and chains on the bind otherwise.**

`registerHandler` never blocks and never waits: it issues `bindModel` with the
owner as the delivery executor and returns. A failed bind rejects every held
call with the bind's error; a handler destroyed while its bind is in flight
rejects every held call with `HandlerDestroyedError`. No caller gates on
registration.

| Backend | `bindModel` | Bound when `registerHandler` returns |
|---|---|---|
| `LocalBackend`, `SimulatedRemoteBackend`, test doubles that override nothing | settles before returning | yes |
| A backend wrapped in `SynchronousBackendAdapter` | settles from the adapter's control strand | no — `execute` chains on the bind |
| `QtWebSocketBackend` | native, settles from `onTextMessage` | no — `execute` chains on the bind |
| `SocketBackend` | native, settles from the I/O loop | no — `execute` chains on the bind |

A backend that violates the one-settle contract by firing twice cannot be
observed doing it from `Bridge`: `CompletionState` drops the second settle
before any bridge code sees it. A backend that throws out of `bindModel` —
`IBackend` is a public extension point — has the throw applied as the bind's
failure.

## Error types

Five exception types are thrown into in-flight `Completion`s. The first four are
raised by a backend; `ClientTimeoutError` is raised by `Bridge` itself, but is
declared alongside them so callers catch every dispatch failure from one header:

| Type | Trigger | Purpose |
|---|---|---|
| `BackendChangedError` | `Bridge::switchBackend()` runs | GUI can retry on the new backend or surface a "backend changed" message. |
| `BridgeDestroyedError` | `Bridge` is destroyed | In-flight completions are cancelled because the bridge is gone. |
| `DisconnectedError` | Transport drops mid-call (e.g. WebSocket disconnect) | Framework retries the call on reconnect if the backend supports it; otherwise the GUI's `.onError(...)` runs. |
| `TimeoutError` | Server-side `LimitPolicy::executeTimeout` elapses | Distinguishes a bounded-wait timeout from any other `err` reply, so callers can retry or surface a specific "request timed out" message. |
| `ClientTimeoutError` | Client-side `Bridge::setExecuteDeadline` elapses with *no* reply of any kind | Bounds the caller's wait when nothing comes back at all (a dropped frame, a hung server). Unlike `TimeoutError` it carries no evidence the server ever saw the request — see [`completion.md`](completion.md), "Client-side execute deadline". |

## `LocalBackend` — in-process execution

`LocalBackend` is the concrete in-process backend. It owns a
`ModelStrands` (core-cpp's `KeyedStrands` over the `IExecutor&` worker pool,
typically a `ThreadPoolExecutor`; see [executor.md](executor.md)) and a
`detail::InstanceDirectory` holding its live model instances.

Both it and `RemoteServer` keep those instances in one
`detail::InstanceDirectory` (`core/detail/instance_directory.hpp`): one record
per instance holding its holder, owner principal, attach count, directory key
and hydration state, plus a `(typeId, primary)` index and a per-type index for
`listInstances`. It holds no lock: it belongs to its backend's owner — the
bridge's owner for `LocalBackend`, the server strand for `RemoteServer` — and
the neighbouring decisions (the `maxLiveModels` admission check, the
connection-scope update) are made in the same owner task, so none can straddle
a directory change. Register-or-attach, the lazy eviction of an instance whose
first action failed, and promotion by `assignPrimary` are each written once,
there, rather than once per backend.

**One owner.** The registry, the pending list and the record of running Task
handlers belong to the caller's owner (the bridge's, given through `setOwner`)
and are touched only there, without a lock; each verb checks it in a debug
build. Only the strand tasks the backend posts run elsewhere, and they reach
nothing of the backend but what each carries — the holder, the call, and the
cancel record below.

**Lifecycle:**
- `bindModel` — overridden to keep the shared directory: register (private),
  register-or-attach (a key), or re-point (a key and a live `current`), on the
  owner, settled before returning.
- `registerModel` — increments a counter, calls the factory, records the new id in `_changeAware` if the holder's
  `isBackendChangeAware()` is `true`, files the holder in the instance
  directory, returns the new `ModelId`.
- `deregisterModel` — releases one attachment
  through the instance directory, which unfiles and destroys the instance in a
  single step when the last one goes away; `_changeAware` is erased only when
  the instance is actually destroyed.
- `execute` — looks up the holder; if `mid` is unknown
  it immediately resolves the completion with
  `std::runtime_error("model not found: id=<n>")`. Otherwise it tracks the
  completion in the pending list, posts `localOp` on the model's strand
  (serialised per-model), sets up a `ScopedContext` (from `call.session`) before
  calling `localOp`, and returns the `Completion`. The strand task also emits
  `executeLatencyMs`/`executeInFlight`/`executeErrors` and calls
  `beginSpan`/`endSpan` around `localOp` — see [observability.md](observability.md).
  Both `registerModel` and `deregisterModel` emit `registerCount`/`deregisterCount`.
- `cancelPending` — takes the pending list, delivers `exc` to every still-live
  state, requests stop on every Task handler still running (its
  `LocalRun::stopSource`), and re-arms the compaction threshold. A run that has
  not started yet reads the cancel record its task carries and fails without
  starting: the record is an immutable node the owner pushes and publishes with
  a release store, read by the run on its strand with an acquire load. See
  [The pending list and its amortised compaction](#the-pending-list-and-its-amortised-compaction).
- `notifyBackendChanged` — looks up only the models recorded in
  `_changeAware` (populated at registration from
  `IModelHolder::isBackendChangeAware()` — a compile-time answer per model type,
  no `dynamic_cast`); then **posts** `holder->onBackendChanged()`
  (the `IModelHolder` base virtual) onto each such model's strand (the holder
  captured by `shared_ptr`). Cost is O(change-aware models), not O(all models).
  Delivery is asynchronous and serialised against that model's `execute` tasks,
  on a pool thread rather than the owner. It runs without a session, even while a Task
  handler of that model instance is suspended: the strand installs a
  suspended handler's session around the coroutines resumed on its instance
  only, and a posted callable such as this one is not one of them (see
  [coroutines.md](coroutines.md), "The handler's resumer"). The same holds
  for an action queued behind the handler, which installs its own session when
  it starts. A detached chain that a finished handler A left behind is a
  resumption, though: when it comes back after handler B of the same instance
  started, it runs under B's session and B's resumer.
- `setReconnectHandler`/`setConnectHandler`/`setDisconnectHandler` — no-op (no transport to (dis)connect).
- `setSession` — not overridden (the default no-op stands): the local path never serialises a `Context` onto a wire envelope, so there is nothing to stamp.

Each model instance gets its own strand so actions are serialised per-model
without a global lock on the pool.

### The pending list and its amortised compaction

`_pending` is a `vector<weak_ptr<CompletionState<shared_ptr<void>>>>` owned by
the backend's owner. It exists for exactly one reader — `cancelPending`, which swaps it
out and fails everything still live on a backend swap or a `~Bridge`. Nothing
else consults it, and nothing unlinks from it when a completion settles: an entry
simply becomes a dead `weak_ptr` that `cancelPending`'s `weak.lock()` skips.

Dead entries therefore have to be reclaimed by a sweep, and the question is only
how often. Sweeping on **every** `execute` makes admitting one call cost one
atomic `weak_ptr::expired()` load per entry already in the list, under the mutex, before any work starts — so a
burst of *n* costs O(n²). Measured against one parked model on an 8-core Linux
box (clang 22.1.8, `-O2`), timing only the `execute()` calls themselves:

| queued executes | total admission time | mean per admission | mean over the last 10% |
|---|---|---|---|
| 1 000 | 0.59 ms | 0.59 µs | 0.82 µs |
| 4 000 | 5.01 ms | 1.25 µs | 2.19 µs |
| 16 000 | 77.8 ms | 4.86 µs | 9.57 µs |
| 32 000 | 362 ms | 11.3 µs | 24.1 µs |

Total time quadruples per doubling of *n* and the per-admission cost doubles —
the O(n²)/O(n) pair, not an artefact of some constant.

The sweep is now **amortised**: `trackPending` sweeps only when `_pending.size()`
reaches `_compactAt`, and each sweep re-arms `_compactAt` at twice the number of
entries that survived it (floor 32, below which sweeping costs more than it
reclaims). A sweep costs O(size) and at least `_compactAt / 2` appends must
happen before the next one, so admission is amortised O(1) at any depth. On the
same benchmark the 32 000-execute case drops from 362 ms to 7.6 ms — within noise
of the 7.6 ms measured with the sweep deleted outright, so what is left is the
`make_shared`, the registry lookup and the strand post, not the sweep.

Two properties are the price and the guarantee:

- **Memory.** The list is bounded at twice the live count plus the floor, rather
  than at exactly the live count. `trackedPendingCount()` makes that observable;
  the fixture in `tests/test_backend_extra.cpp` drives 3 072 settled-and-dropped
  admissions past 48 parked ones and measures 112 entries left, against the 3 120
  an uncompacted list would hold.
- **`cancelPending` is unchanged.** It never saw dead entries in the first place
  — `weak.lock()` has always skipped them — so carrying them for longer changes
  nothing it observes. Every live state admitted across every sweep is still
  reached, which is what the same fixture's second assertion checks. `cancelPending`
  also resets `_compactAt` to the floor, since it has just emptied the list.

What is **not** guarded by a test is the admission latency itself: a wall-clock
assertion on a shared CI runner would be a flake rather than evidence, so the
numbers above come from a benchmark and the test guards only the bound and the
cancellation.

## `RemoteServer` — server-side message handler

`RemoteServer` receives JSON envelopes (`morph::wire::Envelope`) from any
transport and executes the corresponding model operations via an
`ActionDispatcher`. Authorization is delegated to an `IAuthorizer` that defaults
to allow-all. It derives from `std::enable_shared_from_this<RemoteServer>`.

**One owner: the server strand.** The server owns one strand over its worker
pool (`exec::OwnerStrand`, reachable as `strand()`). Its state — the instance
registry and shared-instance directory, the connection scopes, the in-flight
count, the drain waiters, `ready` and the shutdown flag — is touched only in
tasks on that strand, so none of it has a lock. Configuration is a
`ServerConfig`, fixed at construction and read without a lock because nothing
writes it afterwards. The model instances themselves run on their own strands
(`ModelStrands`, one per `ModelId`), as on `LocalBackend`.

**Cross-thread surface.** Every public member is callable from any thread:

| Member | What crosses | Answered |
|---|---|---|
| `handle(msg, reply[, cid])` | Decodes on the calling thread, posts one task to the server strand | `reply`, once, from a pool thread |
| `handleInline(msg[, cid])` | Posts a control envelope to the server strand and waits (inline when already on it) | Its return value |
| `openConnection()` | Draws the id from an atomic, posts the scope's creation | The id, at once |
| `closeConnection(cid)` | Posts | — |
| `health(replyExec)` | Posts | A `Completion<HealthStatus>` settled on the strand, delivered on `replyExec` |
| `beginShutdown()` | Posts | — |
| `drainedWithin(deadline, replyExec)` | Posts | A `Completion<bool>` settled on the strand, delivered on `replyExec` |
| `payloadCompleteness()`, `strand()` | Read immutable members | At once |

A task posted to the server strand by one thread runs after every task that
thread posted before it, so a verb called after another on one thread sees its
effect: an envelope handed to `handle()` after `beginShutdown()` returned is
refused; a `health()` asked after `closeConnection()` counts the reclaimed
models as gone.

**Must be heap-allocated via `std::make_shared`.** Every task the server posts
— to its own strand, to a model's strand, to its timers — captures
`shared_from_this()`, so the server outlives its queued work however the last
external reference is dropped.

**Wire format.** All requests and replies are JSON `morph::wire::Envelope`. The
`kind` field discriminates the message types:

| `kind` | Request fields | Reply | Notes |
|---|---|---|---|
| `register` | `typeId`, `[contextKey]` | `ok` with `modelId` (body empty) | Authenticates the caller (`_authorizer->authenticate(env.session)`), stamping the verified principal onto `env.session.principal` (clearing it when unauthenticated) exactly as `execute` does, then consults `_authorizer->authorizeRegister(env.session, typeId)` — a `false` reply is `err "unauthorized"` and **no instance is created**. Only then creates the model via the `ModelRegistryFactory` and records the (already-verified) principal as its owner. Empty `typeId` → `err "register requires a typeId"` (checked before authorization). If `contextKey` is non-empty, consults `ServerConfig::logProvider` (if set) and, when it returns a non-null log, calls `holder->attachActionLog(log, contextKey)`. The assigned `modelId` is an **opaque** (non-sequential) value — see below. |
| `deregister` | `modelId` | `ok` or `err` | Consults `authorizeInstance` against the recorded owner (denied → `err "unauthorized"`); otherwise erases the model and its owner entry from the registry. |
| `execute` | `modelId`, `modelType`, `actionType`, `body`, `session` | `ok` with `body` or `err` | See the execute flow below. |
| `hello` | `protocolVersion` | `ok` with `body` = `ProtocolRange`, or `err "protocol version unsupported"` | Protocol-version negotiation, exchanged once per connection before any `register`/`execute`. Carries no `session` and is not authorized — orthogonal to `IAuthorizer`. The range's `capabilities` lists `"cancel"`. See [wire.md](wire.md#protocol-version-negotiation). |
| `cancel` | `cancelCallId`, `session` | `ok` (the cancel's own `callId`), always | Stamps the verified principal, then requests stop on the run of the `execute` filed under `cancelCallId` **on the same connection**, if the cancel's verified principal is the execute's and the cancel passes the execute's own `authorize` and `authorizeInstance`. Anything else — unknown, finished, another connection's, another principal's, refused, unscoped — is the same `ok` and changes nothing. See [wire.md](wire.md#cancelling-a-call). |
| `schemas` | `typeId`, `session` | `ok` with `body` = `{actionType: schema}`, or `err` | Empty `typeId` → `err "schemas requires a typeId"` (checked before authorization). Authenticates, then consults `_authorizer->authorize(env.session, typeId, {})` — the same type-level read hook `instances` uses; denied → `err "unauthorized"`. Answers from `ActionDispatcher::schemasJson(typeId)`; a type with no registered actions yields `{}`, not an error. See [Serving action schemas](#serving-action-schemas). |

**Execute flow.** Admission runs on the server strand (`dispatchExecute`), the
action on the model's strand. Admission, in order:

1. **In-flight cap.** With `LimitPolicy::maxInFlightExecutes` set and the
   in-flight count at it → `err "server busy"`. The count is server-strand
   state, so the check and the increment in step 7 cannot be separated by
   another admission: the cap is exact.
2. **Authorize.** `_authorizer->authorize(env.session, env.modelType, env.actionType)`.
   Denied → `err "unauthorized"` (with the request's `callId`), no dispatch.
3. **Authenticate / make the principal authoritative.** After `authorize`
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
   principal, never an attacker-chosen one (see security.md). This is the only
   place the principal is made authoritative; the verifying implementation
   lives in `SigningAuthorizer` (`session_auth.hpp`, cross-ref security.md).
4. **Look up the model** in the registry, with its recorded owner and
   hydration state. Missing → `err "model not found"` (with `callId`), no
   dispatch. (The remote message is the bare string `"model not found"`,
   without the id — unlike the `LocalBackend` path, which resolves the
   completion with `std::runtime_error("model not found: id=<n>")`.)
5. **Per-instance authorize.** `authorize` above saw only the model *type*; this
   step consults `_authorizer->authorizeInstance(env.session, env.modelType,
   env.actionType, modelId, owner)` with the target instance id and its recorded
   owner. Denied → `err "unauthorized"` (with `callId`), no dispatch. The default
   hook allows all; `env.session` already carries the verified principal
   stamped in step 3, so the hook compares the recorded owner against it.
6. **Payload completeness** (opt-in; see
   [`PayloadCompleteness`](#payloadcompleteness--enforcing-the-action-evolution-policy)).
7. **Reserve the in-flight slot**, arm `LimitPolicy::executeTimeout` if one is
   configured, file the call as cancellable if its handler is a Task and it
   arrived on a connection scope (see the `cancel` row above), and **post to
   the model's strand** a task that enters the
   instance's action gate, installs a `ScopedContext` from the (now possibly
   rewritten) `env.session`, calls `dispatch(modelType, actionType, *holder,
   body)` on the server's dispatcher, and replies `ok` with the serialised
   result. Any `std::exception` thrown by the dispatch is caught on the strand
   and returned as `err exc.what()` with the `callId`. The task holds the
   server (`shared_from_this()`) and the model's holder, so neither can go
   before the reply is delivered; it reads the dispatcher via
   `self->_dispatcher`, never a bare reference capture.

The reply is sent exactly once, by whichever of the model strand's finish and
the `executeTimeout` gets there first. Before sending it, that path posts the
in-flight decrement — and the call's withdrawal from the cancellable calls —
back to the server strand, so a `health()` or
`drainedWithin()` asked by someone who has seen the reply already counts it as
finished.

Any envelope that fails to decode produces `err` carrying the decode
exception's message, and a log line (see [Server-side
observability](#server-side-observability)). An unrecognised `kind` produces
`err "unknown envelope kind: <kind>"`. Any `std::exception` thrown while
handling a decoded envelope — including one out of an `IAuthorizer` hook during
admission — is caught and returned as an `err` reply carrying `exc.what()` and
the request's `callId`. Nothing is held across it: the next envelope is simply
the next task on the server strand.

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
unchanged; only the assigned *values* are not sequential. This is
defence-in-depth, not a substitute for `authorizeInstance`: a caller who
independently learns a valid id (from its own register, or a leak) can still
target it, so per-instance ownership remains the actual authorization
boundary — see [security.md](../security.md).

**`handle(msg, reply)`** — asynchronous entry point. Decodes the envelope once,
on the calling thread, and posts it to the server strand, which dispatches by
`kind` and calls `reply` exactly once. A three-argument overload,
`handle(msg, reply, cid)`, additionally attributes any `register` in `msg` to a
connection scope — see "Connection scopes" below.

**`handleInline(msg)`** — synchronous entry point for control envelopes
(`register`, `deregister`, `attach`, `assign`, `instances`, `schemas`,
`hello`, `cancel`), the path `SimulatedRemoteBackend` uses. It posts the envelope to the
server strand and blocks until the reply is written; when the caller is
already on the server strand — host code the strand itself is running, such as
a model constructor or a `LogProvider` that registers another model — it runs
inline instead, since waiting there would wait on itself. It **rejects
`execute`** up front: an `execute` reply is produced later, on the model's
strand, after `handleInline` would have returned, so `handleInline` decodes
the envelope first and, if its `kind` is `execute`, returns an `err` reply
(`"handleInline does not support execute (reply is asynchronous)"`) without
dispatching. A malformed envelope is dispatched like any other and gets the
canonical decode-error reply.

*Why it waits rather than running inline.* Running a control envelope inline
on the caller's thread would touch the registry off its owner. The wait is
safe where the caller is not the thread the server strand needs: the strand
runs on the pool, so a pool thread inside a running action (a handler
constructed from an action handler — the "synchronous re-entry" case in
[concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md#synchronous-re-entry-into-a-backend-from-a-pool-thread))
waits while another pool thread runs the strand's task. It cannot be satisfied
when no other pool thread is free: a one-thread pool calling `handleInline`
from inside its own task, or every pool thread blocked in `handleInline` at
once. That is the price of one owner on a verb that must answer synchronously;
registration becoming asynchronous removes it.

**`ServerConfig`** — everything configured rather than learned, given to the
constructor (`RemoteServer(pool[, authorizer], config[, dispatcher,
registry])`) and fixed for the server's life. No member of `RemoteServer`
changes it.

| Field | Default | Meaning |
|---|---|---|
| `limits` | all `0` | `LimitPolicy`, below. |
| `logProvider` | null | `LogProvider` consulted, on the server strand, whenever an instance is constructed with a non-empty `contextKey` (every `register`, and every `attach` that creates). How `RemoteServer` attaches action logs to instances it creates on behalf of remote clients — the factory closure, on the client side, cannot capture a server-side log. |
| `healthHandler` | null | Called with the server's `HealthStatus` once from the constructor (ready, nothing live, nothing in flight), and again on the server strand when readiness changes (`beginShutdown()`). |
| `minProtocolVersion`, `maxProtocolVersion` | `kProtocolVersion` both | The inclusive range answered to `hello`. `min > max` makes the constructor throw `std::invalid_argument`. |
| `payloadCompleteness` | `Lenient` | See [`PayloadCompleteness`](#payloadcompleteness--enforcing-the-action-evolution-policy). |

```cpp
using LogProvider = std::function<std::shared_ptr<morph::journal::IActionLog>(
    std::string_view modelType, std::string_view contextKey)>;
```

A deployment that wants a different configuration constructs a different
server; nothing in the server needs to reason about a limit or a range changing
under an admission it is making.

**`health()`** — a readiness snapshot, detailed in
[observability.md](observability.md). Returns a `Completion<HealthStatus>`
settled on the server strand, `HealthStatus{ready, liveModels, inFlight}`:
`liveModels` from the registry, `inFlight` from the in-flight count — the same
count `LimitPolicy::maxInFlightExecutes` gates, `drainedWithin()` waits on and
the `executeInFlight` metric reports. `ready` starts `true` and is flipped to
`false`, once and for good, by `beginShutdown()` — there is no un-shutdown.
The completion is delivered on the `replyExec` the caller passes, the owner it
attaches its callbacks from: a `Completion`'s handlers are attached and run on
one executor, and only the caller knows which one it is on.

**Metrics and tracing.** The `register`/`deregister` branches emit
`registerCount`/`deregisterCount`; admission and the in-flight decrement emit
`executeInFlight` (both on the server strand), and the model strand's task
emits `executeLatencyMs`/`executeErrors` and calls `beginSpan`/`endSpan` around
the `ActionDispatcher::dispatch` call. All are no-ops unless a sink is installed
via `morph::observe::setMetricSink`/`setTraceSink` — see
[observability.md](observability.md).

### Per-model execute ordering

The guarantee:

> For one `modelId`, the order in which `handle()` was called for its
> `execute` envelopes is the order the model runs them.

It is two strands' order, composed. `handle()` posts each envelope to the
server strand in the order it is called; the server strand admits them in that
order and posts each admitted one to its model's strand in that order; the
model's strand runs its tasks in the order they were posted. Nothing between
the transport and the model runs concurrently with another request for the
same model, so there is no window in which two of them could swap.

**Why admission is on the server strand, not on the model's strand.** A
single hop — the transport posting an `execute` straight to its model's strand
and admitting it there — is shorter, but admission reads the registry, which
belongs to the server strand. It would also queue a lookup behind the model's
running action: an `execute` for a model whose connection just closed would not
learn "model not found" until the action in front of it finished
(`tests/test_remote_connection_scope.cpp`, "an in-flight execute completes
safely across a disconnect", holds that it answers at once). The server strand
never runs a handler, only admissions and control envelopes, so a rejection is
answered in its own turn and never waits on a busy model.

The cost is that admissions for every model are serial: the authorizer's hooks,
the registry lookup and (when enabled) the payload-completeness parse run one
at a time on the server strand. The handlers themselves still run in parallel,
one strand per model.

**What is not ordered.** Only same-model requests, and only relative to
`handle()` call order:

- **Across models, execution is not ordered** — each model's strand runs
  independently; only their admissions share the server strand.
- **Across threads, "send order" means "`handle()` call order"**. A single
  transport connection delivers messages on one thread, so for one client the
  two coincide. Two connections calling `handle()` concurrently for the same
  model have no send order between them to preserve, and get whichever order
  their posts reach the server strand in.
- **Completion order is not constrained** beyond what the model's action gate
  gives: how long each action takes is the model's business.

`tests/test_remote_execute_ordering.cpp` pins the guarantee: a first request
whose admission is held for 200ms on a two-thread pool still runs before the
request sent after it; a request refused at shutdown or by a throwing
authorizer hook, or rejected between two others, holds nothing for the ones
around it; and two threads calling `handle()` for one model against a
one-thread pool, with one of them stalled inside the pool's `post`, neither
deadlock nor swap.

### Protocol-version negotiation

`ServerConfig::minProtocolVersion`/`maxProtocolVersion` set the inclusive
range this server advertises in reply to `"hello"`. Both default to
`kProtocolVersion` — this build's single supported version — so an
unconfigured server's behavior only changes for clients that opt into sending
`"hello"` in the first place. An empty range (`min > max`) makes the
constructor throw `std::invalid_argument`. See
[wire.md](wire.md#protocol-version-negotiation) for the full negotiation story,
including how `SimulatedRemoteBackend` (synchronously, over `handleInline`)
and `QtWebSocketBackend` (as a `Completion` settled by the reply) each expose
an opt-in `negotiateProtocolVersion`.

### Serving action schemas

`RemoteServer` answers the `"schemas"` kind with
`ActionDispatcher::schemasJson(typeId)` — the `{actionType: schema}` document
for every action registered under that model type, each schema carrying the
action's `x-payloadFingerprint` and `x-payloadShape`. It is the only way a
client that is not linked against the model's C++ can learn an action's shape.

The gate is `authorize(session, typeId, {})`, deliberately the same hook
`"instances"` uses: a description discloses field names, bounds, rules and the
payload fingerprint of every action on the type, so it must not be reachable by
a caller the server would not let execute — while still letting a deployer
refuse *describing* a type without refusing *using* it.

`SimulatedRemoteBackend::fetchActionSchemas(typeId)` is the client-side
accessor, built on the same synchronous `handleInline` path as
`registerModel`/`negotiateProtocolVersion` and opt-in in the same sense. See
[wire.md, "Serving action schemas"](wire.md#serving-action-schemas).

### `PayloadCompleteness` — enforcing the action-evolution policy

`ServerConfig::payloadCompleteness` chooses whether an `execute` body must
carry every field the action's served schema marks `required`
(`payloadCompleteness()` reads it back):

| Value | Behaviour |
|---|---|
| `Lenient` (default) | The action codec's lenient decode is the only gate, and a missing field is a default-constructed one. |
| `RequireDeclaredFields` | An `execute` whose `body` carries no key for a `required` field is refused with `err "payload missing required field(s): <names>"`, before the in-flight slot is reserved and before `Model::execute` runs. |

The check runs **after** authorization (its diagnostic names the action's own
field, which is exactly what `"schemas"` discloses and so is owed the same
gate) and **before** the in-flight reservation, so a payload that will not be
dispatched never consumes a slot. It costs one extra parse of `body`, which is
part of why it is not on by default; the deciding reason is that enabling it by
default would itself be the non-additive wire change the policy it enforces
forbids without a `kProtocolVersion` bump. Read the full derivation — including
what is *not* mechanically checkable — in
[wire.md, "Enforcing the policy"](wire.md#enforcing-the-policy).

### `LimitPolicy` — opt-in resource limits

`ServerConfig::limits` is an optional, connection-agnostic resource policy.
Every field defaults to `0` ("unbounded"), so an unconfigured server applies no
limit:

| Field | Default | Enforcement |
|---|---|---|
| `executeTimeout` | `0` (disabled) | A timer arms when an `execute` is admitted. If it fires first, the server replies `err "timeout"` and the eventual strand result (if the model finishes later) is discarded via a shared once-flag — `handle()`'s reply-exactly-once contract holds regardless of which path resolves first. An ordinary handler keeps running to completion on its strand; morph never interrupts it. A handler returning `core::async::Task` is also asked to stop, through its stop token, and unwinds at its next stop-aware `co_await` (see `docs/spec/core/coroutines.md`, "Execute deadlines"). |
| `maxLiveModels` | `0` (unbounded) | Checked on the server strand before `register` constructs a new instance, and again after the construction and before the insert (the construction is host code that may itself register through `handleInline`); over the cap → `err "too many models"`. Exact: nothing else inserts between the check and the insert. A shared `register`/`attach` that finds its key already live takes a reference and is not counted against the cap. |
| `maxInFlightExecutes` | `0` (unbounded) | The in-flight count, server-strand state, incremented when an `execute` is admitted and decremented (posted back to the strand) when its reply is sent — success, exception, or timeout, whichever resolves the call first; at the cap → `err "server busy"`, no dispatch. Exact, for the same reason. |

A server-side execute timeout surfaces to a caller as `morph::backend::TimeoutError`
(alongside `BackendChangedError`/`BridgeDestroyedError`/`DisconnectedError`) rather
than a generic `std::runtime_error`, on both `SimulatedRemoteBackend` and
`QtWebSocketBackend`.

The timer that enforces `executeTimeout` is
`morph::async::detail::TimeoutScheduler` (`include/morph/core/timeout_scheduler.hpp`),
one per `RemoteServer`, created by the constructor when `executeTimeout` is
positive — so a server that does not use the feature pays nothing — on a
private `exec::IoLoop`. Created once and never replaced, it is cancelled from
the model strand's finish without a lock. The class lives in
`morph::async::detail` rather than `morph::backend::detail` because `Bridge`
uses the same primitive for the *client*-side `setExecuteDeadline` — see
[`completion.md`](completion.md), "Client-side execute deadline".

### Connection scopes

`RemoteServer` can optionally attribute registered models to a
transport-assigned connection, so a transport can reclaim every model a
connection created when that connection goes away. `ConnectionId`
(`morph::backend::ConnectionId`) is a `std::uint64_t` alias; `0` is reserved
and means *unscoped* — the meaning the two-argument `handle()` and
`handleInline()` always have. Scoping is strictly opt-in; enabling it changes
nothing for a caller that never uses it.

- `openConnection()` returns a fresh non-zero `ConnectionId` at once and posts
  the creation of its empty scope to the server strand, ahead of any `handle()`
  the same thread makes next. Call once per accepted transport connection.
- The scoped `handle(msg, reply, cid)` overload attributes any `register` (or
  register-or-attach `attach`) decoded from `msg` to `cid`'s scope: the
  `ModelId` is recorded in a `cid → (ModelId → count)` map, server-strand state
  next to the instance directory, so scope membership can never desync from
  instance existence. The count lets one connection hold more than one
  reference to the same shared instance (e.g. two handlers on one connection
  attaching the same key) without either reference leaking the other's
  release.
- A `deregister` releases exactly the reference **the requesting connection**
  holds — decrementing `cid`'s own scope entry for that `ModelId`, using the
  `cid` the deregister call itself carries, never whichever connection
  happened to attach the instance last. A shared instance can have several
  owning connections at once; crediting the release to the wrong one would
  either strand a reference no one will ever decrement, or let one
  connection's deregister silently consume another's hold. A connection that
  holds no reference to the id — it never attached it, has already released
  every reference it had, or its scope is closed — releases nothing, and is
  answered `ok`, as an unknown id is. Every shipped client sends `deregister`
  fire-and-forget and reads no reply, and an `err` there would only let a
  caller tell a live id it does not hold from one that is gone. The same rule
  covers the instance an `attach` re-point gives up: it is released only if
  the requesting connection holds it. The unscoped path (`ConnectionId` `0`)
  records no references, so its `deregister` always releases one.
- `closeConnection(cid)` posts, to the server strand, the erasure of every model
  still recorded in `cid`'s scope (its directory record and the per-instance
  connection entry) exactly as the `deregister` path does, then drops the scope
  itself. Passing `0`, an unknown `cid`, or a `cid` already closed is a no-op —
  idempotent by construction.
- **`closeConnection` does not consult `IAuthorizer`.** It is server-side
  housekeeping triggered by the transport observing its own connection close,
  not a caller-attributed action — synthesising a `deregister` envelope
  instead would need a session/token to pass `authorizeInstance`, which an
  ownership-enforcing authorizer would rightly reject, and would require the
  transport to parse every `register` reply to learn which ids it owns. Only
  in-process transport code can reach `closeConnection`, and the transport is
  already inside the server's trust boundary (see security.md).
- Cleanup never races a running `execute`: admission copies the instance's
  `shared_ptr<IModelHolder>` into the model strand's task, so an in-flight
  action keeps the holder alive until its task completes; `closeConnection`
  only removes the registry's reference, preventing *new* lookups (see
  concurrency_and_lifetimes.md).
- A `register` that reaches the server strand *after* its scope was closed is
  refused with `err "connection closed"` and no instance is retained. The
  transport's disconnect callback can post `closeConnection` while a
  `register` the client sent just before dropping is still queued behind it.
  The scope is looked up, never default-created: recreating it would strand
  that model (and every later one on the dead id) in a scope nothing closes a
  second time — an unbounded leak that, with `LimitPolicy::maxLiveModels` set,
  wedges the server permanently at `err "too many models"`.
- `SimulatedRemoteBackend` keeps using the unscoped path (its "connection" is
  the process itself) — it is unaffected by connection scopes.

### Graceful shutdown (`beginShutdown()` / `drainedWithin()`)

`beginShutdown()` enters shutdown: every subsequent `register`, `attach` and
`execute` envelope is rejected with `err "server shutting down"` (checked once,
first, on the server strand — before authorization or registry lookups run);
`deregister` (and any other envelope kind) is still served so clients can tear
down cleanly during the drain window. It is posted, so an envelope handed to
`handle()` after it returned is refused, and one admitted before it — even one
the same client sent a moment earlier — runs to its reply. Idempotent, and
irreversible — there is no un-shutdown; a restarted service constructs a fresh
`RemoteServer`. It also flips `health()`'s `ready` to `false` and, if
`ServerConfig::healthHandler` is set, calls it with the post-shutdown snapshot,
on the server strand — the mechanism that lets an orchestrator stop routing to
a server that is draining.

`drainedWithin(deadline, replyExec)` returns a `Completion<bool>` settled on the
server strand and delivered on `replyExec`, the caller's owner: `true` once the in-flight count is zero — at once if it already is —
or `false` when `deadline` elapses first (a `deadline` of `0` answers from the
count as it stands). A waiter is server-strand state; its deadline is a timer
on a `TimeoutScheduler` the strand creates on first use, whose expiry is posted
back to the strand. Nothing blocks: a caller that wants to wait attaches to the
completion. "In-flight" is the count `LimitPolicy::maxInFlightExecutes` gates
and `health()`'s `inFlight` field reads: incremented when an `execute` is
admitted and decremented, on the strand, as its reply is sent on every
resolving path (`ok`, `err`, or a `LimitPolicy::executeTimeout` firing first).

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

- **`RemoteServer::replyUndecodable` — undecodable envelope (`logError`,
  prefixed `[dispatchMessage]`).** Without it, a client that swallows its own
  error (or is malformed precisely because it is confused) leaves no trace of
  a request that never dispatched at all. Logs the connection id, the exception text, the byte count, and a
  **truncated** (256-byte) prefix of the raw payload. The payload is the
  most useful field for diagnosing *why* a client sent something malformed —
  and the most likely to contain application data, hence truncated rather
  than logged in full; server logs should already be treated as
  operationally sensitive (they also carry exception text and, on other
  lines, connection ids), and this is not a general redaction mechanism.
- **`RemoteServer::dispatchEnvelope` — one line per successfully-decoded
  request (`logDebug`, prefixed `[dispatchMessage]`).** `dispatchEnvelope` is
  the one place every kind funnels through, on the server strand, so it is the
  natural spot: connection id, `kind`,
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
loop thread. Every verb, the `QWebSocket` signal slots and the reconnect timer
run on the socket's thread — a `Bridge` over it calls from its owner, which
for a Qt application is that thread — so `_connected`, `_nextCallId`, the
reconnect state and the pending tables (`_pending`, `_pendingControl`,
`_queuedRegistrations`, `_pendingDeregisters`) need no lock. Each site that
touches the tables asserts, in a debug build, that it runs on the socket's
thread. The reconnect handler is posted to the executor it was installed with,
never run from the `connected` slot.

**Every request is asynchronous.** Each one carries a fresh non-zero `callId`
from the one counter (`++_nextCallId`), and its reply settles a `Completion`
delivered on the executor the caller names. Nothing waits for a reply inside a
call: on the Qt thread waiting would mean a nested `QEventLoop`, which a WASM
main thread cannot spin.

- `bindModel` — the request's shape selects `register`, a shared `register`, or
  `attach` (see [Backends with a genuinely non-blocking
  path](#backends-with-a-genuinely-non-blocking-path)). Every shape carries
  `contextKey`: the server constructs the holder itself, and
  `RemoteServer::attachLogIfConfigured` returns **before** consulting its
  `LogProvider` when the envelope's `contextKey` is empty, so a dropped key
  leaves the instance unjournalled. `tests/qt/test_qt_websocket.cpp`, "a
  private registration carries contextKey to the server's log provider", pins
  it against a real `QtWebSocketServer` and asserts on the provider. A private
  bind made before the socket has finished connecting is queued
  (`_queuedRegistrations`) rather than failed, and flushed — each entry
  assigned a call-id and sent, in FIFO order — from the `connected` slot, the
  first connect included, before the reconnect handler is posted. If the
  backend is destroyed (or the socket disconnects) before the queue is flushed,
  `cancelPending` drains it and rejects each entry's `Completion` exactly once.
  A keyed bind on a disconnected socket rejects at once.
- `promoteModel` — sends `assign`; the documented no-op cases resolve without
  sending.
- `instances(typeId, cbExec)` — sends `instances`; the reply's body is the
  key list. `BridgeHandler::instances()` reaches it through
  `Bridge::instancesOf`.
- `negotiateProtocolVersion(replyExec)` — sends `hello` and classifies the
  reply (see [Protocol negotiation](#protocol-negotiation)).
- `registerModel` (and so `registerModelWithContext`, which `IBackend` forwards
  to it), `assignPrimary`, `listInstances` — the synchronous `IBackend` verbs
  that would have to wait for a reply — throw `std::logic_error` naming their
  completion form.
- `deregisterModel` — **fire-and-forget**: if `_connected`, it sends a
  `deregister` envelope and returns without waiting for the ack; if
  disconnected, it does nothing. Its `callId` is filed in `_pendingDeregisters`
  only so the reply is recognised and dropped. An undelivered or lost
  `deregister` does not leak the model indefinitely when the server side is a
  `QtWebSocketServer`: its connection scope reclaims every model this client
  registered at the next disconnect (see "Connection scopes" and Limitations).
- `execute` — if not connected, resolves the returned `Completion` immediately
  with `DisconnectedError`. Otherwise it records the completion state +
  `deserializeResult` + `cbExec` in `_pending[callId]`, serialises the action,
  and sends the `execute` envelope.

**Reply routing.** `onTextMessage` decodes each incoming frame and routes it by
`callId`. `_pending` is checked first (an `execute` reply): `ok` →
`deserialize(body)` into the completion's value (deserialisation exceptions
become the completion's error), any other kind → `std::runtime_error(message)`.
`_pendingControl` is checked next (every other request — same `callId`
namespace, separate map because each settles its own value type): the entry is
removed from the map, then its reply function settles the caller's
`Completion`, since settling may run a continuation that re-enters the
backend. `_pendingDeregisters` last, whose replies are discarded. A `callId`
matching none of them — a late reply for an already-cancelled call, or `0` —
is dropped. A frame that fails to decode fails every pending request
(`cancelPending` with a protocol error): its `callId` is unreadable and the
peer's framing can no longer be trusted.

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
  `initialReconnectDelay`, releases a parked `waitForConnected()`. Fires `_connectHandler`
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

**`waitForConnected(timeoutMs = 5000)`** pumps a local `QEventLoop` until the
socket connects or the timeout elapses; returns the current `_connected` flag.
A desktop or test convenience, called on the Qt thread; a WASM main thread
cannot spin it, and binds without waiting instead (a private bind made before
the connect is queued) or reacts to `setConnectHandler`.

<a id="protocol-negotiation"></a>**`negotiateProtocolVersion(replyExec)`** sends
a `"hello"` and returns a `Completion` the reply settles, classified via
`wire::interpretHelloReply`. Opt-in: intended to be sent once, after the
socket connects and before any `bindModel`/`execute`, but nothing enforces that
ordering and nothing sends it automatically. Rejects with `std::runtime_error`
if the server explicitly rejects the version or the socket is not connected,
and with `DisconnectedError` if it drops before the reply. See
[wire.md](wire.md#protocol-version-negotiation). The reply also tells the
backend whether the server honours `"cancel"`; see
[Cancelling a remote call on the server](#cancelling-a-remote-call-on-the-server).

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
fast on every existing connection), then asks `drainedWithin(deadline)` for an
answer delivered on its own thread through a `QtExecutor`, and pumps the Qt
event loop until it answers — so the reply callbacks
`onTextMessage` already queued via `QMetaObject::invokeMethod` actually run
while it waits. Because `drainedWithin()`'s in-flight count can reach zero a
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
| `messagesPerSecond` | `0` (unbounded) | A per-connection token bucket (capacity = `messagesPerSecond`, refilled continuously). A frame that finds an empty bucket is refused — it never reaches `RemoteServer` — and answered with an `err "rate limited"` addressed to that frame's own `callId`. Not queued, and the connection is not closed. |
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
and the core-cpp modules `morph` already links: the HTTP/1.1 Upgrade handshake
(`Sec-WebSocket-Key`/`Sec-WebSocket-Accept`, via a hand-rolled SHA-1 and
core-cpp's `core::base64::encode`) and the masked/unmasked text-frame codec are
implemented in `include/morph/net/detail/` (`sha1.hpp`, `ws_handshake.hpp`,
`ws_frame.hpp`, `tcp_socket.hpp`), and the sockets, the listener and the
timers underneath are core-cpp's `core::net` event loop's. Because both
transports round-trip the same
`wire::Envelope`, a `SocketBackend` client and a `QtWebSocketServer`
interoperate (and vice versa) with no protocol changes on either side.

**Reconnect handlers are posted to the executor they were installed with.**
`SocketBackend` never runs the handler installed by `setReconnectHandler` on
the I/O loop: when a connection after the first completes, the loop posts the
handler to the executor passed with it — the bridge's owner, for `Bridge`'s
handler. The handler re-registers through `bindModel`, whose replies this same
loop delivers, so nothing on the reconnect path waits for the loop.

### The structural registration surface, natively

`SocketBackend` overrides `bindModel`/`promoteModel` itself rather than being
wrapped in [`SynchronousBackendAdapter`](#synchronousbackendadapter--a-blocking-backend-behind-a-strand).

1. **The transport already has the machinery.** The I/O loop demultiplexes
   replies by `callId` for `execute`, and `RemoteServer` echoes `callId` on
   every control reply it sends — `register`, `registerShared`, `attach` and
   `assign` all answer with `makeOk(env.callId, {}, mid)`. A control call is
   therefore the same shape as an execute, and the native path needs no
   protocol change, no server change and no new thread. It is a second
   `PendingCallTable`, sharing the execute table's `callId` counter so an id
   can never be ambiguous between the two.
2. **A bind cannot block anything.** `bindModel` posts its frame to the loop
   and returns before the reply exists; the loop settles the `Completion` when
   the reply arrives, and the continuation runs on the caller's executor. There
   is no wait for any thread to block — the loop's own included — whichever
   thread issues the call.

The synchronous control verbs (`registerModel`, `registerModelWithContext`,
`assignPrimary`, `listInstances`) post the same request and wait on a future
for its reply, so several may be in flight at once, each matched by `callId`;
on the loop's own thread they throw instead of waiting on themselves.
`cancelPending` sweeps both tables, so a disconnect rejects an in-flight bind
with `DisconnectedError` and a waiting synchronous verb with `"<verb> failed:
disconnected"`.

`tests/net/test_socket_backend.cpp` pins it: several binds in flight at once,
matched by `callId`, with the replies delivered back to front; and a reconnect
handler's body runs as a task of the executor it was installed with, never on
the loop.

**Threading — one I/O loop per process.** Neither `SocketBackend` nor
`SocketServer` owns a thread for its I/O. Every socket, every connection,
every pending call, the reconnect backoff and the accept flow live on an
`morph::exec::IoLoop` (`include/morph/core/io_loop.hpp`): one
`core::net::PlatformLoop` and, natively, the one thread that runs it. An
application constructs one `IoLoop` and passes it to every component built on
it — `SocketBackend(loop, url)`, `SocketServer(loop, server, port)`,
`TimeoutScheduler(loop)`, `NetworkMonitor(loop, …)` — the way `LocalBackend`
takes its pool. The loop is injected rather than a process-wide singleton the
framework starts on first use: the dependency is then visible in every
constructor, there is no global to reset between tests, and an application
decides how many loops it runs (one, normally). Each component also keeps its
old constructor, which builds a private `IoLoop` — one loop, and one thread,
for a caller with no loop to share.

Each component's state is touched only in tasks its loop runs. Its public verbs
post to the loop and return; the ones that must answer do so from an atomic
(`connected`, `port`) or an id allocated before the post (`TimeoutScheduler`'s
handle). Its destructor runs its close on the loop — inline when it runs on
the loop's own thread, posted and waited for otherwise — so it never waits on
itself.

| Component | Cross-thread surface | Everything else |
|---|---|---|
| `SocketBackend` | `execute`, `bindModel`, `promoteModel`, `deregisterModel`, `cancelPending` post; `waitForConnected` posts a waiter and blocks the caller; the synchronous control verbs post their frame and wait on a future for the reply; `setSession`, `setReconnectHandler` post | socket, both pending tables, call-id counter, session, reconnect handler, reconnect delay and backoff timer, connect waiters: loop only |
| `SocketServer` | `listen()`, `close()` run on the loop and wait; `port()` is atomic; a `RemoteServer` reply is posted from the replying thread | listener, connections, writers: loop only |

The flows are `core::async::Task`s spawned on the loop. `SocketBackend` runs
one attempt flow per connection attempt — dial through `core::net::connect`
(its budget is `connectTimeout`; a literal address never leaves the loop, a
hostname is resolved on core-cpp's resolver pool), the RFC 6455 handshake,
then the read loop — and, when the connection ends, arms a loop timer for the
backoff before the next attempt. Each connection has one writer flow that
drains its queued frames one 64 KiB chunk at a time; a loop timer armed around
each chunk's write (`sendTimeout`) closes the socket if the write makes no
progress, which ends the connection like any other disconnect.

`morph::net` keeps its own WebSocket framing (`detail/ws_frame.hpp`) and
handshake text (`detail/ws_handshake.hpp`); only the socket underneath is
core-cpp's. `detail/ws_connection.hpp` holds the loop-side pieces both roles
share: the connection with its outgoing queue, the writer flow, and the
handshake header read.

Because the loop is shared, a callback that runs on it — a
`TimeoutScheduler` callback, a `NetworkMonitor` probe or callback, a
completion delivered on `inlineExecutor()` — must not block. The synchronous
control verbs throw when called on the loop's thread (the reply they would
wait for is read by that thread), so that mistake fails rather than hangs.

Blocking calls like `SocketBackend::waitForConnected()`/the synchronous
`registerModel` genuinely block the calling thread with no event-loop pumping
of their own — fine against a `SocketServer` peer, but calling them directly
from the one thread that owns a `QCoreApplication` a *`QtWebSocketServer`*
peer depends on would starve that peer's own accept/handshake machinery. See
`tests/net_qt_interop/test_net_qt_interop.cpp`'s `waitForConnectedPumpingQt`/
`makeHandlerPumpingQt` helpers for the pattern such a caller needs (poll with
a short timeout while pumping `QCoreApplication::processEvents()`).

**`SocketBackend` — client-side `IBackend`.** Implements every `IBackend`
method with the same observable semantics as `QtWebSocketBackend`:
`registerModel` is synchronous (parks on a condition variable instead of a
nested event loop; a register whose reply never arrives unblocks with
`"register failed: disconnected"` rather than hanging); `deregisterModel` is fire-and-forget
(same trade-off; an undelivered or lost `deregister` against a `SocketServer`
peer does not leak the model, because `SocketServer` participates in
`RemoteServer`'s connection-scope contract exactly as `QtWebSocketServer`
does — see below); `execute` serialises the action on the calling thread and
posts the envelope; the loop assigns a monotonic `callId`, files the pending
record and writes the frame, and the reply flow finds the record and settles
the completion. `setSession` stores the session under its own mutex and every
subsequently built `register`/`registerShared`/`attach`/`assign`/`deregister`
envelope's `session` field is set from it before sending — see
[Session propagation to control envelopes](#session-propagation-to-control-envelopes).
Reconnect is configured by `SocketBackendConfig` (aliased
`SocketBackend::Config`), with the same four fields and defaults as
`QtWebSocketBackendConfig` (`reconnectEnabled`, `initialReconnectDelay`,
`maxReconnectDelay`, `backoffMultiplier`) plus `connectTimeout` (default 5 s,
the whole dial), `sendTimeout` (default 30 s, one chunk's write) and
`handshakeTimeout` (default 10 s, the whole handshake response read, from its
first byte to the header's end). `waitForConnected(timeout = 5000ms)` posts a
waiter the loop releases when a connection completes, and blocks the calling
thread on it until then or the timeout — the non-Qt equivalent of pumping the
Qt event loop. The constructor takes a `ws://` URL string (`wss://` throws
immediately, before any loop is started — see Limitations) and posts the first
connection attempt.

**`SocketServer` — server-side transport.** Fronts a `RemoteServer` by
reference — the same non-owning-reference lifetime rule as `QtWebSocketServer`
applies (see Lifetime & ownership). `listen()` binds `127.0.0.1:port` (`0`
lets the OS assign a free port, exactly like the Qt transport) with
`SO_REUSEADDR`, hands the listener to the loop (`core::net::adoptListener`)
and spawns the accept flow; each accepted connection gets a flow that performs
the server-side handshake (bounded as a whole by `handshakeTimeout`), then
reads framed text messages and calls the **scoped**
`RemoteServer::handle(msg, reply, cid)`. A server built with the loop-less
constructor creates its loop on the first `listen()`; a process out of
descriptors gets `false` rather than a throw.

**Connection scope.** `SocketServer` opts every client into `RemoteServer`'s
connection scope end to end, matching `QtWebSocketServer`:

- **Accept** mints a `ConnectionId` via `_server.openConnection()` and stores
  it on the per-connection state.
- **Dispatch** passes that id to the three-argument `handle()`, so every
  `register` on the connection is attributed to its scope.
- **Teardown** calls `closeConnection(cid)` once per connection, however it
  ends — failed handshake, peer close, read error, a stalled write, or
  `close()`, which does it for every connection still open before it returns.

The `reply` callback runs on a `RemoteServer` worker-pool thread. It encodes
the frame there and posts it to the loop through a weak handle
(`IoLoop::weak()`), so a reply that outlives the loop is dropped rather than
posted to freed memory; on the loop, a connection that closed meanwhile drops
it, the same behaviour `QtWebSocketServer`'s `QPointer` gives. `close()` (also
run by the destructor) is idempotent, and so is concurrent use of it on a live
object: each call is one loop task, and the loop runs them one at a time. It
closes the listener — the accept flow resumes with `Cancelled` on a later turn
and ends — and every connection's socket, which resumes each connection's
parked read the same way. `port()` reads `0` afterwards, and the port is free
to rebind, matching `QtWebSocketServer`.

**A failed accept is retried or ends the flow, by whether it can recur.**
Exhaustion (`EMFILE`, `ENFILE`, `ENOBUFS`, `ENOMEM`) and any other failure the
next attempt may not repeat waits 50 ms and accepts again, so a process out of
descriptors keeps its listener and recovers when descriptors are freed. A
listener that can no longer accept at all ends the flow instead: one that has
stopped listening (`EINVAL`), a closed or foreign descriptor
(`EBADF`/`ENOTSOCK`) or a non-stream socket (`EOPNOTSUPP`) fails every attempt
the same way. core-cpp reports these as `BadHandle`/`Unsupported`, or, in
releases inside the supported range that predate that classification, as
`SystemError` carrying the `errno`; both are recognised. The server then drops
the listener, so `port()` reads `0` and a later `listen()` binds afresh.
Connections already accepted are untouched.

**The listener's non-blocking mode stops at the listener.**
`TcpSocket`'s fd-adopting constructor clears `O_NONBLOCK` on every descriptor it
takes ownership of, so a connection from `accept()` or `tryAccept()` is always
blocking, whatever mode the listener it came from is in. The transports no
longer read through `TcpSocket` — the loop's sockets are non-blocking by
design — but the tests and any blocking caller do: macOS/BSD propagate a
listening socket's `O_NONBLOCK` onto the sockets `accept(2)` returns (POSIX
permits this; Linux does not do it), and a blocking reader of such a socket
fails on `EAGAIN` before its peer has written.

## Cancelling a remote call on the server

`SocketBackend` and `QtWebSocketBackend` tell the server when they abandon an
`execute`, by sending `cancel {cancelCallId}` (see
[wire.md](wire.md#cancelling-a-call)), so a server-side Task handler stops
instead of running on for a reply nobody reads. Each sends one only to a
server that advertised `"cancel"` in its `"hello"` reply, so each sends none
until the application has called `negotiateProtocolVersion` — opt-in, like
the handshake itself. Once called, the backend re-sends `"hello"` after every
reconnect and forgets the capability on every disconnect: a new connection may
reach a different server.

A call is cancelled on the server when:

- **Its stop is requested** — an execute deadline (`Bridge::setExecuteDeadline`)
  or any other holder of the call's `ActionCall::stopSource`. The backend
  registers a stop callback on that source once the call has its `callId` and
  its frame is queued. The callback runs on whichever thread requested the
  stop, so it touches no table: `SocketBackend`'s posts `requestCancel(callId)`
  to the I/O loop; `QtWebSocketBackend`'s sets the call's flag and starts a
  zero-interval single-shot wake timer through the member-name
  `QMetaObject::invokeMethod(&timer, "start", Qt::QueuedConnection)` — which
  allocates nothing, unlike the functor overload — and the timer's drain, on
  the Qt thread, sends a cancel for each flagged call. Either way the owner
  sends the cancel only if the call is still waiting for its reply, and the
  pending table stays the owner's alone, with no lock. A stop after the reply sends
  nothing: the callback is deregistered with the pending entry. A call with no
  stop source — `Bridge` gives one only to a call whose handler is a Task,
  the one kind a server can stop — never registers one.
- **`cancelPending` sweeps it** — `~Bridge` and `Bridge::switchBackend` call it.
  Every drained execute is cancelled, with or without a stop source: in a
  client-only build `Bridge` cannot tell a Task handler from an ordinary one,
  and the server answers a cancel for an unstoppable call with the same
  no-op `ok`.

The cancel carries the session the `execute` carried (kept with the pending
entry for a stoppable call; the backend's control session otherwise), since
the server honours it only under the execute's verified principal. It is sent
fire-and-forget under a fresh `callId` filed nowhere, so its `ok` is dropped
by the reply router.

**Ordering, and why `SocketBackend::cancelPending` being posted is acceptable.**
`SocketBackend::cancelPending` is posted to the I/O loop (**G0** on return), so
the cancels it sends sit behind whatever the loop already had queued. That is
acceptable, for three reasons. The cancel can never overtake the execute it
names: the execute was posted to the same loop before it, the loop runs its
tasks in order and writes one connection's frames in order, and the server
handles one connection's messages in order, so the execute is always admitted
before its cancel is read. What waits is only the server's handler, which
runs on for at most as long as the loop takes to drain its queue — work that
never blocks — and which already ran to its end before cancels existed. And
the caller's own guarantee does not depend on it: the calls are settled with
the cancel's exception in the same loop task, before any reply to them can be
delivered. A stop-triggered cancel is posted in the same way, from the stopping
thread, and is ordered the same.

**Teardown.** `~Bridge` calls `cancelPending` and then destroys the backend,
so the cancels are queued just before the backend closes its socket. Each
backend's close therefore lets queued frames out first: `SocketBackend`'s
close is a close-after-flush on the loop (the write still bounded by
`sendTimeout`), and `QtWebSocketBackend`'s destructor flushes its socket,
without blocking, before the abort. A close that discarded queued frames
would drop exactly these cancels; the `~Bridge` tests in both suites fail
when it does (measured on both backends). `QtWebSocketBackend` sends nothing from its own
destructor's sweep — its socket is already aborted there — so a backend
destroyed without its owner's `cancelPending` cancels nothing.

### Where the remote backends' cancel rows are measured

The policy table in
[concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md#every-cancel-verb-measured)
gives the remote backends' rows. Their *work* column — a Task handler on a
server that advertised `"cancel"` is asked to stop — is measured over a real
loopback, through `~Bridge` and through the execute deadline, in
`tests/net/test_socket_backend.cpp` and `tests/qt/test_qt_websocket.cpp`
(`[cancel]`). Their G-levels are read, not measured.

## Lifetime & ownership

The backends hold *references*, not owning pointers, to the resources they run
on. Getting the destruction order wrong is a use-after-free, so the invariants
are:

- **The worker pool must outlive the backend.** Every backend takes an
  `IExecutor& workerPool` by reference (`LocalBackend`, `RemoteServer`) and wraps
  runs its strands on it. The pool (typically a `ThreadPoolExecutor`) must be
  destroyed *after* the backend that references it — and, in practice, after the
  `Bridge` that owns the backend. Destroying the pool first leaves the strand
  pointing at freed storage.
- **`RemoteServer` must be created via `std::make_shared`.** It derives from
  `std::enable_shared_from_this<RemoteServer>`; every public verb but
  `openConnection`'s id and the two accessors captures `shared_from_this()`
  into the task it posts. Constructing it on the stack and calling one throws
  `std::bad_weak_ptr` (see ARCHITECTURE.md "RemoteServer must be
  heap-allocated").
- **`~RemoteServer` closes the server strand first.** Every task on it holds
  the server, so when the destructor runs none is queued; closing waits for a
  task still running on another thread (and returns at once from inside the
  server's own last task), before any member the strand's tasks touch goes.
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
- **The `exec::IoLoop` must outlive every component built on it.**
  `SocketBackend`, `SocketServer`, `TimeoutScheduler` and `NetworkMonitor`
  borrow the loop they were given, and each destructor runs its close on it
  and waits: a loop destroyed first would never run that close. Destroy the
  components, then the loop. A component built with its loop-less constructor
  owns a private loop and destroys it last, after its own close.
- **Pending strand tasks capture shared_ptr copies, so model destruction
  mid-flight is safe.** Both backends' `execute` strand tasks capture the model
  `holder` by `shared_ptr` copy (and `RemoteServer`'s also captures the reply
  callback and the moved `Envelope`). A `deregisterModel` that erases the map
  entry while a task is queued or running only drops the *map's* reference; the
  in-flight task holds its own, so the holder stays alive until the task
  completes. `RemoteServer`'s tasks additionally keep the server itself
  alive via `shared_from_this()`. `closeConnection` erases the same map entries
  as an explicit `deregister`, so the same guarantee covers it: it never races a
  running `execute` into use-after-free, only prevents *new* lookups.
- **A backend must outlive every call into it — destroying one while a thread
  is parked inside it is undefined.** This is the same rule the destruction
  ordering table in
  [concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md#destruction-ordering--who-must-outlive-whom)
  states for a call on a handler whose `Bridge` is gone; no backend is exempt
  from it. It is worth spelling out for `morph::net::SocketBackend`, because
  its blocking calls are made from threads other than the loop that owns its
  state: `waitForConnected()` blocks on a future and then reads the backend's
  connected flag, and `~SocketBackend` does not wait for a parked waiter, so
  destroying the backend from another thread frees the storage that read
  touches — a data race, whatever the waiter's remaining timeout. A caller
  that wants to abandon a wait must bound it with the `timeout` argument and
  let it return before destruction begins; there is no cancel.

## Failure modes

| Situation | Local (`LocalBackend`) | Remote (`RemoteServer` / `SimulatedRemoteBackend`) |
|---|---|---|
| `register` with an unregistered `typeId` | N/A — the local factory closure constructs the instance directly; there is no registry lookup and no type-id failure. | `ModelRegistryFactory::create(typeId)` fails → the catch in `dispatchEnvelope` replies `err "unknown model type: <typeId>"`. Remote registration therefore requires the model to have been macro-registered with `BRIDGE_REGISTER_MODEL`. `SimulatedRemoteBackend::registerModelWithContext` turns that `err` into a thrown `std::runtime_error("register failed: unknown model type: <typeId>")`. |
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
| `register` reply is `err` (e.g. unknown model type) | `bindModel`'s `Completion` rejects with `std::runtime_error(message)`. |
| Malformed reply frame | Every pending request is rejected with a protocol error: the `callId` is unreadable, and the peer's framing can no longer be trusted. |

`morph::net::SocketBackend` gives the same guarantees over its own transport
(its synchronous verbs wait on a future, off the loop's thread):

| Situation | `SocketBackend` |
|---|---|
| `execute` while the socket is disconnected | Completion resolves immediately with `DisconnectedError`. |
| Socket drops with execute calls in flight | The loop's disconnect handling sweeps both pending tables, resolving every pending completion with `DisconnectedError`. |
| Reply arrives for an unknown/cancelled `callId` | Dropped silently. |
| `register` reply is `err` (e.g. unknown model type) | `registerModel` throws `std::runtime_error("register failed: " + message)`. |
| `register` reply never arrives (never connected, or disconnects mid-call) | The posted request is rejected with `DisconnectedError` when the connection drops or the backend closes; the waiting verb rethrows it as `std::runtime_error("register failed: disconnected")` — never hangs. Called on the loop's own thread, or after the loop has stopped, it throws at once instead of waiting. |

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
| `localOp` | The **model strand** (`LocalBackend` only) — posted on the per-`ModelId` strand, serialised against other actions for the same model. Never invoked on the remote path. |

On the server side, `handle()`'s calling thread — the transport's — decodes
the envelope; `RemoteServer` then runs authorize/authenticate and the model
lookup on the server strand, and `ActionDispatcher::dispatch` (and the
`ScopedContext`) on the model's strand. Both strands run on the server's pool
(see [Per-model execute ordering](#per-model-execute-ordering)).
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
runs on the server strand / model strand as above), and marshals the reply *back*
onto the Qt thread before `sendTextMessage`.

`morph::net::SocketBackend` splits the same callables between the caller and
its I/O loop instead of the Qt thread:

| Callable | Runs on (`SocketBackend`) |
|---|---|
| `serializeAction` | The **calling thread** — `execute` invokes it while building the envelope, before posting it to the loop. |
| `deserializeResult` | The **I/O loop's thread** — invoked when the matching reply frame arrives. |
| `localOp` | Never invoked (no local models). |

Unlike `QtWebSocketBackend`, `SocketBackend`'s `execute`/`registerModel`/
`deregisterModel` may be called from any thread, because each posts to the
loop that owns the state (the synchronous verbs excepted on the loop's own
thread, where they throw). `bindModel`/`promoteModel` are callable from any
thread, the loop's included: they park on nothing, and their continuations run
on the caller's `cbExec` rather than on the loop that settles them.
`morph::net::SocketServer` receives frames on the loop, hands them to
`RemoteServer::handle` (server strand / model strand, as above), and the reply,
produced on whichever thread finishes the work, is posted back to the loop,
which queues it on the connection's writer. `SocketServer::close()` is callable
from any thread, including concurrently with another `close()` on the same live
server: each call is a loop task, so the loop serialises them.

## API reference

### `detail::ActionCall`

| Member | Type | Notes |
|---|---|---|
| `modelTypeId` | `std::string_view` | Target model type id. Referent must outlive the dispatch. |
| `actionTypeId` | `std::string_view` | Target action type id. Referent must outlive the dispatch. |
| `action` | `std::shared_ptr<void>` | Owns the action object; may be null when the callables ignore it. |
| `serializeAction` | `std::string (*)(const void*)` | JSON serialiser; remote path only. Borrows `action`. |
| `deserializeResult` | `std::shared_ptr<void> (*)(std::string_view)` | JSON deserialiser; remote path only. Does not read `action`. |
| `localOp` | `std::shared_ptr<void> (*)(IModelHolder&, void*)` | Direct execution; local path only. Borrows `action`. |
| `session` | `morph::session::Context` | Session context for the call. |
| `serializeBody()` | `std::string serializeBody() const` | Calls `serializeAction(action.get())`; throws `std::runtime_error` if the pointer is null. |

### `detail::IBackend`

| Method | Signature | Notes |
|---|---|---|
| `registerModel` | `virtual ModelId registerModel(const string&, function<unique_ptr<IModelHolder>()>)` | Pure virtual. |
| `registerModelWithContext` | `virtual ModelId registerModelWithContext(const string&, function<unique_ptr<IModelHolder>()>, string_view)` | Default: drops `contextKey`, calls `registerModel`. |
| `bindModel` | `virtual Completion<ModelId> bindModel(BindRequest, IExecutor& cbExec)` | Default: binds a private instance through `registerModelWithContext` for every shape (no shared directory), releases a non-zero `current` after acquiring, and settles before returning. See [The structural registration surface](#the-structural-registration-surface--bindmodel-and-promotemodel). |
| `promoteModel` | `virtual Completion<ModelId> promoteModel(PromoteRequest, IExecutor& cbExec)` | Default: calls `assignPrimary` inline and settles with `request.mid`. |
| `instances` | `virtual Completion<vector<string>> instances(const string&, IExecutor& cbExec)` | Default: calls `listInstances` inline and settles with its answer (a throw rejects). `QtWebSocketBackend` overrides it with an `instances` request. |
| `setOwner` | `virtual void setOwner(const exec::detail::OwnerAffinity&)` | Default: ignored. Called by `Bridge` when it installs the backend. |
| `deregisterModel` | `virtual void deregisterModel(ModelId)` | Pure virtual. |
| `execute` | `virtual Completion<shared_ptr<void>> execute(ModelId, ActionCall, IExecutor*)` | Pure virtual. |
| `notifyBackendChanged` | `virtual void notifyBackendChanged()` | Pure virtual. |
| `cancelPending` | `virtual void cancelPending(const exception_ptr&)` | Pure virtual. |
| `setReconnectHandler` | `virtual void setReconnectHandler(function<void()>, IExecutor*)` | Default: no-op. The handler is posted to the executor after the second and later connects, never run on the backend's thread. Both null clears. |
| `setConnectHandler` | `virtual void setConnectHandler(const function<void()>&)` | Default: no-op. Fires on every successful connect, first included. |
| `setDisconnectHandler` | `virtual void setDisconnectHandler(const function<void()>&)` | Default: no-op. Fires whenever the transport drops, before any reconnect is scheduled. |
| `setSession` | `virtual void setSession(session::Context)` | Default: no-op. Stamped onto every control envelope (`register`/`registerShared`/`attach`/`assign`/`deregister`) subsequently built. See [Session propagation to control envelopes](#session-propagation-to-control-envelopes). |

### `detail::BindRequest` / `detail::PromoteRequest`

| Field | Type | Meaning |
|---|---|---|
| `BindRequest::typeId` | `std::string` | String type-id of the model. |
| `BindRequest::factory` | `std::function<unique_ptr<IModelHolder>()>` | Constructs the holder. Local path only; not called on an attach to a live shared instance. |
| `BindRequest::contextKey` | `std::string` | Entity key for the action log; empty if none. Owned, not a view. |
| `BindRequest::primary` | `std::string` | Canonical primary key; empty means a private instance. Owned, not a view. |
| `BindRequest::current` | `ModelId` | Instance currently held; non-zero makes the bind a re-point. |
| `PromoteRequest::mid` | `ModelId` | Live instance to promote. |
| `PromoteRequest::typeId` | `std::string` | Model type id — the directory's first key component. |
| `PromoteRequest::primary` | `std::string` | Key to file `mid` under. |


### `SynchronousBackendAdapter`

| Method | Notes |
|---|---|
| `SynchronousBackendAdapter(shared_ptr<IBackend> inner, IExecutor& blockingExec)` | Throws `std::invalid_argument` if `inner` is null. `blockingExec` is `MORPH_LIFETIMEBOUND` and must keep running tasks until the destructor's wait completes. |
| `wrapped()` | The wrapped backend; never null. |
| `bindModel(request, cbExec)` | Posts `inner->bindModel(request, inlineExecutor())` onto the control strand; settles the returned `Completion` on `cbExec` from there. Never blocks the caller. |
| `promoteModel(request, cbExec)` | Posts `inner->promoteModel(request, inlineExecutor())` onto the control strand; settles on `cbExec`. |
| `cancelPending(exc)` | On the caller's owner: sets each still-unsettled `bindModel`/`promoteModel` record's `cancelled` flag and rejects its promise with `exc`, **then** posts `inner->cancelPending(exc)` to the strand. Not a plain forward: those promises are settled from `_control` tasks the wrapped backend has never heard of. The flag is what stops a task still *queued* on `_control` from making its control call after the caller was told the bind was cancelled; a task already inside that call is unaffected. |
| `setOwner(affinity)` | Records the caller's owner; not forwarded (the wrapped backend's owner is the control strand). |
| every other `IBackend` verb | Run on the control strand. The synchronous ones (`registerModel`, `registerModelWithContext`, `assignPrimary`, `listInstances`) wait for it, so they must not be called from the executor's only thread. |

### Error types

| Type | Base | Message |
|---|---|---|
| `BackendChangedError` | `std::runtime_error` | `"backend changed before completion resolved"` |
| `BridgeDestroyedError` | `std::runtime_error` | `"bridge destroyed before completion resolved"` |
| `DisconnectedError` | `std::runtime_error` | `"transport disconnected before completion resolved"` |
| `TimeoutError` | `std::runtime_error` | `"execute timed out on the server"` |
| `ClientTimeoutError` | `std::runtime_error` | `"execute timed out waiting for any reply"` |

### `LocalBackend`

| Method | Notes |
|---|---|
| `explicit LocalBackend(IExecutor& workerPool)` | Constructs with a strand around `workerPool`. |
| `registerModel(typeId, factory)` | Atomically increments `_nextId`, files the holder as a private instance in `_instances`, on the owner; also records the id in `_changeAware` when the holder is backend-change-aware. `typeId` is accepted for interface compatibility but not used. |
| `deregisterModel(mid)` | Releases one attachment through `_instances`, on the owner; erases from `_changeAware` when that destroys the instance. |
| `notifyBackendChanged()` | Looks up the models recorded in `_changeAware`, on the owner, then posts `onBackendChanged()` (the `IModelHolder` base virtual — no `dynamic_cast`) onto each such model's strand. Cost is O(change-aware models). |
| `execute(mid, call, cbExec)` | Posts `call.localOp` on the model's strand with `ScopedContext`. Returns a `Completion`. |
| `cancelPending(exc)` | Snapshots `_pending`, delivers `exc` to each live state, and re-arms the compaction threshold. |
| `trackedPendingCount()` | `[[nodiscard]] std::size_t trackedPendingCount() const` — size of `_pending`, on the owner. **Not** the in-flight count: between sweeps the list also holds entries whose state is already destroyed. An upper bound on in-flight, and the observable that makes [the compaction policy](#the-pending-list-and-its-amortised-compaction)'s memory cost measurable. For in-flight *calls*, use `Bridge::pendingCalls()`. |

### `RemoteServer`

| Method | Notes |
|---|---|
| `RemoteServer(workerPool, dispatcher, registry)` | Allow-all authorizer, default `ServerConfig`. |
| `RemoteServer(workerPool, authorizer, dispatcher, registry)` | Custom authorizer; null → allow-all. Default `ServerConfig`. |
| `RemoteServer(workerPool, config, dispatcher, registry)` | Allow-all authorizer, `config` fixed for the server's life. Throws `std::invalid_argument` if `config.minProtocolVersion > config.maxProtocolVersion`. Calls `config.healthHandler` once before returning. |
| `RemoteServer(workerPool, authorizer, config, dispatcher, registry)` | Both. |
| `handle(msg, reply)` | Async: decodes on the calling thread, posts to the server strand, calls `reply` once from a pool thread. Unscoped (`cid == 0`). Callable from any thread; calls from one thread are handled in call order — see [Per-model execute ordering](#per-model-execute-ordering). |
| `handle(msg, reply, cid)` | Like `handle(msg, reply)`, additionally attributing any `register` in `msg` to connection `cid`'s scope. `cid == 0` behaves exactly like the two-argument overload. |
| `handleInline(msg)` | Sync: runs the control envelope on the server strand — posted and waited for, or inline when already on it — and returns the reply JSON. **Rejects `execute`** — returns an `err` reply without dispatching, because an `execute` reply is produced asynchronously after this call returns. Needs a pool thread other than the caller's free to run the strand. Unscoped. |
| `openConnection()` | Returns a fresh non-zero `ConnectionId` at once; its empty scope is opened on the server strand, before the caller's next `handle()`. |
| `closeConnection(cid)` | Posts: erases every model still recorded in `cid`'s scope (as `deregister` would) and drops the scope. `cid == 0`, unknown, or already-closed is a no-op — idempotent. Bypasses `IAuthorizer` by design. |
| `payloadCompleteness()` | The `ServerConfig::payloadCompleteness` given at construction. |
| `strand()` | The server strand, as an `exec::IExecutor`: `runningOn(server.strand())` is true inside every task on the server's state. |
| `health(replyExec)` | `[[nodiscard]] Completion<HealthStatus> health(IExecutor& replyExec)` — readiness/liveModels/inFlight, answered on the server strand; delivered on `replyExec`, the executor the caller attaches from (borrowed: it must outlive the delivery). See [observability.md](observability.md). |
| `beginShutdown()` | Posts: subsequent `register`/`attach`/`execute` envelopes get `err "server shutting down"`; `deregister` still served. A client therefore cannot re-attach to a shared instance during the drain window. Idempotent, irreversible. Flips `ready` to `false` and calls `ServerConfig::healthHandler`, on the server strand. |
| `drainedWithin(deadline, replyExec)` | `[[nodiscard]] Completion<bool> drainedWithin(std::chrono::milliseconds deadline, IExecutor& replyExec)` — settled on the server strand: `true` once no `execute` is in flight, `false` if `deadline` elapses first; delivered on `replyExec`, the executor the caller attaches from (borrowed: it must outlive the delivery). Blocks nothing. |

### `ServerConfig`

| Field | Notes |
|---|---|
| `limits` | `LimitPolicy`; all-zero (default) applies no limit. |
| `logProvider` | `LogProvider`, consulted on the server strand for an instance created with a non-empty `contextKey`; null attaches no log. |
| `healthHandler` | `std::function<void(const HealthStatus&)>`: called from the constructor, then on the server strand when readiness changes. |
| `minProtocolVersion`, `maxProtocolVersion` | Inclusive `hello` range; default `{kProtocolVersion, kProtocolVersion}`. |
| `payloadCompleteness` | `PayloadCompleteness`; default `Lenient`. |

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

### `QtWebSocketBackend` (namespace `morph::qt`)

| Method | Notes |
|---|---|
| `QtWebSocketBackend(serverUrl, dispatcher = defaultDispatcher(), registry = defaultRegistry(), tls = nullopt, cfg = Config{})` | Opens the socket to `serverUrl` in the constructor. `dispatcher`/`registry` params are accepted but unused (models live on the server). `tls` non-null → `wss://`. `tls` is not declared at all when Qt is built with `QT_NO_SSL` (see above). |
| `QtWebSocketBackend(serverUrl, tls, cfg = Config{})` | Overload that skips the unused `dispatcher`/`registry` pair: a caller who only needs `tls`/`cfg` reaches them directly, without naming `morph::model::detail::defaultDispatcher()`/`defaultRegistry()` explicitly. Delegates to the main constructor with both defaulted. Not declared on a `QT_NO_SSL` build (no `tls` parameter to distinguish it from the `(serverUrl, cfg)` overload below). |
| `QtWebSocketBackend(serverUrl, cfg)` | Overload that skips `dispatcher`/`registry` and `tls` together — the common case for a caller that only wants to set a `Config` field (e.g. `reconnectEnabled`) over a plaintext `ws://` connection. Delegates to the main constructor with `dispatcher`/`registry` defaulted and (on an SSL-enabled build) `tls = std::nullopt`. |
| `bindModel(request, cbExec)` | Builds the envelope `request`'s shape names — `register`, shared `register`, or `attach` — assigns a fresh `callId` (the same counter `execute` uses), files the pending entry in `_pendingControl[callId]` and sends. The `Completion` settles later from `onTextMessage` (or from `cancelPending` on a disconnect). A private bind on an unconnected socket is queued in `_queuedRegistrations` instead; a keyed one rejects with `"disconnected"`. |
| `promoteModel(request, cbExec)` | Sends `assign` through the same path. An empty `primary` or zero `mid` resolves with `request.mid` without sending. |
| `instances(typeId, cbExec)` | Sends `instances` through the same path; the reply's body is decoded into the key list. Rejects with `"disconnected"` when not connected. |
| `waitForConnected(timeoutMs = 5000)` | Pumps a local `QEventLoop` until connected or timeout; returns `_connected`. Not for a WASM main thread. |
| `negotiateProtocolVersion(replyExec)` | Opt-in: sends `hello` through the same path and settles the returned `Completion` with `wire::interpretHelloReply`'s classification; records whether the server advertised `"cancel"`, and re-sends `hello` after every reconnect. Rejects on an explicit version rejection, when not connected, or on a drop. |
| `registerModel(typeId, factory)`, `assignPrimary(...)`, `listInstances(typeId)` | Throw `std::logic_error`: each would have to wait for a reply. `registerModelWithContext` is `IBackend`'s default, which forwards to `registerModel`. |
| `deregisterModel(mid)` | **Fire-and-forget** — sends only if connected, does not wait for the ack. Carries a non-zero `callId` from the same counter `execute` uses, recorded in `_pendingDeregisters` so `onTextMessage` recognises the reply and drops it. |
| `execute(mid, call, cbExec)` | Assigns a `callId`, sends `execute`, returns a `Completion`. Immediate `DisconnectedError` if not connected. |
| `notifyBackendChanged()` | No-op. |
| `cancelPending(exc)` | On the socket's thread, takes `_pending`, `_pendingControl` and `_queuedRegistrations` out, sends a `cancel` for each `execute` when connected to a server that advertised `"cancel"`, then delivers `exc` to each — the exception itself, so a request rejected by a dropped socket carries the same `DisconnectedError` an `execute` does. |
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
| `sendTimeout` | `std::chrono::milliseconds` | `30 s` |
| `handshakeTimeout` | `std::chrono::milliseconds` | `10 s` |

### `SocketBackend` (namespace `morph::net`)

| Method | Notes |
|---|---|
| `SocketBackend(loop, serverUrl, cfg = Config{})` | Parses `serverUrl` (`ws://` only — throws immediately on `wss://`) and posts the first connection attempt to `loop`, an `exec::IoLoop` that must outlive the backend. |
| `explicit SocketBackend(serverUrl, cfg = Config{})` | The same, on a private `IoLoop` the backend owns. The URL is parsed before that loop is started. |
| `~SocketBackend()` | Runs its close on the loop — inline on the loop's own thread, posted and waited for otherwise: closes the connection, retires the backoff timer, rejects every pending call — a waiting synchronous verb's included — with `DisconnectedError`. Frames already queued — the cancels and deregisters of a `cancelPending` just before — are written before the socket closes, bounded by `sendTimeout`. Never waits for a dial in progress. |
| `waitForConnected(timeout = 5000ms)` | Posts a waiter the loop releases on the next completed connection, and blocks the calling thread on it until then or the timeout; returns the current connected state. On the loop's own thread it answers at once. The backend must outlive the call — destroying it while a thread is parked here is undefined, and there is no cancel (see Lifetime & ownership). |
| `registerModel(typeId, factory)` | Forwards to `registerModelWithContext` with an empty `contextKey`; `factory` ignored. |
| `registerModelWithContext(typeId, factory, contextKey)` | Synchronous: posts `register` carrying `contextKey` and waits on a future for the loop to settle the reply, so the server's `LogProvider` is consulted for a private registration exactly as it is for a shared one. `factory` ignored. Throws on `err` reply or disconnect, and on the loop's own thread. Any number may be in flight, matched by `callId`. |
| `bindModel(request, cbExec)` | Native override of the structural surface. Posts the envelope `request`'s shape names and returns immediately; the loop gives it a non-zero `callId`, files it and writes it; every shape carries `request.contextKey`, the private one included; the loop settles the `Completion` when the reply arrives, delivered on `cbExec`. Any number may be in flight. Rejects with `DisconnectedError` when the socket is down or drops first, or with `std::runtime_error{"<verb> failed: <server message>"}`. |
| `promoteModel(request, cbExec)` | The `assign` counterpart of `bindModel`, on the same path; resolves with `request.mid` echoed back. An empty `primary` or a zero `mid` resolves without sending, matching `assignPrimary`'s guards. |
| `deregisterModel(mid)` | **Fire-and-forget** — posted only if connected, does not wait for the ack. The loop gives it a non-zero `callId` from the same counter `execute` uses, so its unawaited `ok` is never taken for another call's reply. Needs no pending-id bookkeeping of its own: the reply router already drops a non-zero `callId` that is not pending. |
| `execute(mid, call, cbExec)` | Serialises the action on the calling thread and posts the envelope; the loop assigns a `callId`, files the pending record and writes the frame. Returns a `Completion`, already rejected with `DisconnectedError` if not connected. Callable from any thread; any number may be in flight. |
| `notifyBackendChanged()` | No-op. |
| `cancelPending(exc)` | Posted: the loop drains **both** pending tables — the `execute` calls and the `bindModel`/`promoteModel` control calls — and delivers `exc` to each state. Covers every call issued before it. When the server advertised `"cancel"`, sends one for each drained `execute` first. |
| `negotiateProtocolVersion(replyExec)` | Opt-in: posts a `hello` on the callId-multiplexed path and settles the returned `Completion` with `wire::interpretHelloReply`'s classification; records whether the server advertised `"cancel"`, and re-sends `hello` after every reconnect. Rejects on an explicit version rejection, and with `DisconnectedError` when the socket is down or drops. |
| `setReconnectHandler(handler, exec)` | Posted: the loop stores the handler and its executor, and after every *subsequent* connect posts the handler to `exec` — never runs it on the loop. `nullptr` clears. |
| `setSession(session)` | Posted: the loop stores the session it stamps onto every control envelope it builds afterwards. |

### `SocketServerConfig` (`morph::net::SocketServer::Config`)

| Member | Type | Default |
|---|---|---|
| `backlog` | `int` | `64` |
| `handshakeTimeout` | `std::chrono::milliseconds` | `10 s` |
| `sendTimeout` | `std::chrono::milliseconds` | `30 s` |

`handshakeTimeout` bounds the whole RFC 6455 Upgrade read (accept to the
terminating `\r\n\r\n`), as one loop timer that closes the connection when it
runs out: a peer that dribbles one byte at a time cannot stretch it, the way it
could stretch a per-read bound to `handshakeTimeout` times the header's 64 KiB
cap. `SocketBackendConfig::handshakeTimeout` is the same whole-read bound on
the client side. `sendTimeout` bounds each 64 KiB chunk of a reply's write,
with the same mechanism and default as `SocketBackendConfig::sendTimeout`:
without it, a peer that stops reading fills the kernel send buffer and its
replies queue on the loop forever, while the connection goes on executing
actions whose replies go nowhere. When it runs out the connection is retired —
its socket closed, its models reclaimed. Both are `0`-disables opt-outs;
deliberately not opt-in, since neither has the false-positive risk that ruled
out an `idleTimeout` (reaping an idle-by-design desktop client with no
keepalive to tell "idle" from "dead" apart).

### `SocketServer` (namespace `morph::net`)

| Method | Notes |
|---|---|
| `SocketServer(loop, server, port = 0, cfg = Config{})` | Fronts `RemoteServer& server` on `loop`, an `exec::IoLoop` that must outlive it. Does not start listening. |
| `SocketServer(server, port = 0, cfg = Config{})` | The same, on a private `IoLoop` the first `listen()` creates. |
| `listen()` | On the loop, and waited for: binds `127.0.0.1:port` with `SO_REUSEADDR`, hands the listener to the loop and starts the accept flow; returns success. `false` if the port cannot be bound or, for the loop-less constructor, the loop cannot be created (a process out of descriptors). |
| `port()` | Bound port (OS-assigned when constructed with `0`), or `0` when not listening — including after the listener stopped being able to accept. Atomic; callable from any thread. |
| `close()` | On the loop, and waited for (inline on the loop's own thread): closes the listener, reclaims every connection's models (`closeConnection`) and closes its socket. `port()` reads `0` afterwards and the port is free to rebind. Idempotent; also run by the destructor. Concurrent callers on a **live** object are safe — each call is one loop task, run one at a time. Racing `close()` against the *destructor* remains out of contract, as for any member call. |

## `executeInto` — settling the caller's own completion

`IBackend` carries two dispatch entry points:

| Verb | Shape | Who implements it |
|---|---|---|
| `execute(mid, call, cbExec)` | returns `Completion<std::shared_ptr<void>>` | Pure virtual. Every backend. |
| `executeInto(mid, call, cbExec, sink)` | settles an `async::detail::ISettleSink` the caller owns | **Default-implemented**, forwarding to `execute`. Overridden by `LocalBackend` only. |

`Bridge::executeVia` dispatches through `executeInto`, handing down a sink that
**is** the typed `CompletionState<R>` its caller holds. That removes the
`.then`/`.onError` block that used to forward the backend's erased completion
into the typed one — six allocations per call of the 14.06 a local round trip
took, measured down to 8.06 (see
[`bridge.md`](bridge.md#bridgesink--the-typed-state-the-backend-settles)).

**Why a default rather than a pure virtual.** `execute` is implemented by five
production backends and roughly ten test doubles. Making `executeInto` pure
would be a fifteen-site change to gain an allocation on one path. The default
is exactly the forwarding block it replaces, so a backend that does not
override it costs precisely what it costs today — no gain, no regression.

**The trap this creates, and how it is closed.** `LocalBackend` overrides
`executeInto`, so a subclass of `LocalBackend` that overrode only `execute`
would be bypassed for every bridge dispatch and would intercept nothing but the
handful of direct `execute` callers — silently, with every test it still
reaches passing. `LocalBackend::execute` is therefore **`final`**, which turns
that into a compile error naming the fix: derive from `LocalBackend` and
override `executeInto`, the primitive both entry points share. A backend
deriving straight from `IBackend` is unaffected — it overrides `execute` and
inherits the default `executeInto`, which forwards to it.

**Pending tracking follows the sink.** `LocalBackend::_pending` holds
`weak_ptr<ISettleSink>` rather than `weak_ptr<CompletionState<shared_ptr<void>>>`,
and `cancelPending` calls `settleException` on each. A `cancelPending` racing a
reply settles the same sink twice, which `ISettleSink`'s contract requires the
implementation to absorb — see
[`completion.md`](completion.md#detailisettlesink--where-a-backend-settles-one-dispatch).

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Dual-path `ActionCall` | Three callables: `localOp`, `serializeAction`, `deserializeResult` | The same `ActionCall` struct works for both local and remote execution without an `if (isRemote)` branch at the call site — each backend uses the field(s) it needs. |
| Callables are function pointers, not `std::function`s | `std::string (*)(const void*)` etc., with the action in `ActionCall::action` | Every call builds all three, whichever path it takes, so a stateful callable charges an allocation to calls that never invoke it. The behaviour is a constant of `(Model, Action)`; only the action is per-call. Measured at 3 allocations per local round trip. The cost is an explicit borrow: see [Lifetime contract](#lifetime-contract). |
| Type ids are `string_view`s, not `std::string`s | `modelTypeId`, `actionTypeId` | A typed call's ids are views of `constexpr` string literals from the registration macros, so copying them into a `std::string` buys nothing and allocates whenever an id exceeds the SSO threshold (`"CreateSwimlane"` is 14 characters; the margin is one character wide). A raw call's ids are views into the call's own action, valid as long as the call. |
| `registerModelWithContext` | Virtual with a default that drops `contextKey` | `LocalBackend`'s factory closure already captures identity, so there is nothing to forward — which is why the default drops the key rather than being pure virtual. A backend whose instances are constructed on the far side of a wire protocol has no such closure, so the envelope is the only channel the identity has: `SimulatedRemoteBackend` and `SocketBackend` both override it so the server's `LogProvider` can attach an action log. The default being *permissive* is what lets a wire backend ship without an override and silently stop journalling private registrations; the price of that permissiveness is that "is this a wire backend?" has to be answered by hand for each new transport. |
| `RemoteServer` heap requirement | `std::enable_shared_from_this` | Every task the server posts captures `shared_from_this()` — the server must outlive any in-flight message. |
| `RemoteServer` state on one strand | `exec::OwnerStrand` over the pool; `execute` admitted there, run on the model's strand | The registry, scopes, in-flight count and readiness need no lock, and per-model order is two strands' FIFO rather than a ticket gate. Admission is not on the model's strand, so a rejection never waits on a busy model — see [Per-model execute ordering](#per-model-execute-ordering). |
| `RemoteServer` configuration | `ServerConfig` at construction, no setters | Read on the strands without a lock because nothing writes it afterwards. |
| `handleInline` | Synchronous; caller-restricted to control messages | Safe to call from a worker-pool thread (e.g. from a `BridgeHandler` constructor). It is meant for `register`/`deregister` only; an `execute` envelope is rejected with an `err` reply, because `dispatchExecute` posts to the strand and would reply after `handleInline` returns (writing into an already-destroyed reply buffer). The rejection is now enforced by the code, matching the documented intent. |
| `SimulatedRemoteBackend` factory ignored | Model construction delegated to `RemoteServer`'s `ModelRegistryFactory` | The factory closure lives on the client side; the server owns the actual instances. |
| `cancelPending` snapshots | Weak-ptr snapshot under lock, then resolves outside | Avoids holding the lock while delivering exceptions to each state, preventing deadlock if a callback re-enters the backend. |
| `_pending` compacted amortised, not intrusively | Sweep when `size() >= _compactAt`, re-arm at twice the survivors | The intrusive alternative is to give `CompletionState` a slot index and unlink on settle, making both registration and removal O(1) with no sweep at all. Rejected. It pushes a back-reference to the backend's table into a type shared by every backend, and puts a cross-thread unlink — a lock, or a post to the owner — on the settle path of every completion, turning a cost paid once per burst into work paid by every strand thread on every result, on the exact path `Completion`'s value-handling contract exists to keep cheap. The amortised sweep buys the same O(1) admission for one `size_t` of state confined to `LocalBackend`, at the cost of a list bounded at 2× the live count instead of exactly it. |
| `setReconnectHandler` | Default no-op | Only backends with a transport layer (e.g. `QtWebSocketBackend`) need to react to reconnects. `LocalBackend` and `SimulatedRemoteBackend` never invoke it. |
| `setConnectHandler`/`setDisconnectHandler` on `IBackend`, not only `QtWebSocketBackend` | Same no-op-default pattern as `setReconnectHandler` | Connection state is a property of any transport-backed backend; a UI observing it shouldn't have to downcast to a concrete backend type. A purely local backend has no meaningful connection state, so the base-class hook is simply inert for it — no behavior change, matching the existing `setReconnectHandler` precedent exactly. |
| `setDisconnectHandler` fires before reconnect scheduling | Ordering choice, not incidental | An instant successful reconnect must not look, from an observer's perspective, like nothing happened — the disconnected state must be visible even when the very next thing that happens is a fresh `connected`. |
| Strand-per-model | `ModelStrands` (core-cpp's `KeyedStrands`) serialises actions per `ModelId` | Actions against the same model run sequentially; different models can run in parallel. No global lock on the pool. |
| Overwrite `session.principal` on remote execute | `authenticate()` result replaces the client claim before dispatch | The client-asserted `Context::principal` is untrusted; a verifying authorizer makes the token-derived identity authoritative so `session::current()->principal` inside a model is trustworthy. Non-verifying authorizers return `nullopt` and change nothing. |
| Opaque model ids | Monotonic counter run through a keyed 4-round Feistel permutation (`detail::OpaqueIdGenerator`), key drawn from `std::random_device` at construction | Guarantees uniqueness (Feistel networks are bijections for any round function) while making ids unguessable without the key; self-contained, no external crypto dependency — same posture as the reference HMAC-SHA256 in `session_auth.hpp`. |
| WebSocket `deregisterModel` is fire-and-forget | Send-only, no nested event loop | A synchronous deregister would need a nested `QEventLoop`, which is typically driven from a destructor (`~BridgeHandler`) and can trip Qt asserts. A lost/undelivered deregister no longer leaks indefinitely: `QtWebSocketServer`'s connection scope reclaims the model at the next disconnect (see Limitations). |
| Connection-scoped cleanup bypasses `IAuthorizer` | `closeConnection` never calls `authorize`/`authorizeInstance`/`authenticate` | It is server housekeeping triggered by the transport's own connection-close event, not a caller action; synthesising a `deregister` envelope would need a token to pass ownership checks and would require the transport to learn ids by parsing replies — recording the owning connection at register time is simpler and cannot desync. |
| `callId`-multiplexed replies | Every request carries a non-zero `callId` from one counter per connection, the fire-and-forget `deregister` included, on both WebSocket transports; a reply with `callId == 0` names no request and is dropped | Lets one socket carry many requests in flight and match each reply to its `Completion` without waiting for any of them. `deregister` takes an id too, filed only so its reply is recognised and discarded rather than mistaken for another request's. |
| Reconnect handler skipped on first connect | Fired only when `_everConnected` was already true | The initial handler registration is driven by `BridgeHandler` constructors; firing the reconnect handler on the very first connect would double-register. |
| No reconnect for never-connected sockets | `disconnected` schedules a retry only if `_everConnected` | A socket that never reached the server (bad URL / refused) fails fast via `waitForConnected` returning false, rather than backing off forever. |
| Server reply marshalled to the Qt thread | `QMetaObject::invokeMethod(..., QueuedConnection)` with a `QPointer` | `RemoteServer::handle` produces the reply on a pool thread, but `QWebSocket::sendTextMessage` must run on the Qt thread; the weak `QPointer` drops the reply cleanly if the client disconnected meanwhile. |
| `executeTimeout` implementation | A `morph::async::detail::TimeoutScheduler` per `RemoteServer`, created by the constructor when `ServerConfig::limits.executeTimeout` is positive, on a private `exec::IoLoop` (one thread running core-cpp's `PlatformLoop`), not a per-call thread | `IExecutor` has no delayed-post primitive and `RemoteServer` is transport-agnostic (cannot assume Qt's `QTimer`). One thread amortizes across every timed call, and a server configured without the feature pays no cost. The deadlines are the loop's own timers, so nothing polls: an armed timer is what bounds the loop's next wait. |
| `messagesPerSecond` algorithm | Per-connection token bucket, capacity = rate, continuous refill; on empty the frame is refused with an `err` reply, and the connection is left open | Simplest correct rate limiter; allows a legitimate one-second burst without penalizing an otherwise well-behaved client. Refusing rather than closing keeps a transient burst from taking down the connection. The frame is *answered* rather than discarded because a reply costs nothing at the protocol level and is the difference between a caller's `Completion` failing and it hanging: the id is recovered by the same bounded prefix scan (`peekCallId`) the `maxMessageBytes` branch uses, so no decode of a frame that will not run is needed. |
| Graceful shutdown drains via a shared in-flight counter, not a new `IExecutor::waitIdle` | `RemoteServer` counts its own accepted-but-unreplied executes rather than adding a general drain API to `IExecutor` | The drain condition morph can define precisely — "every accepted execute has replied" — lives at the server layer, where the work is counted; executor.md's "no graceful drain / `waitIdle`" limitation is deliberately left as-is for raw executor users. |
| Backend-change-awareness captured at registration | `IModelHolder::isBackendChangeAware()` (compile-time answer per model type) + `LocalBackend::_changeAware`, maintained by `registerModel`/`deregisterModel` | Replaces a per-`notifyBackendChanged`-call `dynamic_cast` sweep over every live model with a virtual query done once at registration, and a lookup restricted to the models that actually opted in. No RTTI dependency; cost is O(change-aware models) instead of O(all models). No change to the model-facing contract (`IBackendChangedSink`, `BackendChangedMixin`) or to when/where `onBackendChanged()` runs. |
| `morph::net`'s I/O model | One `exec::IoLoop` (a core-cpp `PlatformLoop` and its one thread) injected into every component, instead of a thread per component or the Qt event loop | Lets `SocketBackend`/`SocketServer` run with no GUI event loop and no Qt dependency, and puts every socket, timer and probe of a process on one owner: the components' state needs no lock, and a process runs one I/O thread rather than one per connection. Injected rather than a framework-owned singleton so the dependency is visible in each constructor and nothing global outlives a test. `SocketBackend` stays callable from any thread (`QtWebSocketBackend` is pinned to its event-loop thread) because every verb posts to the loop. |
| `morph::net` frame/handshake implementation | Hand-rolled RFC 6455 (SHA-1 + HTTP Upgrade + frame codec), with base64 from core-cpp, not a WebSocket library | The spec's own interop requirement (a `morph::net` client/server must talk to the real Qt transport and vice versa) rules out a bespoke non-WebSocket framing; hand-rolling avoids adding a dependency beyond core-cpp, which morph links anyway, and RFC 6455's core (handshake + frame codec, including fragment reassembly) is a small, bounded surface. |
| `WsFrameReader` reassembles fragments | Accumulates continuation frames and returns only the completed message | Fragmentation is not an exotic case: a peer fragments whenever a message exceeds its outgoing frame size, and Qt's `QWebSocket` defaults that to 512 KiB. Rejecting fragments broke interop with the transport this project ships, for every payload past that size. Control frames interleaved between fragments pass through untouched, and the reassembled total is bounded by `wire::kMaxEnvelopeBytes` so a stream of tiny continuations cannot grow the buffer without limit. |
| `WsFrameReader` rejects RFC 6455-illegal frames instead of tolerating them | Masking direction, RSV bits, opcode range, control-frame framing, Close status code, minimal length encoding and text-payload UTF-8 are all checked; a violation throws out of `tryExtractFrame()` and the call site drops the connection | The interop requirement above makes what the reader *refuses* part of the transport's contract rather than an implementation detail: a tolerant reader accepts ten classes of illegal frame, and a peer that sends one here gets disconnected instead. The reader is given its role at construction (`expectMasked`) because §5.1 is directional — a server MUST reject an unmasked client frame and a client MUST reject a masked server frame, and that rule is the anti-cache-poisoning defence, not a formality. Text UTF-8 is validated incrementally, since a multi-byte sequence may straddle a fragment boundary. On the sending side the mask key is drawn per frame from a thread-local `std::random_device` rather than a thread-local `std::mt19937`, whose state a peer can reconstruct from 624 observed keys (§5.3); `random_device` has no reproducible state to recover, and holding it thread-local keeps the entropy source open instead of reacquiring it on every outbound message. |
| Registration continuation delivered via a caller-supplied `IExecutor&`, not on the backend's thread | `bindModel`/`promoteModel` return a `Completion<ModelId>` built with the caller's executor | A per-verb non-blocking twin can only state its threading contract in prose, and a violation of it is a use-after-free. Making the executor an argument moves the choice of delivery thread from fifteen implementors that know nothing about the caller's teardown to the one caller that does, and turns it from a `@note` into a value a call site must produce. Rejected: matching `execute`'s `IExecutor*` — a null pointer makes `Completion` drop every handler silently, which is the same unobservable failure the surface removes. |
| One `bindModel` instead of three acquire verbs | Behaviour selected by `BindRequest`'s shape (`primary` empty?, `current` zero?) | The three verbs already degrade into each other exactly along those two fields, so naming them separately stated the same distinction three times — and tripled it again for the `*Async` twins. The default implementation still routes each shape to the legacy verb it names, so a backend that overrides only some of the three is unaffected. |
| `SynchronousBackendAdapter` is a decorator, not a base class or a CRTP mixin | Wraps `shared_ptr<IBackend>` and forwards every verb | It must work on `LocalBackend` and eleven test doubles *without modifying them*, which rules out anything they would have to derive from. Cost is one forwarding method per unchanged verb; benefit is that an unmodified blocking backend reaches the new surface at all. |
| The adapter's blocking executor is required, not defaulted | Constructor parameter with no default; null `inner` throws | "Where does the blocking happen" is the only question the class exists to answer. An adapter that silently ran the call inline when handed nothing would block on some configurations and not others — contract by configuration, which is the thing being removed. |
| `SocketBackend` runs reconnect handlers on a dedicated thread | Not on the I/O loop that completes the connection | A reconnect handler re-registers models via the synchronous control path, which waits for a reply only the loop's read flow can deliver. Inline, that wait blocks the very thread that would satisfy it, deadlocking the transport with no timeout. Still load-bearing even though `bindModel` cannot deadlock this way: the blocking verbs can, and a caller may still reach them. |
| `SocketBackend` implements `bindModel`/`promoteModel` natively | Not wrapped in `SynchronousBackendAdapter`, although it overrides none of the four `*Async` verbs | The I/O loop already demultiplexes replies by `callId` for `execute` and `RemoteServer` echoes `callId` on every control reply, so the non-blocking path costs a second `PendingCallTable` and no protocol change. Wrapping instead would park a thread per bind for a round trip this transport need not park for. The adapter's reconnect-handler property, the other reason to consider it, does not apply: it forwards `setReconnectHandler` and the blocking verbs straight through, so a wrapped `SocketBackend` would run reconnect control calls exactly where it does today. |

## Lifetime annotations

`LocalBackend`'s `IExecutor& workerPool`, `RemoteServer`'s `workerPool`,
`dispatcher` and `registry`, and `SimulatedRemoteBackend`'s `RemoteServer&` are
all marked `MORPH_LIFETIMEBOUND` (`morph/attributes.hpp`) — the "must outlive"
column of the destruction-ordering table, restated where the compiler can check
it. See [concurrency_and_lifetimes.md](../concurrency_and_lifetimes.md#morph_lifetimebound--the-must-outlive-rules-told-to-the-compiler).

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
| observability.md | The `morph::observe` metrics/trace seam wrapping `RemoteServer`/`LocalBackend` dispatch, and `RemoteServer::health()`/`ServerConfig::healthHandler`. |
| testing_strategy.md | `fuzz_dispatch_execute` fuzzes `RemoteServer::handle` directly; the soak test (`test_soak_switch_backend.cpp`) cycles `switchBackend` between `LocalBackend` and `SimulatedRemoteBackend` under load; the load benchmark (`bench_dispatch_latency.cpp`) baselines dispatch throughput/latency; the adversarial run (`test_qt_websocket_adversarial.cpp`) drives a hostile client against `QtWebSocketServer` and exercises the default (unconfigured) `LimitPolicy`/`QtWebSocketServerConfig`. |

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
  plain worker thread, and `waitForConnected` pumps a nested `QEventLoop` on
  that thread. Completion callbacks reach the
  GUI only if `cbExec` (typically `QtExecutor`) posts back to the Qt loop.
  `morph::net::SocketBackend` does not have this limitation (see above) — but a
  test or app that mixes a `SocketBackend`/`SocketServer` with a
  `QtWebSocketServer`/`QtWebSocketBackend` peer on the *same* thread that owns
  the `QCoreApplication` must still pump Qt events while any blocking
  `morph::net` call is outstanding, or the Qt-side peer starves (see the
  `SocketBackend`/`SocketServer` Threading note above).
- **`morph::net` is Linux/macOS only.** The listener is bound through
  `TcpSocket`, a thin wrapper over POSIX `sys/socket.h`, before the loop adopts
  it; `MORPH_BUILD_NET=ON` on Windows produces a `message(WARNING ...)` and
  builds nothing. Windows/Winsock2 support is documented future work, not
  implemented today. Teardown does not depend on the kernel: closing the
  listener and each connection's socket resumes their parked flows through the
  loop on every platform.
- **A blocking callback stalls the whole loop.** Every socket, timer and probe
  built on one `IoLoop` shares its thread, so a `TimeoutScheduler` callback, a
  `NetworkMonitor` probe or callback, or a completion delivered on
  `inlineExecutor()` that blocks, stalls every connection on that loop until it
  returns. The synchronous control verbs throw on the loop's thread; anything
  else is the caller's to keep short.
- **A hostname is resolved off the loop, on core-cpp's resolver pool.** A
  numeric address never leaves the loop's thread; a name goes to
  `core::net::defaultAsyncResolver()`, a small fixed pool core-cpp owns
  process-wide.
- **`morph::net` has no TLS.** `SocketBackend`/`SocketServer` speak plaintext
  `ws://` only; `parseWsUrl` throws immediately on a `wss://` URL. A `wss://`
  variant needing a TLS library (e.g. OpenSSL) is future work.
- **`morph::net` never *sends* a fragmented message.** `encodeWsFrame` always
  writes one complete `FIN=1` frame. Incoming fragments are reassembled (see
  the design-decision table), so this is one-directional and not a practical
  limitation for morph's own traffic — a `wire::Envelope` is always one JSON
  line, and the frame format's 64-bit extended-length field already covers up
  to `wire::kMaxEnvelopeBytes` in a single frame.
- **A protocol violation drops the connection without sending a Close frame.**
  `WsFrameReader` rejects a frame that is masked in the wrong direction (an
  unmasked client→server frame, or a masked server→client one), has any RSV
  bit set (no extension is ever negotiated), carries a reserved opcode
  (`0x3`-`0x7`, `0xB`-`0xF`), is a control frame that is fragmented or longer
  than 125 bytes, is a Close frame whose status code is not one the IANA
  close-code registry allows on the wire (1000-1003, 1007-1014, 3000-4999 —
  note 1012-1014 were registered after RFC 6455 §7.4.1's own table) or whose
  reason phrase is not valid UTF-8, uses a non-minimal extended-length
  encoding, or is a text message whose payload is not valid UTF-8. Each of those throws out
  of `tryExtractFrame()`; `SocketBackend::drainFrames`/
  `SocketServer::drainFrames` catch it, stop reading and tear the connection
  down — **no `1002`/`1007`/`1009` Close frame is sent first**, so the peer
  learns only that the connection went away, and neither side logs which check
  failed. Sending the RFC status code needs the reader's error model to change
  from throwing to `std::expected<..., WsProtocolError>` at both call sites;
  that is separable work and is not done here. A Close frame
  *from* the peer is a different path and is not a violation: it is echoed back
  carrying the peer's own status code (§5.5.1), where it used to be echoed
  empty.
- **A dial in progress outlives `~SocketBackend` by up to
  `Config::connectTimeout`.** The destructor does not wait for it: the dial's
  flow holds the loop-side state, finds it closed when the dial completes, and
  drops the socket. Until then it holds a descriptor and, for a hostname, a
  resolver-pool slot.
- **`RemoteServer::handleInline` needs a free pool thread.** It waits for the
  server strand, which runs on the pool; a one-thread pool calling it from
  inside its own task, or every pool thread blocked in it at once, never gets
  its reply. See [`handleInline(msg)`](#remoteserver--server-side-message-handler).
- **`RemoteServer` admits every `execute` on one strand.** The authorizer's
  hooks, the registry lookup and the optional payload parse run one at a time
  across all models; a slow authorizer slows admission for every model, though
  never the handlers, which run on their own strands.
- **Graceful shutdown never preempts a running action.** `beginShutdown()`,
  `drainedWithin()`, and `closeGracefully()` only stop new work from arriving
  and wait for old work to finish; a model whose action runs longer than the
  caller's `deadline` still finishes on its strand after `drainedWithin`
  answers `false` (and after `closeGracefully`'s hard stop reclaims the
  connection). This is intentional — morph never interrupts a strand task —
  but it does mean a model with no self-imposed bound can make
  `closeGracefully` always hit its hard stop.