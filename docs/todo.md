# TODO — production-hardening checklist

Tracking list for design-approved but not-yet-implemented work, framed around
**what a production deployment needs**. Planned specs live under `docs/planned/`;
the authoritative current-state specs stay under `docs/spec/`. Items link to their
planned spec where one exists.

Readiness depends on deployment mode:

- **Local mode** (models in-process, no network): near production-ready. Only the
  operational items (§C) and a couple of §B robustness items apply.
- **Remote mode** (`RemoteServer` over a network): needs the §A hardening
  milestone before public/multi-tenant exposure. None are architecturally
  blocked — the seams exist — but they are real work, not configuration.

Legend: **[spec]** = full design spec exists (implement against it, then flip its
*Status* banner and rewrite to present tense per `CLAUDE.md`). **[design-needed]**
= agreed direction, no spec yet. Priority: **P0** must-have before that mode ships,
**P1** strongly recommended, **P2** nice-to-have.

---

## A. Remote-mode hardening (do before networked/public/multi-tenant use)

### A1 — Server-side action validation · P0 · [spec: `planned/validation.md`]
Run `ActionValidator::ready` (+ declared-precision reconciliation) inside the
dispatcher runner and `Bridge::executeVia`'s `localOp`, before `Model::execute`.
A hand-built envelope currently reaches the model unvalidated. Add
`ValidationError`; reject as `err`/`onError`. Backward compatible.
*Touches:* `registry.hpp`, `bridge.hpp`, `remote.hpp`.

### A2 — Register authorization & opaque model ids · P0 · [spec: `planned/instance_authorization.md`]
`register`/`deregister` are unauthenticated and model ids are sequential/guessable.
Add `IAuthorizer::authorizeRegister` (default allow-all) enforced in `RemoteServer`;
replace `_nextId` with opaque (random, non-sequential) id generation. Both opt-in.
*Touches:* `session.hpp`, `remote.hpp`.

### A3 — Transport-level resource limits · P0 · [spec: `planned/transport_limits.md`]
No per-request timeout, no rate limit, no per-connection model cap, no connection
cap — only the 8 MiB message-size bound. Add a server-side `LimitPolicy`
(execute timeout, max live models, max in-flight) and Qt-transport per-connection
config (max connections, rate, idle/handshake timeouts). All default off.
*Touches:* `remote.hpp`, `qt/qt_websocket_server.hpp`; new `TimeoutError`.

### A4 — TLS + peer verification as the enforced default · P0 · [spec: `planned/tls_peer_verification.md`]
The shipped Qt client documents `QSslSocket::VerifyNone` for self-signed certs,
which encrypts but does not authenticate the server (MITM-vulnerable). Bearer
tokens are not bound to a connection, so without real TLS a stolen token replays.
*Work:* make peer verification the documented/default production path (CA or
pinned cert), add an example, and a startup guard/warning when a server is
exposed beyond loopback without TLS. *Touches:* `qt/qt_websocket_*`, `security.md`.

### A5 — Inject a vetted HMAC for production · P1 · [spec: `planned/vetted_hmac.md`]
The reference `hmacSha256` is correct but not side-channel-hardened beyond a
constant-time compare. Production should inject a vetted library (libsodium,
OpenSSL) via the existing `MacFunction` seam. *Work:* document the recommended
wiring, provide an example adapter, and consider a build option that fails if the
reference impl is used in a release/production configuration. *Touches:*
`session_auth.hpp` docs, `security.md`, examples.

### A6 — Protocol / action-schema versioning · P1 · [spec: `planned/protocol_versioning.md`]
Unknown envelope/token keys are now ignored (forward-compatible), but there is no
negotiated protocol version and no migration story for evolving action structs
across client/server versions. *Work:* a version field + negotiation on connect,
and a documented action-evolution policy (additive-only, deprecation window).
*Touches:* `wire.md`, `security.md`, new spec.

---

## B. Durability & data-integrity (both modes, if you persist)

### B1 — Durable offline queue & cross-restart dead-lettering · P1 · [spec: `planned/durable_queue.md`]
`SyncWorker`'s retry counter is in-memory (poison items never dead-letter across
restarts); dead-lettering is log-only. Add durable `QueueItem::attempts` +
`setAttempts` hook and an optional `DeadLetterSink`. No durable queue impl ships
(fields/hooks only — a SQL/file `IOfflineQueue` is still the host's to write).
*Touches:* `offline_queue.hpp`, `sync_worker.hpp`.

### B2 — Transactional outbox (journal + store atomicity) · P1 · [spec: `planned/outbox.md`]
The action log and a model's own store commit as two independent writes and can
diverge on a crash (`examples/bank` shows it). Add an opt-out from the automatic
append and an `OutboxRelay` seam so a store-backed model logs in its own
transaction. Reuses the idempotency-key dedup contract (align with B1).
*Touches:* `journal` headers, `registry.hpp` (auto-append suppression).

### B3 — A shipped durable `IOfflineQueue` implementation · P2 · [spec: `planned/durable_offline_queue_impl.md`]
Only `InMemoryOfflineQueue` ships (loses everything on exit). A reference
SQLite/file-backed queue that persists payload + `idempotencyKey` + `attempts`
across restarts would make B1/B2 usable without every host re-writing it.
*Touches:* new header/example.

---

## C. Operational readiness (needed for any production, esp. remote)

### C1 — Observability: metrics, tracing, health · P1 · [spec: `planned/observability.md`]
Logging is a replaceable sink, but there are no metrics (dispatch latency,
in-flight, queue depth, reconnect counts), no tracing hooks, and no health/readiness
signal. *Work:* a lightweight metrics/trace seam (injectable, like the logger) and
a server health endpoint/callback. *Touches:* new spec, `remote.hpp`, `logger.hpp`.

### C2 — Non-Qt transport option · P2 · [spec: `planned/non_qt_transport.md`]
The only network transport is `QtWebSocketBackend/Server` — single-threaded,
Qt-event-loop-bound. Shops that don't want Qt on the server must implement
`IBackend` + wire it to `RemoteServer` themselves. *Work:* a reference
transport-agnostic (or plain-socket / HTTP) transport, or a documented, worked
example of writing one. *Touches:* new example, `backend.md`.

### C3 — Load / soak / fuzz testing · P1 · [spec: `planned/testing_strategy.md`]
Unit coverage is strong (2734 assertions), but there is no evidence of load, soak,
or fuzz testing, or an adversarial cross-process run. *Work:* a fuzz harness over
`wire::decode`/`dispatchExecute`, a soak test for reconnect/switchBackend churn,
and a throughput/latency benchmark. Gates confidence in A1–A3.

### C4 — Compile-time `onBackendChanged` dispatch · P2 · [spec: `planned/backend_changed_dispatch.md`]
`LocalBackend::notifyBackendChanged` `dynamic_cast`s every live model under the
registry lock (RTTI dependency, O(all models)). Capture backend-change-awareness
at registration and drive from a maintained set. Pure internal refactor,
behavior-preserving. *Touches:* `model.hpp`, `backend.hpp`.

---

## D. Process / project

### D1 — Spec ↔ code drift guard (CI) · P1 · [spec: `planned/drift_guard.md`]
The recurring audit finding was header docs/specs disagreeing with code (the
`authenticate` principal-clearing lie, the false "unknown keys ignored" claim,
the `runFor` comment, a stale `AuthError` cardinality). Add a CI check pinning the
mechanical facts: enum cardinalities, key constants (`kMaxDecimalPlaces`,
`kMaxEnvelopeBytes`), canonical error-message strings, and glaze
`error_on_unknown_keys` behavior — so future drift fails the build.

### D2 — API stability / 1.0 commitment · P2 · [spec: `planned/api_stability.md`]
The API is still being corrected (this branch is `fix/spec-audit-remediation`).
Before production adoption at scale, declare a supported version, a deprecation
policy, and ABI/source-compat expectations (header-only eases ABI but not source).

---

## E. GUI enhancement program (rapid + flexible GUI development)

A layered program to generate GUIs from the user's model + action types with
minimal declaration, while keeping the result flexible. Umbrella spec:
[gui_overview.md](planned/gui_overview.md) (principle: *infer by default, declare
to override*; all new `x-*` keys are additive/unversioned; Qt/QML is the
reference renderer, the schema contract stays renderer-agnostic).

### Tier 1 — richer forms (additive metadata/logic on the single-action form)

- **E-G1 — Field metadata** · P1 · [spec: `planned/gui_field_metadata.md`] —
  labels, help, placeholder, read-only, hidden.
- **E-G2 — Layout & grouping** · P1 · [spec: `planned/gui_layout_grouping.md`] —
  sections, tabs, accordions, column spans.
- **E-G3 — Widget hints** · P1 · [spec: `planned/gui_widget_hints.md`] — control
  selection (multiline, slider, radio vs combo), type-derived where possible.
- **E-G4 — Cross-field rules** · P1 · [spec: `planned/gui_cross_field_rules.md`] —
  typed rule vocabulary evaluated on client **and** server; shares one
  declaration with [validation.md](planned/validation.md).
- **E-G5 — Computed fields** · P2 · [spec: `planned/gui_computed_fields.md`] —
  derived read-only fields, recomputed live client-side, authoritative server-side.
- **E-G6 — Dependent choices** · P2 · [spec: `planned/gui_dependent_choices.md`] —
  `Choice` options parameterised by sibling field values (cascading picklists).

### Tier 2 — app generation (a view/app schema layer above the action schema)

- **E-G7 — Collections & views** · P2 · [spec: `planned/gui_collections_views.md`] —
  list/table + master-detail from query+edit+delete action sets.
- **E-G8 — Workflows & navigation** · P2 · [spec: `planned/gui_workflows_navigation.md`] —
  multi-step wizards (shared draft across actions) + app-shell/route descriptor.

### Ecosystem

- **E-G9 — Renderer toolkit** · P1 · [spec: `planned/gui_renderer_toolkit.md`] —
  reusable Qt/QML reference renderer, a renderer conformance test kit, and
  per-field widget-override / theming slots.

### Execution order (GUI program)

Ordered so each step lands on a stable base and de-risks the next. Steps within a
wave are independent and can be done in parallel.

| Wave | Items | Rationale |
|---|---|---|
| **0 — Foundation** | A1 (server-side validation) | E-G4's rules reuse the server-side validator; land it first so rules have a server evaluator to plug into. Not a GUI item, but the GUI program's prerequisite. |
| **1 — Presentation** | E-G1, E-G2, E-G3 | Pure additive `x-*` metadata; biggest "looks bespoke" ROI, lowest risk, no new logic. Do first and in parallel. |
| **2 — Renderer toolkit (start)** | E-G9 (reference renderer + conformance kit) | Stand up the reusable QML renderer + conformance corpus against Wave-1 keys, so every later key has a renderer that proves it and a test that pins it. Theming/slots can trail. |
| **3 — Form logic** | E-G4, then E-G5, E-G6 | E-G4 first (single rule source → schema + client + server, on top of Wave-0). E-G5 and E-G6 build on the reactive path and can follow in parallel once E-G4's rule/annotation plumbing exists. |
| **4 — App generation** | E-G7, then E-G8 | The view/app-schema layer. E-G7 (lists/master-detail) first — E-G8's wizards/navigation compose G7 screens and Tier-1 forms, so it comes last. |

Rule of thumb: **Waves 0–2 make single-action forms production-grade; Waves 3–4
turn the form generator into an app generator.** A team wanting quick wins can
stop after Wave 2 and still have a dramatically better form-building story.

## Fast reference — minimum bars

- **Local, trusted, in-process:** ship now + C1 (observability) + C3 (soak) for
  confidence. Consider B1/B2 if you persist.
- **Remote, internal/trusted network:** A1 + A2 + A3 + A4 at minimum; B1/B2 if you
  persist; C1 + C3.
- **Remote, public / multi-tenant:** all of §A, all of §B if persisting, all of
  §C, D1. Treat everything in §A as P0.

## Notes

- Every item is opt-in or backward compatible by default — none change existing
  behavior unless enabled. §A–§D items can largely land independently (mind the
  B1→B2 idempotency-key dependency and the A1→E-G4 dependency). The **§E GUI
  program has a recommended execution order** — see
  [Execution order (GUI program)](#execution-order-gui-program).
- Specs marked **[spec]** carry a `Status: planned` banner; implement against the
  spec, verify, then rewrite it to present tense and update `ARCHITECTURE.md`.
- All planned specs live in `docs/planned/`; the authoritative current-state
  specs stay in `docs/spec/`. When a planned item ships, move nothing — just flip
  its spec's banner to present tense (it stays a `docs/spec/` reference only if it
  documents a public type; otherwise it can remain under `planned/` as history or
  be folded into the relevant `spec/` file).
