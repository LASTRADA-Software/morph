# Connection-scoped model cleanup (planned)

> **Status: planned — not yet implemented.** This spec closes the "no
> connection-scoped cleanup — orphaned models leak" limitation documented in
> [backend.md](../spec/core/backend.md): models registered over a connection that
> dies stay registered on the server forever. It extends `RemoteServer` and
> `QtWebSocketServer` ([backend.md](../spec/core/backend.md)) with an **opt-in
> connection scope**, and complements
> [transport_limits.md](transport_limits.md) and
> [instance_authorization.md](instance_authorization.md). See
> [todo.md](../todo.md).

## The gap

`RemoteServer` is connection-blind by design: `handle()` takes a message and a
reply callback, nothing more ([backend.md](../spec/core/backend.md),
[transport_limits.md](transport_limits.md)). Models it creates on `register`
live in `_models` until an explicit `deregister` arrives. Nothing ever arrives
for a connection that dies:

- **The transport forgets, the server keeps.** `QtWebSocketServer`'s
  disconnect handler removes the socket from its client list and nothing else;
  no `deregister` is synthesised, so every model the connection registered
  stays live until process exit.
- **The client cannot reliably clean up either.** `QtWebSocketBackend`'s
  `deregisterModel` is deliberately fire-and-forget (no nested event loop in a
  destructor — [backend.md](../spec/core/backend.md) design decisions), and a
  crashed or power-cut client never sends anything at all. The header itself
  warns: "an undelivered or lost `deregister` leaves the model registered on
  the server indefinitely."
- **This is a normal-operation leak, not just an adversarial one.** Every
  client crash, laptop sleep, or network drop strands its instances. Under
  [transport_limits.md](transport_limits.md)'s `maxLiveModels` the leak
  converts into an availability failure: dead connections consume the budget
  until every new `register` is denied — a crash-looping client exhausts the
  cap all by itself. [instance_authorization.md](instance_authorization.md)
  bounds *who* may create instances, not *how long* they live.

The client side already treats model ids as connection-scoped: on disconnect
`QtWebSocketBackend` cancels its pending calls, and after a reconnect the
`Bridge` re-registers every live handler through the reconnect handler
([backend.md](../spec/core/backend.md)) — fresh registrations, fresh ids. Old ids
are never reused by a correct client. Only the server side pretends they are
still meaningful.

## Design

### Connection scopes on `RemoteServer` (NEW)

```cpp
// namespace morph::backend — NEW on RemoteServer.
using ConnectionId = std::uint64_t;   // 0 is reserved: "unscoped" (today's behavior)

/// Transport calls this when it accepts a connection. Returns a fresh scope id.
ConnectionId openConnection();

/// Transport calls this when the connection is gone (disconnect, close, error).
/// Destroys every model still registered under the scope. Idempotent; an
/// unknown or already-closed id is a no-op.
void closeConnection(ConnectionId cid);

/// NEW overload: like handle(msg, reply), additionally attributing any
/// `register` in @p msg to the scope @p cid.
void handle(std::string msg, std::function<void(std::string)> reply, ConnectionId cid);
```

- A `register` handled under a non-zero `cid` records the new `ModelId` in
  that connection's scope set; an explicit wire `deregister` removes it again.
  The bookkeeping lives next to `_models`/`_owners` under the same `_regMtx`
  (a `cid → set<ModelId>` map plus the owning `cid` stored per instance), so
  scope membership can never desync from instance existence.
- `closeConnection(cid)` erases every surviving scoped instance exactly as the
  `deregister` path does (`_models` + `_owners` + scope entry), then drops the
  scope. What it does **not** do is consult the authorizer — see below.
- The existing two-argument `handle()` and `handleInline()` stay **unscoped**
  (`cid == 0`): nothing is recorded and nothing is ever cleaned up — byte-for-
  byte today's behavior. `SimulatedRemoteBackend` keeps using the unscoped
  path (its "connection" is the process itself). Scoping is strictly opt-in,
  per the house rule that hardening features default to current behavior.

### Why cleanup is not a synthesised wire `deregister`

The tempting alternative — the transport fabricates `deregister` envelopes on
disconnect — is wrong twice:

1. **It breaks under instance authorization.** The `deregister` path runs
   `IAuthorizer::authorizeInstance` with the envelope's session. A synthesised
   envelope has no token, so an ownership-enforcing authorizer
   ([instance_authorization.md](instance_authorization.md)) would reject the
   cleanup *precisely when hardening is enabled*. Cleanup must not impersonate
   a caller; it is the server's own housekeeping.
2. **The transport would have to learn the ids.** It would need to parse every
   `register` reply to discover which `modelId`s it owns — duplicated,
   desync-prone bookkeeping. Recording the id at the moment the server creates
   the instance is simpler and cannot drift.

`closeConnection` therefore bypasses `IAuthorizer` by design. This is not an
authorization hole: only in-process transport code can call it, and the
transport is already inside the server's trust domain (it hands the server
every envelope; see [security.md](../spec/security.md) on the trust model).

### Qt transport wiring

`QtWebSocketServer` opts in end to end:

- **Accept** (`onNewConnection`): `cid = _server.openConnection()`, stored in
  the per-socket map alongside the `QWebSocket*`.
- **Message** (`onTextMessage`): forward via the scoped `handle(msg, reply,
  cid)` overload instead of the two-argument one.
- **Disconnect** (`onDisconnected`): `_server.closeConnection(cid)` after
  removing the socket — the step that is missing today.
- **Shutdown** (`close()`): `closeConnection` for every live client before
  aborting its socket, so an orderly server stop also reclaims instances.

### Safety of cleanup while a model is executing

`dispatchExecute` copies the instance's `shared_ptr<IModelHolder>` into the
strand task before it runs ([concurrency_and_lifetimes.md](../spec/concurrency_and_lifetimes.md)).
`closeConnection` only erases the map's reference; an in-flight execute keeps
the holder alive until its task completes, and its reply is then delivered to
a dead socket and dropped by the existing `QPointer` guard in the Qt server's
reply path. Cleanup therefore never races an execution into use-after-free —
it merely prevents *new* lookups, exactly like an explicit `deregister`.

### Interactions

- **[transport_limits.md](transport_limits.md):** `maxLiveModels` counts
  `_models`; cleanup shrinks it, so the cap bounds *live* usage instead of
  accumulated history. `idleTimeout` composes: an idle disconnect now also
  reclaims the connection's instances.
- **[instance_authorization.md](instance_authorization.md):** opaque id
  generation is orthogonal; the recorded owner is erased with the instance.
- **`RemoteServer::LogProvider` logs ([journal.md](../spec/journal/journal.md)):**
  erasing the holder releases its reference to the attached `IActionLog`; the
  log object itself is shared and host-owned, so recorded history survives —
  consistent with "entries are never removed by the framework".
- **[observability.md](observability.md):** `closeConnection` is a natural
  metric hook (instances reclaimed per disconnect) once the metrics seam
  lands.

## Non-goals

- **No session resumption.** Model ids stay connection-scoped; a reconnecting
  client re-registers and gets fresh ids — that is the *existing* contract
  (`Bridge` re-registration on reconnect, [backend.md](../spec/core/backend.md)),
  which this spec makes the server honour rather than changes. A host that
  wants ids to survive reconnects must keep using the unscoped path.
- **No lease/TTL expiry.** Unscoped registrations (embedded hosts,
  `SimulatedRemoteBackend`) keep living until explicit deregistration;
  [transport_limits.md](transport_limits.md)'s `maxLiveModels` remains their
  backstop. A time-based lease is a different, heavier mechanism this spec
  deliberately avoids.
- **No client-side change.** `QtWebSocketBackend` is untouched; fire-and-forget
  `deregister` stays. Its lost-message case is healed at the next disconnect
  (the leak becomes bounded by connection lifetime instead of process
  lifetime).
- **Not distributed state.** One `RemoteServer`, its own instances; nothing
  here coordinates across processes.

## Testing (planned)

- Register N models over a scoped connection, drop the socket: the server
  holds zero of them afterwards; an `execute` against an old id gets
  `err "model not found"`.
- The unscoped `handle()` path never cleans up (regression guard: today's
  embeddings unaffected).
- Explicit `deregister` followed by disconnect: idempotent, no double-erase.
- In-flight execute across a disconnect: the strand task completes, the reply
  is dropped, no crash or leak; the instance is gone afterwards.
- With an ownership-enforcing `authorizeInstance` installed: a foreign wire
  `deregister` is still rejected, while `closeConnection` reclaims the same
  instance (the bypass is deliberate and tested).
- With `maxLiveModels` set (once [transport_limits.md](transport_limits.md)
  lands): fill the cap from connection A, disconnect A, and connection B can
  register again — the budget is freed.
- `QtWebSocketServer::close()` reclaims every client's scope.

## Cross-references

- [backend.md](../spec/core/backend.md) — the documented limitation this closes;
  `RemoteServer` register/deregister/execute paths; the fire-and-forget
  `deregisterModel` design decision; the reconnect-handler re-registration
  contract the cleanup relies on.
- [security.md](../spec/security.md) — the trust model that makes an
  in-process, non-impersonating cleanup path sound.
- [instance_authorization.md](instance_authorization.md) — ownership
  enforcement on *wire* deregisters, which synthesised-envelope cleanup would
  have collided with.
- [transport_limits.md](transport_limits.md) — `maxLiveModels`/`idleTimeout`,
  the caps this keeps meaningful.
- [concurrency_and_lifetimes.md](../spec/concurrency_and_lifetimes.md) — the
  strand task's `shared_ptr` capture that makes erase-during-execute safe.
- [observability.md](observability.md) — the metrics seam a cleanup event can
  feed.
- [todo.md](../todo.md) — roadmap placement (§A remote-mode hardening).
