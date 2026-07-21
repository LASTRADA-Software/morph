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

### A2 — Register authorization & opaque model ids · P0 · **Implemented** — see `security.md` ("The register-authorization hook", "Opaque model ids") and `core/backend.md`.

### A3 — Transport-level resource limits · P0 · shipped — folded into [`spec/core/backend.md`](spec/core/backend.md#limitpolicy--opt-in-resource-limits)
`RemoteServer::LimitPolicy` (execute timeout, max live models, max in-flight) and
`QtWebSocketServerConfig` (max connections, per-frame size, per-connection rate,
idle/handshake timeouts) are implemented, all opt-in/default-off. See
`spec/security.md`'s hardening checklist for the deployment recommendation.

### A4 — TLS + peer verification as the enforced default · P0 · shipped — folded into [`spec/security.md`](spec/security.md#transport-security-the-qt-websocket-transport) and [`spec/core/backend.md`](spec/core/backend.md#qtwebsocketserver--server-side-websocket-transport)
`qt_tls.hpp` ships `tlsVerifyingConfig()`/`tlsPinnedConfig()`/`tlsInsecureNoVerify()`
as the documented client-side path, `examples/qt_tls_client/` demonstrates pinned-cert
acceptance and MITM rejection end to end, and `QtWebSocketServerConfig::bindAddress`/
`allowPlaintextExposure` make `QtWebSocketServer::listen()` refuse a silent
non-loopback plaintext bind unless explicitly overridden.

### A5 — Inject a vetted HMAC for production · P1 · DONE
Shipped: `examples/vetted_hmac/` (libsodium and OpenSSL `MacFunction`
adapters, each with a known-answer + interop test) and the opt-in
`MORPH_REQUIRE_VETTED_HMAC` build option, which drops the `mac = hmacSha256`
default argument on `TokenIssuer`/`TokenVerifier`/`SigningAuthorizer` so a
build relying on it fails to compile. See `docs/spec/security.md`,
"MAC-primitive recommended wiring".

### A7 — Connection-scoped model cleanup · P0 · shipped — folded into [`spec/core/backend.md`](spec/core/backend.md#connection-scopes)
`RemoteServer` has an opt-in `ConnectionId` scope (`openConnection`/
`closeConnection` + a scoped `handle(msg, reply, cid)` overload), and
`QtWebSocketServer` opts every client into it end to end, so a client crash or
dropped socket now reclaims its models instead of stranding them until process
exit. `closeConnection` is server housekeeping — it bypasses `IAuthorizer` by
design, not a synthesized wire `deregister`.

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

### C2 — Non-Qt transport option · DONE · [spec: `spec/core/backend.md`]
Shipped as `morph::net` (`SocketBackend`/`SocketServer`, opt-in via
`MORPH_BUILD_NET`) — a raw-socket RFC 6455 WebSocket transport with no Qt
dependency, wire-interoperable with `QtWebSocketBackend`/`QtWebSocketServer`.
See `docs/spec/core/backend.md`'s `SocketBackend`/`SocketServer` section.

### C3 — Load / soak / fuzz testing · P1 · shipped — folded into [`spec/testing_strategy.md`](spec/testing_strategy.md)
`fuzz_wire_decode`/`fuzz_dispatch_execute` (`MORPH_BUILD_FUZZERS=ON`), the
switchBackend/reconnect soak tests and the throughput/latency benchmark
(`MORPH_BUILD_LOAD_TESTS=ON`), and the adversarial cross-socket run
(`MORPH_BUILD_QT=ON`) are implemented, all opt-in/default-off. See
`spec/testing_strategy.md` for what each proves and how to scale/run them.

### C4 — Compile-time `onBackendChanged` dispatch · P2 · [spec: `planned/backend_changed_dispatch.md`]
`LocalBackend::notifyBackendChanged` `dynamic_cast`s every live model under the
registry lock (RTTI dependency, O(all models)). Capture backend-change-awareness
at registration and drive from a maintained set. Pure internal refactor,
behavior-preserving. *Touches:* `model.hpp`, `backend.hpp`.

### C5 — Graceful shutdown & drain · P1 · shipped — folded into [`spec/core/backend.md`](spec/core/backend.md#graceful-shutdown-beginshutdown--drainedwithin)
`RemoteServer::beginShutdown()` + `drainedWithin(deadline)` (reject new
`register`/`execute` with `err "server shutting down"`, drain the shared
in-flight counter, flip `health().ready` to `false`) and
`QtWebSocketServer::closeGracefully(deadline)` (close frames, then hard stop)
are implemented, opt-in/default-off — a server that never calls either
behaves byte-for-byte as before.

---

## D. Process / project

### D1 — Spec ↔ code drift guard (CI) · P1 · DONE — see [CONTRIBUTING.md, "Quality gates"](../CONTRIBUTING.md#quality-gates)
The recurring audit finding was header docs/specs disagreeing with code (the
`authenticate` principal-clearing lie, the false "unknown keys ignored" claim,
the `runFor` comment, a stale `AuthError` cardinality). Landed: a CI check
pinning the mechanical facts (`docs/spec/pinned_facts.toml`,
`tests/test_pinned_facts.cpp`, `scripts/check_spec_citations.sh`) — enum
cardinalities, key constants (`kMaxDecimalPlaces`, `kMaxEnvelopeBytes`,
`kClockSkewMs`), canonical error-message strings, and glaze
`error_on_unknown_keys` behavior — so future drift fails the build.

### D2 — API stability / 1.0 commitment · P2 · **Implemented** — see `docs/spec/VERSIONING.md`.

---

## E. GUI enhancement program (rapid + flexible GUI development)

A layered program to generate GUIs from the user's model + action types with
minimal declaration, while keeping the result flexible. Umbrella spec:
[gui_overview.md](planned/gui_overview.md) (principle: *infer by default, declare
to override*; all new `x-*` keys are additive/unversioned; Qt/QML is the
reference renderer, the schema contract stays renderer-agnostic).

### Tier 1 — richer forms (additive metadata/logic on the single-action form)

- **E-G1 — Field metadata** · P1 · **Implemented** — see
  `spec/forms/forms.md` ("Field metadata — `FieldMeta`").
- **E-G2 — Layout & grouping** · P1 · [spec: `planned/gui_layout_grouping.md`] —
  sections, tabs, accordions, column spans.
- **E-G3 — Widget hints** · P1 · [spec: `planned/gui_widget_hints.md`] — control
  selection (multiline, slider, radio vs combo), type-derived where possible.
- **E-G4 — Cross-field rules** · P1 · [spec: `spec/forms/forms.md`] —
  typed rule vocabulary evaluated on client **and** server; shares one
  declaration with [the server-side validator](spec/core/registry.md).
- **E-G5 — Computed fields** · P2 · [spec: `spec/forms/forms.md`] —
  derived read-only fields, recomputed live client-side, authoritative server-side.
- **E-G6 — Dependent choices** · P2 · [spec: `spec/forms/choice.md`] —
  `Choice` options parameterised by sibling field values (cascading picklists).
- **E-G10 — Localisation (i18n)** · P1 · [spec: `planned/gui_i18n.md`] —
  translated labels/help/rule messages via schema-derived stable message keys
  and a renderer-side catalog seam; locale formatting duties pinned by the
  conformance kit. Cross-cutting: fix its key scheme alongside E-G1 (both
  shape `FieldMeta`).

### Tier 2 — app generation (a view/app schema layer above the action schema)

- **E-G7 — Collections & views** · P2 · **Implemented** — see
  `spec/forms/views.md` (`morph::views::viewSchemaJson`, `BRIDGE_REGISTER_VIEW`,
  `ViewRegistry`) and the `src/qt/forms` `CollectionView.qml` reference
  renderer.
- **E-G8 — Workflows & navigation** · P2 · **Implemented** — see
  `spec/forms/workflows_navigation.md` (`morph::flows::Wizard`/`FlowSession`,
  `morph::app::App`, `BRIDGE_REGISTER_WIZARD`/`BRIDGE_REGISTER_APP`) and the
  `src/qt/forms` `WizardView.qml` reference renderer plus the demo's
  `AppShell.qml`.

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
| **1 — Presentation** | E-G1, E-G2, E-G3, E-G10 | Pure additive `x-*` metadata; biggest "looks bespoke" ROI, lowest risk, no new logic. Do first and in parallel. E-G10 rides along because its key derivation shapes `FieldMeta` — fixing it before labels proliferate is cheap; retrofitting it after is a migration. |
| **2 — Renderer toolkit (start)** | E-G9 (reference renderer + conformance kit) | Stand up the reusable QML renderer + conformance corpus against Wave-1 keys, so every later key has a renderer that proves it and a test that pins it. Theming/slots can trail. |
| **3 — Form logic** | E-G4, then E-G5, E-G6 | E-G4 first (single rule source → schema + client + server, on top of Wave-0). E-G5 and E-G6 build on the reactive path and can follow in parallel once E-G4's rule/annotation plumbing exists. |
| **4 — App generation** | E-G7, then E-G8 | The view/app-schema layer. E-G7 (lists/master-detail) first — E-G8's wizards/navigation compose G7 screens and Tier-1 forms, so it comes last. |

Rule of thumb: **Waves 0–2 make single-action forms production-grade; Waves 3–4
turn the form generator into an app generator.** A team wanting quick wins can
stop after Wave 2 and still have a dramatically better form-building story.

## Fast reference — minimum bars

- **Local, trusted, in-process:** ship now + C1 (observability) + C3 (soak) for
  confidence. Consider B1/B2 if you persist.
- **Remote, internal/trusted network:** A1 + A2 + A3 + A4 + A7 at minimum; B1/B2
  if you persist; C1 + C3.
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
