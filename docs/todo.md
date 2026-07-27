# Program tracker

Two programs live here:

- **[§F — Stateful models](#f-stateful-models-open)** is **open**. It came out of
  [issue #18](https://github.com/LASTRADA-Software/morph/issues/18) ("compare
  against Axelor / Jmix / Causeway / Orleans and find out what we are missing").
  Its designs are in [`docs/planned/`](planned).
- **§A–§E — Production hardening & GUI generation** is **shipped**, kept for
  the rationale (priority, dependency order) that motivated the work.
  Present-tense designs for those are in [`docs/spec/`](spec).

---

# §F. Stateful models (open)

## What the issue #18 survey actually found

The four projects named in the issue are not comparable to morph as a set:
Axelor, Jmix and Causeway are full-stack Java business-application platforms;
Orleans is a .NET distributed actor runtime. Reading their feature lists side by
side produces a backlog of ~25 items, most of which morph should never build —
it does not own a database, a process engine, or an IDE.

So the survey was done the other way round: against **use cases in
`examples/bank`**, morph's largest worked example, asking where a developer hits
a wall rather than which boxes are unticked. That produced one finding that
subsumes most of the others.

**morph's models are anonymous and stateless, so the model layer is doing
nothing.** Every bank model's only member is the `std::optional<DataMapper>` it
inherits from `WithMapper` — a connection, not domain state. The per-instance
strand that morph advertises as its core service therefore protects nothing;
every action is a full round trip to SQLite; and because a `BridgeHandler`
registers one instance, the desktop GUI holds five `AccountModel` instances (and
five SQLite connections) for what is logically one thing.

Orleans is the comparator that names this directly — a grain is *identity +
behaviour + state*, and morph has only behaviour. But the fix is not to become
an actor runtime. It is to make morph's existing model layer do the job it
already claims: hold state, be identified, be reachable.

## Accepted items

### F1 — Reshape `examples/bank` onto stateful models · P0 · planned

Split the per-domain models into models keyed by the entity they are named
after, holding that entity in memory, hydrated on activation and written
through on mutation. This is first deliberately: it is what introduces the main
idea of the library, and F2/F3 are unverifiable without it — a primary key
identifies nothing when instances carry nothing.

See [`planned/stateful_bank_example.md`](planned/stateful_bank_example.md).

### F2 — Keyed, shareable model instances · P0 · planned

A model declares a `PrimaryKey`; actions declare which field carries it (or that
their *result* establishes it); `BridgeHandler<M, AllowShared>` opts a handler
into a **server-side** instance directory, so instances are reusable across
clients. `instances<M>()` enumerates the live keys. A keyed action re-points a
handler rather than re-keying an instance, so key collisions do not arise.

Carries a change to shipped behaviour: A7's `closeConnection` must decrement a
reference count rather than erase, or one client's disconnect destroys an
instance another client is using.

See [`planned/shared_model_instances.md`](planned/shared_model_instances.md).

### F3 — Instance subscriptions · P1 · planned

`subscribe<R>(cb)` keyed on the **result/state** type, firing whenever an `R` is
produced on the attached instance by any handler on any connection. Introduces
morph's first server-initiated wire message.

**Removes** the reactive-draft mechanism (`set<&A::field>`, `reset<A>`, the old
action-keyed `subscribe`, in-flight coalescing), whose job stateful models do
better by holding the draft server-side. Blocked on reworking `morph::flows`,
which is built on the mechanism being deleted.

See [`planned/instance_subscriptions.md`](planned/instance_subscriptions.md).

## Considered and refused

Recorded so the survey does not get re-run and so the boundary is explicit.

| Not building | Why |
|---|---|
| **Entity metamodel / naked objects** (Causeway) | morph's unit is the *action*, deliberately. Rows plus row-bound actions already are an object UI; what is missing is metadata on the row, not a second parallel metamodel. |
| **Server-authoritative per-instance action availability** ("see it, use it, do it") | Real gap — `CloseAccount` is implemented, tested and exposed in no GUI because the client cannot ask "may I?". Judged not worth the surface area now. |
| **Query invalidation / live lists** | Superseded in part by F3, which gives shared instances a change channel without a query-invalidation vocabulary. |
| **Paging / sorting contract on view schemas** | `views.md` already records "no server-side query language" as a non-goal; paging stays a field on the query action. |
| **Result-type presentation metadata** (money, enum labels, badge severity) | Every GUI controller hand-writes a `toMap()` projection. Genuine duplication, but it is a forms-layer concern, not a model-layer one. |
| **Field- and row-level permissions** (Jmix, Causeway SecMan) | They own the ORM and the whole app; morph owns a seam. Per-principal schema redaction would break the one-cached-schema-per-type design. The model is the right place, and `bank`'s `loadOwned` guard shows it works. |
| **BPM / BPMN engine** (Axelor, Jmix) | A process engine without a store is meaningless, and morph does not own the store. |
| **Grain call filters / interceptor pipeline** (Orleans) | morph already has validator + authorizer + `observe` + journal on every dispatch path. No observed friction. |
| **Clustering, silos, placement, grain versioning, stateless workers, distributed ACID transactions** (Orleans) | morph is a UI bridge with one server, not a distributed runtime. |
| **Managed streams with cursors and checkpoints** (Orleans) | F3 is best-effort and unbuffered by design; durability here is a distributed-runtime concern. |
| **Reporting engine** (BIRT, JasperReports), **ORM + schema migration**, **IDE Studio** | Out of identity. morph owns neither the store nor the tooling. |
| **Runtime custom fields / dynamic attributes** (Axelor, Jmix) | Hard against compile-time reflection, and no observed need. |
| **REST / GraphQL facade, blob transfer, tabular export, multi-tenancy discriminator** | Plausible, no observed need. Revisit when one exists. |

## Also surfaced

- **`README.md`'s "Status & limitations" is stale.** It still claims the wire
  protocol has no version negotiation, that `RemoteServer` model ids are
  guessable sequential integers, and that only an in-memory offline queue
  ships — all fixed by §A/§B below. Worth correcting independently of §F.

---

# Production hardening & GUI generation (shipped)

This tracked a design-approved, prioritized checklist of production-hardening
and GUI-generation work. **Every item in §A–§E has shipped.** The authoritative,
present-tense design for each is in `docs/spec/`; this section is a record of
what landed and why, kept for the rationale (priority, dependency order) that
motivated the work.

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

> **F2 changes this.** Cross-client instance sharing requires `closeConnection`
> to decrement a reference count rather than erase. See
> [`planned/shared_model_instances.md`](planned/shared_model_instances.md).

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

  > **F3 reworks this.** `FlowSession` is built on the reactive-draft mechanism
  > F3 removes; it must be re-expressed as a stateful, keyed flow model first.

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

- Every §A–§E item landed opt-in or backward compatible by default — none change
  existing behavior unless enabled. **§F breaks this pattern deliberately:** F2
  changes A7's cleanup semantics and F3 removes a public API.
- Two items surfaced *by* the §A–§E program, not originally on it, and fixed
  before it closed out: C3's fuzz harness found two real bugs in `morph::wire`'s
  glaze-based parsing (a heap-buffer-overflow reachable by a 5-byte input, and
  a case where `RemoteServer`'s own error reply didn't round-trip through
  `encode`/`decode`). See `docs/spec/testing_strategy.md`'s "Known findings"
  section.
