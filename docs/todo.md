# Production-hardening program — status

This tracked a design-approved, prioritized checklist of production-hardening
and GUI-generation work. **Every item below has shipped.** The authoritative,
present-tense design for each is in `docs/spec/`; this file is now a changelog
of what landed and why, kept for the rationale (priority, dependency order)
that motivated the work.

Readiness depended on deployment mode:

- **Local mode** (models in-process, no network): was already near
  production-ready; only the operational items (§C) and a couple of §B
  robustness items applied.
- **Remote mode** (`RemoteServer` over a network): needed the §A hardening
  milestone before public/multi-tenant exposure. All of §A is now shipped.

Priority key (as originally assigned): **P0** must-have before that mode
ships, **P1** strongly recommended, **P2** nice-to-have.

---

## A. Remote-mode hardening (do before networked/public/multi-tenant use)

### A1 — Server-side action validation · P0 · shipped
`ActionValidator<A>::ready` (+ declared-precision reconciliation) now runs
inside the dispatcher runner and `Bridge::executeVia`'s `localOp`, before
`Model::execute`. A `false` result rejects the call as `morph::model::ValidationError`
(`err`/`onError`). See `spec/core/registry.md` and `spec/core/bridge.md`.

### A2 — Register authorization & opaque model ids · P0 · shipped
`IAuthorizer::authorizeRegister` (default allow-all) is enforced in
`RemoteServer`; model ids are opaque (non-sequential) rather than a
guessable counter. See `spec/session/session.md` ("The register-authorization
hook", "Opaque model ids") and `spec/core/backend.md`.

### A3 — Transport-level resource limits · P0 · shipped
`RemoteServer::LimitPolicy` (execute timeout, max live models, max in-flight)
and `QtWebSocketServerConfig` (max connections, per-frame size, per-connection
rate, idle/handshake timeouts) are implemented, all opt-in/default-off. See
`spec/core/backend.md#limitpolicy--opt-in-resource-limits` and
`spec/security.md`'s hardening checklist.

### A4 — TLS + peer verification as the enforced default · P0 · shipped
`qt_tls.hpp` ships `tlsVerifyingConfig()`/`tlsPinnedConfig()`/`tlsInsecureNoVerify()`
as the documented client-side path, `examples/qt_tls_client/` demonstrates
pinned-cert acceptance and MITM rejection end to end, and
`QtWebSocketServerConfig::bindAddress`/`allowPlaintextExposure` make
`QtWebSocketServer::listen()` refuse a silent non-loopback plaintext bind
unless explicitly overridden. See `spec/security.md#transport-security-the-qt-websocket-transport`
and `spec/core/backend.md#qtwebsocketserver--server-side-websocket-transport`.

### A5 — Inject a vetted HMAC for production · P1 · shipped
`examples/vetted_hmac/` ships libsodium and OpenSSL `MacFunction` adapters
(each with a known-answer + interop test) and the opt-in
`MORPH_REQUIRE_VETTED_HMAC` build option, which drops the `mac = hmacSha256`
default argument on `TokenIssuer`/`TokenVerifier`/`SigningAuthorizer` so a
build relying on it fails to compile. See `spec/security.md`, "MAC-primitive
recommended wiring".

### A6 — Protocol / action-schema versioning · P1 · shipped
A `"hello"` handshake negotiates a protocol version range between client and
server (opt-in, explicit — not automatic-on-connect); `BRIDGE_REGISTER_ACTION`'s
generated `fromJson`/`resultFromJson` now decode leniently, making the
additive-only action-evolution policy actually hold. See `spec/core/wire.md`'s
"Protocol version negotiation" and "Action-evolution policy" sections.

### A7 — Connection-scoped model cleanup · P0 · shipped
`RemoteServer` has an opt-in `ConnectionId` scope (`openConnection`/
`closeConnection` + a scoped `handle(msg, reply, cid)` overload), and
`QtWebSocketServer` opts every client into it end to end, so a client crash or
dropped socket now reclaims its models instead of stranding them until process
exit. `closeConnection` is server housekeeping — it bypasses `IAuthorizer` by
design, not a synthesized wire `deregister`. See `spec/core/backend.md#connection-scopes`.

---

## B. Durability & data-integrity (both modes, if you persist)

### B1 — Durable offline queue & cross-restart dead-lettering · P1 · shipped
`QueueItem::attempts` (durable) + `IOfflineQueue::setAttempts` write-back hook
and an optional `SyncWorker::DeadLetterSink` are implemented — `InMemoryOfflineQueue`
overrides `setAttempts` in-memory (unchanged behavior); a durable queue (see B3)
persists the count across restarts. See `spec/offline/offline.md`.

### B2 — Transactional outbox (journal + store atomicity) · P1 · shipped
`LogEntry::idempotencyKey` + dedup in `InMemoryActionLog`/`FileActionLog::append()`,
`IModelHolder::setOutboxManaged`/`isOutboxManaged` (suppresses the automatic
journal append), and `journal::OutboxRelay` (drains an outbox table into a
durable `IActionLog` in the model's own transaction) are implemented. See
`spec/journal/journal.md` and `spec/core/registry.md`.

### B3 — A shipped durable `IOfflineQueue` implementation · P2 · shipped
`FileOfflineQueue` (zero-dependency NDJSON, ships in the default `morph`
target) and `SqliteOfflineQueue` (opt-in via `MORPH_BUILD_OFFLINE_SQLITE`)
both persist `payload` + `idempotencyKey` + `attempts` across restarts. See
`spec/offline/offline.md`.

### B4 — Journal format versioning & retention · P2 · shipped
`journal::fromJson` decodes leniently (matching the wire's forward-compatible
stance); every `LogEntry` carries a `v` line-format version, rejecting a
future format a reader doesn't understand instead of silently misreading it;
`FileActionLog::rotate(sealedPath)` gives hosts a retention seam. See
`spec/journal/journal.md`.

---

## C. Operational readiness (needed for any production, esp. remote)

### C1 — Observability: metrics, tracing, health · P1 · shipped
A lightweight, injectable `morph::observe` seam (metrics + trace spans,
mirroring the logger's replaceable-sink pattern) is wired into `RemoteServer`,
`LocalBackend`, `SyncWorker`, and `ReconnectCoordinator`; `RemoteServer::health()`/
`setHealthHandler()` expose a readiness snapshot, flipped by `beginShutdown()`
(see C5). See `spec/core/observability.md`.

### C2 — Non-Qt transport option · P2 · shipped
`morph::net` (`SocketBackend`/`SocketServer`, opt-in via `MORPH_BUILD_NET`) is
a raw-socket RFC 6455 WebSocket transport with no Qt dependency, wire-interoperable
with `QtWebSocketBackend`/`QtWebSocketServer` (verified both directions). No
TLS in this reference transport — pairs with a TLS-terminating proxy, or use
the Qt transport's TLS support (A4) directly. See `spec/core/backend.md`'s
`SocketBackend`/`SocketServer` section.

### C3 — Load / soak / fuzz testing · P1 · shipped
`fuzz_wire_decode`/`fuzz_dispatch_execute` (`MORPH_BUILD_FUZZERS=ON`), the
switchBackend/reconnect soak tests and the throughput/latency benchmark
(`MORPH_BUILD_LOAD_TESTS=ON`), and the adversarial cross-socket run
(`MORPH_BUILD_QT=ON`) are implemented, all opt-in/default-off. The fuzz
harness surfaced two real `morph::wire` bugs on first run (a `skip_ws`
heap-buffer-overflow, and unescaped control bytes breaking the `err`-reply
round-trip) — both are now fixed; see `spec/testing_strategy.md`'s "Known
findings" for details and the permanent regression cases under
`tests/fuzz/findings/`.

### C4 — Compile-time `onBackendChanged` dispatch · P2 · shipped
`LocalBackend::notifyBackendChanged` no longer `dynamic_cast`s every live
model; backend-change-awareness is captured at registration time
(`IModelHolder::isBackendChangeAware()`) and driven from a maintained set —
a pure, behavior-preserving refactor (parity-tested). See
`spec/core/backend.md` and `spec/core/registry.md`.

### C5 — Graceful shutdown & drain · P1 · shipped
`RemoteServer::beginShutdown()` + `drainedWithin(deadline)` (reject new
`register`/`execute` with `err "server shutting down"`, drain the shared
in-flight counter, flip `health().ready` to `false`) and
`QtWebSocketServer::closeGracefully(deadline)` (close frames, then hard stop)
are implemented, opt-in/default-off — a server that never calls either
behaves byte-for-byte as before. See `spec/core/backend.md#graceful-shutdown-beginshutdown--drainedwithin`.

---

## D. Process / project

### D1 — Spec ↔ code drift guard (CI) · P1 · shipped
A CI check pins the mechanical facts that had drifted before (enum
cardinalities, key constants, canonical error-message strings, glaze
`error_on_unknown_keys` behavior) via `docs/spec/pinned_facts.toml`,
`tests/test_pinned_facts.cpp`, and `scripts/check_spec_citations.sh` — future
drift now fails the build. See `CONTRIBUTING.md`, "Quality gates".

### D2 — API stability / 1.0 commitment · P2 · shipped
A concrete versioning/deprecation/compat policy is published, plus
`morph::version` constants cross-checked against `CMakeLists.txt` and a CI
lint on `[[deprecated]]` marker format. See `docs/spec/VERSIONING.md`.

---

## E. GUI enhancement program (rapid + flexible GUI development) — shipped

A layered program that generates GUIs from the user's model + action types
with minimal declaration, while keeping the result flexible. The guiding
principle (*infer by default, declare to override*; all new `x-*` keys are
additive/unversioned; Qt/QML is the reference renderer, the schema contract
stays renderer-agnostic) is now documented in
[forms.md](spec/forms/forms.md#design-principle-infer-by-default-declare-to-override).

### Tier 1 — richer forms (additive metadata/logic on the single-action form)

- **E-G1 — Field metadata** · P1 · shipped — `spec/forms/forms.md`
  ("Field metadata — `FieldMeta`").
- **E-G2 — Layout & grouping** · P1 · shipped — `spec/forms/forms.md`
  ("Layout & grouping — sections, tabs, spans").
- **E-G3 — Widget hints** · P1 · shipped — `spec/forms/widget_hints.md`
  (`Multiline`/`Ranged`, `x-widget`/`x-min`/`x-max`/`x-step`).
- **E-G4 — Cross-field rules** · P1 · shipped — `spec/forms/forms.md`
  ("Cross-field rules — the `x-rules` vocabulary"), shared with the
  server-side validator (`spec/core/registry.md`).
- **E-G5 — Computed fields** · P2 · shipped — `spec/forms/forms.md`
  ("Computed fields").
- **E-G6 — Dependent choices** · P2 · shipped — `spec/forms/choice.md`
  ("Dependent (cascading) options").
- **E-G10 — Localisation (i18n)** · P1 · shipped — `spec/forms/forms.md`
  ("Localisation — message keys and the catalog seam"), `FieldMeta::i18nKey`.

### Tier 2 — app generation (a view/app schema layer above the action schema)

- **E-G7 — Collections & views** · P2 · shipped — `spec/forms/views.md`
  (`morph::views::viewSchemaJson`, `BRIDGE_REGISTER_VIEW`, `ViewRegistry`)
  and the `src/qt/forms` `CollectionView.qml` reference renderer.
- **E-G8 — Workflows & navigation** · P2 · shipped — `spec/forms/workflows_navigation.md`
  (`morph::flows::Wizard`/`FlowSession`, `morph::app::App`,
  `BRIDGE_REGISTER_WIZARD`/`BRIDGE_REGISTER_APP`) and the `src/qt/forms`
  `WizardView.qml` reference renderer plus the demo's `AppShell.qml`.

### Ecosystem

- **E-G9 — Renderer toolkit** · P1 · shipped — `spec/forms/forms.md`
  ("Shipped Qt/QML reference renderer", "Renderer conformance kit",
  "Theming / component-override registry").

## Fast reference — minimum bars (all now met)

- **Local, trusted, in-process:** ship + C1 (observability) + C3 (soak) for
  confidence. B1/B2 if you persist. All shipped.
- **Remote, internal/trusted network:** A1 + A2 + A3 + A4 + A7 at minimum;
  B1/B2 if you persist; C1 + C3. All shipped.
- **Remote, public / multi-tenant:** all of §A, all of §B if persisting, all
  of §C, D1. All shipped.

## Notes

- Every item landed opt-in or backward compatible by default — none change
  existing behavior unless enabled.
- `docs/planned/` no longer holds any implemented-item specs; the
  authoritative current-state specs are entirely in `docs/spec/`.
- Two items surfaced *by* this program, not originally on it, and fixed before
  it closed out: C3's fuzz harness found two real bugs in `morph::wire`'s
  glaze-based parsing (a heap-buffer-overflow reachable by a 5-byte input, and
  a case where `RemoteServer`'s own error reply didn't round-trip through
  `encode`/`decode`). See `docs/spec/testing_strategy.md`'s "Known findings"
  section.
