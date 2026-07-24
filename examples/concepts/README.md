# examples/concepts

Minimal, runnable examples of morph's production-hardening features — journal/
outbox, offline queue durability, server-side validation, register
authorization, transport limits, protocol versioning, connection scoping,
observability, and graceful shutdown. Each file is a small, self-contained
Catch2 test with heavily-commented golden-path usage; see `docs/spec/` for the
full design reference of each feature.

Start with `journal_and_outbox.cpp`: it explains (in a file-scope comment
above its model/action structs) why every file below registers its demo
types at file scope rather than in the anonymous namespace next to their
`InlineExecutor`/`CapturedReply` scaffolding — a detail the other files
don't repeat. The rest can be read in any order.

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
