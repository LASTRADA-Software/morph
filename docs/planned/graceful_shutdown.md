# Graceful shutdown & drain (planned)

> **Status: planned — not yet implemented.** This spec gives a server an
> orderly way to stop: refuse new work, finish what is in flight, tell clients
> properly, then tear down. It extends `RemoteServer` and `QtWebSocketServer`
> ([backend.md](../spec/core/backend.md)), addresses at the server layer the
> "no graceful drain / `waitIdle`" limitation
> [executor.md](../spec/core/executor.md) documents, and integrates with
> [observability.md](observability.md)'s readiness signal and
> [transport_limits.md](transport_limits.md)'s in-flight accounting. See
> [todo.md](../todo.md).

## The gap

Stopping a morph server today is abrupt at every layer:

- **The transport aborts.** `QtWebSocketServer::close()` `abort()`s every
  client socket immediately — no close frame, no chance for an in-flight
  execute's reply to be delivered. Clients discover the stop as a dead socket
  and fail their pending calls.
- **Nothing stops new work first.** There is no way to make `RemoteServer`
  refuse new `register`/`execute` envelopes while letting started work finish;
  work keeps arriving until the process is torn down under it.
  [executor.md](../spec/core/executor.md) is explicit that `~ThreadPoolExecutor`
  drains already-queued tasks but "tasks posted concurrently with or after
  destruction may be lost" and there is "no graceful drain / `waitIdle`" —
  the caller must synchronise teardown externally, and has no primitive to do
  it with.
- **Readiness never flips.** [observability.md](observability.md) plans a
  `HealthStatus`/readiness signal, but nothing transitions it during a deploy,
  so an orchestrator or load balancer keeps routing to a server that is about
  to vanish.

Every routine deploy pays for this: restarting a server mid-traffic drops
whatever was executing. As the durability track lands
([durable_queue.md](durable_queue.md), [outbox.md](outbox.md)) clean drains
matter more, not less — a drained stop is the cheap way to keep queues and
logs boring.

## Design

### `RemoteServer::beginShutdown()` and `drainedWithin()` (NEW)

```cpp
// namespace morph::backend — NEW on RemoteServer.

/// Enter shutdown: from now on, `register` and `execute` envelopes are
/// rejected with err "server shutting down"; `deregister` is still served so
/// clients can tear down cleanly. Idempotent. There is no way back — a
/// restarted service constructs a fresh RemoteServer.
void beginShutdown();

/// Block until every in-flight execute has delivered its reply, or the
/// deadline expires. Returns true if drained, false on timeout.
[[nodiscard]] bool drainedWithin(std::chrono::milliseconds deadline);
```

- The rejection string, `"server shutting down"`, is canonical protocol text
  (a [drift_guard.md](drift_guard.md) pinnable, like `"model not found"`).
  Clients see it through the normal `Completion` error path — an ordinary
  fast failure, not a hang.
- **One in-flight counter, three consumers.** The counter incremented when an
  execute is accepted for dispatch and decremented when its reply is
  delivered is the same state
  [transport_limits.md](transport_limits.md)'s `maxInFlightExecutes` gates
  and [observability.md](observability.md)'s in-flight gauge reads — shared,
  never double-counted. `drainedWithin` waits on a condition variable
  signalled when it reaches zero.
- Once drained, the existing teardown rules
  ([concurrency_and_lifetimes.md](../spec/concurrency_and_lifetimes.md))
  apply unchanged — but now trivially, because every queue is empty. This
  spec deliberately adds **no** `waitIdle` to `IExecutor`
  ([executor.md](../spec/core/executor.md)'s limitation stands for raw executor
  users): the drain condition morph can define precisely — "every accepted
  execute has replied" — lives at the server layer, where the work is
  counted.
- Readiness integration: `beginShutdown()` flips
  [observability.md](observability.md)'s `HealthStatus.ready` to `false` and
  fires its state-change callback (once that seam lands), so the orchestrator
  stops routing while the drain runs — the standard unready → drain → stop
  sequence.

### `QtWebSocketServer::closeGracefully(deadline)` (NEW)

The transport counterpart sequences the stop end to end:

1. **Stop accepting** new connections (`QWebSocketServer::pauseAccepting()`).
2. **`beginShutdown()`** on the `RemoteServer` — new work now fails fast.
3. **`drainedWithin(deadline)`** — in-flight replies are delivered over the
   still-open sockets.
4. **Close properly:** each client socket gets a real WebSocket close frame
   (`CloseCodeGoingAway`, reason `"server shutting down"`) instead of
   `abort()`, so clients distinguish an orderly stop from a crash.
5. **Hard stop** for stragglers: after the deadline (drained or not), the
   existing `close()` path runs as today. With
   [connection_scoped_cleanup.md](connection_scoped_cleanup.md) in place,
   each connection's scope is reclaimed here as well — the two specs compose.

The existing abrupt `close()` remains unchanged for tests and emergencies;
`closeGracefully` is additive and opt-in, per the house rule.

### What clients experience

- In-flight calls complete normally during the drain window.
- New calls fail fast with `"server shutting down"` — for hosts using the
  offline stack, an ordinary failure the queue retries after the restart
  ([offline.md](../spec/offline/offline.md), [durable_queue.md](durable_queue.md)).
- The socket then closes with `going away` rather than dying. The Qt client
  backend's auto-reconnect behaves per its existing config
  ([backend.md](../spec/core/backend.md)) and finds the restarted server;
  differentiated client handling of the close reason is a possible later
  refinement, not part of this spec.

## Non-goals

- **No preemption of a running `Model::execute`.** Same stance as
  [transport_limits.md](transport_limits.md): morph never interrupts a strand
  task. A model that can run unboundedly long bounds itself; the deadline
  bounds the *wait*, after which the hard stop proceeds.
- **No un-shutdown.** `beginShutdown()` is one-way; a restarted service is a
  new `RemoteServer`. Pausing/resuming acceptance without teardown is a
  different feature.
- **No load-balancer protocol.** The readiness flip is the integration point;
  connection draining at the LB, DNS, or mesh layer is the deployment's job.
- **Not crash safety.** A power cut still interrupts mid-flight work — that
  is what the durability track ([outbox.md](outbox.md),
  [journal.md](../spec/journal/journal.md)) exists for. This spec makes *intentional*
  stops clean, nothing more.
- **Does not change local mode.** `LocalBackend` lives and dies with the
  application; there is no server to drain.

## Testing (planned)

- After `beginShutdown()`: `execute`/`register` are rejected with the
  canonical string; `deregister` still succeeds; an in-flight execute
  completes and its reply is delivered.
- `drainedWithin` returns `true` promptly once replies are out, `false` when
  a deliberately slow model overruns the deadline (and the hard stop still
  works after it).
- `closeGracefully`: clients receive a `going away` close frame after the
  drain, not an abort; with a slow model, the hard stop fires at the
  deadline.
- Readiness (once [observability.md](observability.md) lands): `ready` is
  observed `false` for the whole drain window.
- Regression: a server that never calls the new APIs behaves byte-for-byte as
  today, including the abrupt `close()`.

## Cross-references

- [backend.md](../spec/core/backend.md) — `RemoteServer` dispatch and reply paths;
  `QtWebSocketServer::close()`, today's abrupt stop.
- [executor.md](../spec/core/executor.md) — the drain-on-destruction semantics and
  the "no graceful drain / `waitIdle`" limitation this addresses one layer
  up.
- [concurrency_and_lifetimes.md](../spec/concurrency_and_lifetimes.md) — the
  teardown-ordering rules a drained stop satisfies trivially.
- [observability.md](observability.md) — `HealthStatus`/readiness, flipped by
  `beginShutdown()`.
- [transport_limits.md](transport_limits.md) — `maxInFlightExecutes`, the
  other consumer of the shared in-flight counter; the no-preemption stance.
- [connection_scoped_cleanup.md](connection_scoped_cleanup.md) — scope
  reclamation at the hard-stop step.
- [durable_queue.md](durable_queue.md) / [outbox.md](outbox.md) — why drained
  stops keep the durability story boring.
- [todo.md](../todo.md) — roadmap placement (§C operational readiness).
