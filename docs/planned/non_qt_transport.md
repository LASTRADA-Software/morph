# A non-Qt / transport-agnostic reference transport (planned)

> **Status: planned — not yet implemented.** This spec extends
> [backend.md](../spec/backend.md) (the `IBackend` contract and `RemoteServer`
> handling) and [wire.md](../spec/wire.md) (the `Envelope`). It provides a
> reference transport that does not depend on Qt, so a server or client can run
> without a Qt event loop. See [todo.md](../todo.md).

## The gap

The only network transport is Qt. [backend.md](../spec/backend.md)'s Limitations
name it: "`QtWebSocketBackend` must live on the Qt event loop thread; there is no
way to drive it from a plain worker thread ... WebSocket transport is
single-threaded and Qt-bound." `RemoteServer` itself is transport-agnostic (it
"receives JSON envelopes ... from any transport," [backend.md](../spec/backend.md)),
but the only concrete client `IBackend` and the only concrete server front are
`QtWebSocketBackend` / `QtWebSocketServer`.

So a shop that does not want Qt on the server (or client) must implement the
whole `IBackend` interface and wire a socket to `RemoteServer` themselves, from
scratch, with no worked reference. The seams are all present — this is a
missing *reference implementation*, not a missing capability.

## Goal

Ship one reference transport-agnostic transport — a plain-socket / HTTP
client-side `IBackend` and a matching server front — that talks to the **same**
`RemoteServer` over the **same** `wire::Envelope`, with **no Qt dependency**, so a
non-Qt deployment has a working starting point instead of a blank
`IBackend`. It is additive: the Qt transport is unchanged and stays the default
for Qt apps.

## Design

### The contract it must satisfy (all EXISTING)

The reference client backend implements every method of
`detail::IBackend` ([backend.md](../spec/backend.md)), matching the semantics the
existing transports already establish:

| Method | Reference behavior (mirrors `QtWebSocketBackend`) |
|---|---|
| `registerModel(typeId, factory)` | Sends a `register` `Envelope` and blocks for the `ok`/`err` reply (synchronous control op). `factory` ignored (models live on the server). Throws `std::runtime_error("register failed: ...")` on `err` or disconnect. |
| `registerModelWithContext(typeId, factory, contextKey)` | Default (drops `contextKey`) unless the transport carries it; overriding it lets the server's `LogProvider` attach a log. |
| `deregisterModel(mid)` | Fire-and-forget `deregister` `Envelope` if connected (same rationale as Qt — avoids a blocking teardown). As with the Qt transport there is no connection-scoped server cleanup, so an undelivered `deregister` leaves the model registered on the server; explicit deregistration is the client's responsibility. |
| `execute(mid, call, cbExec)` | Assigns a monotonic `callId`, records the completion in a `callId → pending` map, serialises via `call.serializeAction()`, sends an `execute` `Envelope`, returns a `Completion` resolved by the matching-`callId` reply. Immediate `DisconnectedError` if not connected. |
| `notifyBackendChanged()` | No-op (models live on the server, like every remote backend). |
| `cancelPending(exc)` | Snapshots the pending map, resolves each with `exc`; on disconnect, resolve all with `DisconnectedError`. |
| `setReconnectHandler(handler)` | Stores it; invoked on each *subsequent* connect so `Bridge` re-registers live bindings. |

It reuses the existing typed error hierarchy — `DisconnectedError`,
`BackendChangedError`, `BridgeDestroyedError` ([backend.md](../spec/backend.md)) —
unchanged, and the existing `callId` multiplexing so concurrent in-flight
executes work (`RemoteServer` echoes the request `callId`, [wire.md](../spec/wire.md)).

### The threading model is the key difference

`QtWebSocketBackend` is pinned to the Qt event loop and uses nested
`QEventLoop`s for its synchronous `register`; the reference transport instead runs
its own **I/O thread** and uses ordinary condition-variable waits for the
synchronous control ops:

```cpp
// namespace morph::net — NEW, in an opt-in header, no Qt include.

class SocketBackend : public morph::backend::detail::IBackend {
public:
    /// Connects to `serverUrl` (ws:// or wss://, or a plain TCP framing).
    /// Runs its own I/O thread; does NOT require any GUI event loop.
    explicit SocketBackend(std::string serverUrl, Config cfg = {});
    // ... IBackend overrides above ...
};
```

- **No event-loop requirement.** The backend owns a dedicated reader thread that
  frames incoming messages and routes them by `callId` to the pending map (guarded
  by a mutex, since replies arrive on the I/O thread and `execute`/`cancelPending`
  can be called from `Bridge` on another thread — the same locking `QtWebSocketBackend`
  applies to `_pending`, [backend.md](../spec/backend.md)).
- **Synchronous control via condition variable.** `registerModel` sends and waits
  on a `std::condition_variable` for the correlated reply (or a disconnect
  wakeup), replacing the nested `QEventLoop`. The disconnect path wakes a parked
  waiter so a register whose reply never arrives fails with `"disconnected"`
  rather than hanging — the same hardening `sendSync` has today.
- **Callbacks still marshal via `cbExec`.** Completion `.then`/`.onError` deliver
  on the `IExecutor*` passed to `execute` ([completion.md](../spec/completion.md)),
  so a non-Qt host uses a `ThreadPoolExecutor`/`MainThreadExecutor`
  ([ARCHITECTURE.md](../ARCHITECTURE.md)) as `cbExec` instead of `QtExecutor`.

### The server front

A matching non-Qt server front fronts the **same** `RemoteServer`:

```cpp
// namespace morph::net — NEW.
class SocketServer {
public:
    /// Fronts an existing RemoteServer (held by reference — the RemoteServer
    /// shared_ptr must outlive this, exactly as for QtWebSocketServer).
    SocketServer(morph::backend::RemoteServer& server, std::uint16_t port,
                 Config cfg = {});
    bool listen();
    std::uint16_t port() const;
    void close();
};
```

- For each accepted connection it reads framed text messages and calls
  `RemoteServer::handle(msg, reply)` (async, posts to the server pool) — identical
  to how `QtWebSocketServer::onTextMessage` forwards ([backend.md](../spec/backend.md)).
- The `reply` callback writes back to the originating socket; if the socket is
  gone before the reply is ready, the reply is dropped (mirroring the Qt front's
  weak-`QPointer` behavior).
- It honours the same lifetime rule: it holds `RemoteServer& _server` by
  reference, so the server's owning `shared_ptr` must outlive it
  ([backend.md](../spec/backend.md)'s Lifetime & ownership).
- TLS and peer verification are the transport's responsibility, per
  [security.md](../spec/security.md); the reference front documents the same
  verify-the-peer default as [tls_peer_verification.md](tls_peer_verification.md)
  rather than shipping a `VerifyNone` example.

### Alternatively: a documented worked example

Per [todo.md](../todo.md), the deliverable may instead be a *documented, worked
example* of writing an `IBackend` (rather than a shipped `morph::net` component),
if the maintainers prefer to keep the core Qt-only-plus-simulated. Either way the
artifact is the same: a compiling, tested, Qt-free implementation of the
`IBackend`/`RemoteServer`-front contract that a host can copy. This spec specifies
the contract; the packaging (shipped header vs. `examples/`) is an implementation
decision recorded at build time.

## Non-goals

- **No change to `RemoteServer` or the wire.** The server is already
  transport-agnostic; this adds a *client backend* and a *server front*, both over
  the existing `Envelope`. `RemoteServer::handle`/`handleInline` are unchanged.
- **Not a replacement for the Qt transport.** Qt apps keep `QtWebSocketBackend`/
  `QtWebSocketServer`; this is an alternative for non-Qt hosts, not a deprecation.
- **Not a new protocol.** It speaks the same `wire::Envelope` (JSON, `kind`
  discriminator, `callId` correlation) so a non-Qt client and a Qt server (or vice
  versa) interoperate. Protocol evolution follows
  [protocol_versioning.md](protocol_versioning.md).
- **Not a full production HTTP server.** Like Qt's front, it is a thin transport;
  edge concerns (a hardened reverse proxy, WAF, global rate limits) stay upstream
  ([transport_limits.md](transport_limits.md)).
- **Does not affect local or simulated-remote mode.** `LocalBackend` and
  `SimulatedRemoteBackend` are unchanged.

## Testing (planned)

- A `SocketBackend` client against a `SocketServer` (non-Qt, both) completes
  `register` → `execute` → reply round-trips, with concurrent in-flight executes
  correctly matched by `callId`.
- Cross-transport interop: a `SocketBackend` client against a `QtWebSocketServer`,
  and a `QtWebSocketBackend` against a `SocketServer`, both work — same
  `Envelope`, so the transports are interchangeable.
- Disconnect mid-call resolves in-flight completions with `DisconnectedError` and
  wakes a parked synchronous `register` with `"disconnected"` (no hang) — the same
  guarantees `QtWebSocketBackend` gives.
- Lifetime: dropping the `RemoteServer` shared_ptr while a `SocketServer` still
  references it is the documented misuse (same rule as the Qt front); an in-flight
  `handle()` task stays safe via `shared_from_this()`.
- Callbacks marshal onto a `ThreadPoolExecutor` `cbExec` with no Qt present
  (proves the Qt-free path).

## Cross-references

- [backend.md](../spec/backend.md) — the full `IBackend` contract
  (`registerModel`/`registerModelWithContext`/`deregisterModel`/`execute`/
  `notifyBackendChanged`/`cancelPending`/`setReconnectHandler`) this implements,
  the `RemoteServer::handle`/`handleInline` forwarding it drives, the typed error
  hierarchy it reuses, and the `QtWebSocketBackend`/`QtWebSocketServer` behavior it
  mirrors without Qt.
- [wire.md](../spec/wire.md) — the `Envelope`, `kind` discriminator, and `callId`
  correlation both transports share, and `kMaxEnvelopeBytes`.
- [security.md](../spec/security.md) — transport confidentiality/peer
  authentication is the transport's job; the reference front must supply TLS.
- [tls_peer_verification.md](tls_peer_verification.md) — the verify-the-peer
  default the non-Qt front documents rather than an insecure example.
- [transport_limits.md](transport_limits.md) — the connection-level limits any
  transport (Qt or not) should carry; the reference front's `Config` mirrors them.
- [completion.md](../spec/completion.md) — `Completion`/`cbExec` callback delivery
  a non-Qt host wires to a `ThreadPoolExecutor` instead of `QtExecutor`.
