# Transport-level resource limits (planned)

> **Status: planned — not yet implemented.** This spec covers the
> denial-of-service bounds `morph` deliberately delegates to the transport today,
> and the design for the pieces that should ship with the Qt transport (and any
> future one). It extends [security.md](../spec/security.md) and [wire.md](../spec/wire.md). See
> [todo.md](../todo.md).

## The gap

`RemoteServer` is transport-agnostic and sees only decoded envelopes. The one
wire-layer bound that exists is `wire::kMaxEnvelopeBytes` (8 MiB per message,
checked before parsing — see [wire.md](../spec/wire.md)). Everything else is explicitly
the transport's job and, in the shipped Qt transport, **absent**:

- **No per-request timeout.** A slow or stalled dispatch leaves the client's
  `Completion` pending indefinitely; a hostile peer can open many calls and never
  let them resolve.
- **No per-connection rate limit.** A client can send unbounded messages as fast
  as it likes; each sub-cap message still costs a decode + dispatch.
- **No cap on models per connection.** `register` is unauthenticated
  ([instance_authorization.md](instance_authorization.md)) *and* unbounded — one
  connection can register models until memory is exhausted.
- **No connection count / accept cap.** `QtWebSocketServer` accepts every client.
- **No idle/handshake timeout.** A peer that connects and never speaks (or never
  finishes TLS) holds a slot.

`security.md`'s hardening checklist names all of these and says "the transport
must add its own size/rate bounds and timeouts." This spec makes that concrete:
a small, transport-agnostic policy the server can enforce, plus the specific
knobs the Qt transport should carry.

## Design

### A transport-agnostic limit policy

Resource limits split cleanly into two layers, matching where the information
lives:

1. **Per-message / per-dispatch limits** the `RemoteServer` can enforce because
   it sees every envelope and owns dispatch. These belong on `RemoteServer` as an
   optional `LimitPolicy`.
2. **Per-connection / per-socket limits** only the transport can enforce because
   `RemoteServer` has no concept of a connection (`handle()` takes a message and a
   reply callback, nothing more). These belong on the transport
   (`QtWebSocketServer`) as config.

#### `RemoteServer::LimitPolicy` (server-side, connection-agnostic)

```cpp
// namespace morph::backend
struct LimitPolicy {
    /// Max wall-clock a single execute may take before its Completion is
    /// resolved with a TimeoutError and the strand result (if it later arrives)
    /// is dropped. 0 = no timeout (today's behavior).
    std::chrono::milliseconds executeTimeout{0};

    /// Max models a single RemoteServer will hold live at once, across all
    /// callers. A register beyond this replies err "too many models".
    /// 0 = unbounded (today's behavior).
    std::size_t maxLiveModels{0};

    /// Max concurrent in-flight executes the server will accept before replying
    /// err "server busy" instead of dispatching. 0 = unbounded.
    std::size_t maxInFlightExecutes{0};
};
```

- Installed via `RemoteServer::setLimitPolicy(policy)` (thread-safe, like
  `setLogProvider`). Defaults to all-zero → **today's unbounded behavior**, so
  existing servers are unaffected.
- **`executeTimeout`** is enforced by arming a timer when `dispatchExecute` posts
  the strand task; if the timer fires first, the pending `Completion` is resolved
  with a new `TimeoutError` (a `std::runtime_error` sibling of
  `DisconnectedError`/`BackendChangedError`) and the eventual strand result is
  discarded via the existing idempotent `setValue`/`setException` no-op. The
  model still runs to completion on its strand (morph never interrupts a running
  `Model::execute` — see the non-goal below); the timeout bounds *the client's
  wait*, not the model.
- **`maxLiveModels`** is checked in the `register` path under `_regMtx` before
  constructing the instance; over the cap → `err "too many models"`.
- **`maxInFlightExecutes`** is checked when an `execute` arrives; over the cap →
  `err "server busy"`, no dispatch. A cheap atomic counter incremented on
  dispatch, decremented on reply.

#### Qt transport config (per-connection / per-socket)

`QtWebSocketServerConfig` (mirroring `QtWebSocketBackendConfig`'s
declaration-order rationale), applied by `QtWebSocketServer` to each accepted
socket:

| Field | Default | Meaning |
|---|---|---|
| `maxConnections` | `0` (unbounded) | Reject/close new accepts beyond this many live clients. |
| `maxMessageBytes` | `wire::kMaxEnvelopeBytes` | Per-frame cap enforced *before* handing to `RemoteServer::handle` (tighter than the 8 MiB wire cap if set lower). |
| `messagesPerSecond` | `0` (unbounded) | Token-bucket rate limit per connection; excess frames are dropped or the connection is closed. |
| `handshakeTimeout` | `10s` | Close a socket that connects but does not complete TLS / send a first frame in time. |
| `idleTimeout` | `0` (disabled) | Close a connection with no traffic for this long. |

These are pure `QWebSocket`/`QWebSocketServer`-level controls; they never reach
`RemoteServer`. All default to today's behavior (unbounded, standard Qt frame
limit) so enabling them is opt-in.

### Where each limit is enforced

| Limit | Layer | Mechanism |
|---|---|---|
| Message size (coarse) | wire | `kMaxEnvelopeBytes`, always on |
| Message size (tight) | transport | `QtWebSocketServerConfig::maxMessageBytes` before `handle()` |
| Per-request timeout | server | `LimitPolicy::executeTimeout` timer on the pending `Completion` |
| In-flight executes | server | `LimitPolicy::maxInFlightExecutes` atomic gate |
| Live models | server | `LimitPolicy::maxLiveModels` check in `register` |
| Connection count | transport | `QtWebSocketServerConfig::maxConnections` |
| Per-connection rate | transport | `messagesPerSecond` token bucket |
| Idle / handshake | transport | `idleTimeout` / `handshakeTimeout` |

The split is the point: `RemoteServer` limits are connection-blind and portable
across any transport; connection-shaped limits stay in the concrete transport
that actually owns sockets.

## Non-goals

- **No preemption of a running `Model::execute`.** `executeTimeout` bounds the
  *client's wait*, not the model's execution — morph never interrupts a task on a
  strand (there is no safe way to, and it would violate the single-threaded model
  contract). A model that can run unboundedly long must bound *itself*.
- **Not a replacement for an upstream WAF / reverse proxy.** For public exposure,
  a hardened proxy (rate limiting, connection management, request
  canonicalization for the duplicate-key caveat in [wire.md](../spec/wire.md)) is still
  recommended in front of `RemoteServer`; these knobs are defence-in-depth and
  the baseline for the shipped transport, not a full edge stack.
- **No global (cross-connection) rate limit in the transport.** `messagesPerSecond`
  is per-connection; a global budget across all clients, if needed, is a
  `LimitPolicy`-level or proxy-level concern, not per-socket.
- **Does not change local mode.** `LocalBackend` has no transport and no
  untrusted peer; none of these apply there.

## Testing (planned)

- With `executeTimeout` set, a deliberately slow model action resolves the
  client `Completion` with `TimeoutError` at the deadline; a later strand result
  is a no-op (idempotent completion).
- With `maxLiveModels`/`maxInFlightExecutes` set, registers/executes past the cap
  get `err "too many models"` / `err "server busy"` and do not dispatch; under
  the cap they behave normally.
- Qt transport: connections past `maxConnections` are refused; a frame larger
  than `maxMessageBytes` is rejected before `handle()`; a burst past
  `messagesPerSecond` is throttled; a silent socket is closed after
  `handshakeTimeout`/`idleTimeout`.
- All limits default off → behavior identical to today (regression guard).

## Cross-references

- [security.md](../spec/security.md) — the hardening checklist item ("Bound message size
  and add timeouts in the transport") this implements; the threat model that
  motivates it.
- [wire.md](../spec/wire.md) — `kMaxEnvelopeBytes`, the one always-on bound, and the
  `body` double-parse and duplicate-key caveats a front proxy still handles.
- [backend.md](../spec/backend.md) — `RemoteServer` register/execute paths where the
  server-side `LimitPolicy` checks live; `QtWebSocketServer` where the
  connection-level config applies; the `TimeoutError` joins the existing typed
  error hierarchy.
- [instance_authorization.md](instance_authorization.md) — `maxLiveModels`
  complements `authorizeRegister`: one bounds *how many* a caller may create, the
  other bounds *who* may create.
- [completion.md](../spec/completion.md) — the idempotent `setValue`/`setException` that
  makes a timeout-then-late-result safe.
