# examples/concepts

Minimal, runnable examples of one morph feature each — the getting-started
seam itself, then the production-hardening features: journal/outbox, offline
queue durability, server-side validation, register authorization, transport
limits, protocol versioning, connection scoping, observability, and graceful
shutdown. Each file is a small, self-contained
Catch2 test with heavily-commented golden-path usage; see `docs/spec/` for the
full design reference of each feature.

Start with `getting_started.cpp` if you have never used morph: it is the
worked snippet [`docs/GETTING-STARTED.md`](../../docs/GETTING-STARTED.md)
walks through — a model, its registration, one call site, and the same call
site against a second backend that serialises through the wire protocol. It
lives here rather than only in that document so CI compiles and runs it; keep
the two in step.

Then `journal_and_outbox.cpp`: it explains (in a file-scope comment
above its model/action structs) why every file below registers its demo
types at file scope rather than in the anonymous namespace next to their
`InlineExecutor`/`CapturedReply` scaffolding — a detail the other files
don't repeat. The rest can be read in any order.

- `getting_started.cpp` — model, registration, `Completion<T>` on your
  executor, the identical call site local and remote, and
  `schemaJson<A>()`. See `docs/GETTING-STARTED.md`.
- `journal_and_outbox.cpp` — attaching an action log, idempotency-key dedup,
  the transactional outbox pattern. See `docs/spec/journal/journal.md`.
- `offline_queue_and_sync.cpp` — enqueue/drain/markDone, SyncWorker's retry
  budget and dead-letter sink, `FileOfflineQueue` durability. See
  `docs/spec/offline/offline.md`.
- `server_side_validation.cpp` — an action's `validate()` member rejected
  automatically on the server dispatch path. See
  `docs/spec/core/registry.md`.
- `register_authorization_and_ids.cpp` — a custom `IAuthorizer` gating who may
  register a model instance. See `docs/spec/session/session.md`.
- `transport_limits.cpp` — `LimitPolicy::maxLiveModels` capping live
  instances on a `RemoteServer`. See `docs/spec/core/backend.md`.
- `protocol_and_connections.cpp` — `hello`/`ProtocolRange` version
  negotiation and connection-scoped register/`closeConnection`. See
  `docs/spec/core/wire.md` and `docs/spec/core/backend.md`.
- `observability_and_shutdown.cpp` — a metric sink observing a
  register/execute cycle, and `beginShutdown()`/`drainedWithin()` draining a
  server cleanly. See `docs/spec/core/observability.md` and
  `docs/spec/core/backend.md`.
