# The application ladder

Three stateful applications of clearly different size, each anchored to an
existing open source project, built to stress-test morph and find the
framework's limits. Persistence is SQLite through the Lightweight ORM
throughout; every client is the generic one in [`client`](client), drawing the
screens each application's server serves.

The ladder is a **detector, not a product**. Its output is findings fixed in
`include/morph`, not applications shipped — see [`AGENTS.md`](../AGENTS.md)'s
filing bar for what a rung's defect is worth recording and what it is not.
When a rung needs the same hand-rolled guarantee twice, the framework is
missing a seam.

## The rungs

| # | App | Size | Anchor project(s) | What it is there to stress |
|---|-----|------|-------------------|----------------------------|
| 1 | [`bookmarks`](bookmarks) | small | [linkding](https://github.com/sissbruecker/linkding) | The shortest complete path through models, persistence, screens and a client: multi-entity CRUD, bulk actions, signed-token sessions and authorization, a background job, a changes-since poll |
| 2 | [`bank`](bank) | medium | — | A real domain: many models and actions, sign-in, money, keyed stateful models, the offline queue and `SyncWorker`, and the WebAssembly client |
| 3 | [`lims`](lims) | large | [SENAITE](https://github.com/senaite/senaite.core), [InvenTree](https://github.com/inventree/InvenTree), [ODK Central](https://github.com/getodk/central) | Many screens across modules: unit algebra, versioned schema-driven forms, shared instances, offline capture with conflict detection, role-gated transitions, the journal as an audit trail |

The sizes differ on purpose. A small application keeps the simple path honest;
a medium one exercises a real domain; a large one is where navigation,
permissions and many screens across modules are tested. **lims is the rung that
grows**: new screens, modules and models are added there, so the ladder always
has one application whose size is what a real client faces. A new framework
capability that needs an application to prove it lands in lims unless it is
specifically about the small path (bookmarks) or bank's domain.

[`rungs.txt`](rungs.txt) is the single authority for the rung list and its
order. bookmarks and lims are built by `morph_add_rung()`; bank keeps its own
`CMakeLists.txt`, because it builds variants a rung does not (the in-memory
model library for WebAssembly, the command-line client), and is added by the
same rung loop. [`bank/README.md`](bank/README.md) says which conventions it
does not yet follow.

Each rung's folder has a README describing what it implements, the reference
implementations it studies, the design questions it resolved and the gaps it
ships with. Two binding companion documents:

- [`IMPLEMENTATION.md`](IMPLEMENTATION.md) — how the apps are written: models
  are the application; screens decide nothing; strong types only in DTOs;
  persistence exclusively through the Lightweight ORM; models 100% unit
  tested.
- [`TESTING.md`](TESTING.md) — how they are tested: screens headless on the
  screen harness, models through the dual-mode rig (in-process `LocalBackend`,
  a WASM-shaped single-thread mode, and `QtWebSocketBackend` against an
  in-test `RemoteServer` with N clients), journeys, and scenario files against
  a running server.

**Audience.** The primary audience of every rung is morph's own regression
suite. No rung is a showcase: [`IMPLEMENTATION.md`](IMPLEMENTATION.md) rule 2's
zero-styling rule applies to all three.

**Design questions are answered in writing.** Each rung README names the
design questions its features raise and records the answer, with the
reasoning, in the README itself, before the feature is called done.

## Spikes without an application

Some framework questions are cheaper to answer with a focused test than with a
rung. These live in the library's own suite and stay there:

- **Forms conformance** — `tests/test_forms_conformance_corpus.cpp`: fixture
  actions exercising every key of the renderer contract in
  `docs/spec/forms/forms.md`, generated through `schemaJson<A>()`, so an
  emitter change that alters the contract fails here, with no socket.
- **`Rational` at ledger-realistic magnitudes** —
  `tests/test_ledger_rational_fuzz.cpp`: intermediate overflow and the
  checked-arithmetic cases.
- **Journal payload evolution** — `tests/test_journal_payload_evolution.cpp`
  and the two-binary skew probe `tests/compile_checks/journal_skew_probe.cpp`:
  replay across a renamed or retyped action field.
- **Runtime custom fields** — `tests/test_extension_bag_spike.cpp`: one model
  with a runtime-defined field through schema, forms, validation and the
  journal, using only primitives the framework already ships.

## Cross-cutting stress map

- **Sessions / authorization** — bookmarks (signed tokens, instance and row
  ownership), bank (sign-in, per-owner checks), lims (role-gated transitions,
  refusal of an empty principal)
- **Shared, keyed instances and per-model strands** — bank (`AccountModel`,
  `CustomerModel`), lims (`SampleModel`, attached by bench and office clients)
- **Journal / audit** — bookmarks (outbox-managed bulk actions), lims (the
  audit trail reconstructed from the journal alone)
- **Offline queue + replay** — bank (`InMemoryOfflineQueue` and `SyncWorker`,
  and a Lightweight-backed queue against the conformance suite), lims (field
  capture with base versions and conflict detection)
- **Exactly-once** — bookmarks (import op-ids through
  `morph::offline::IReplayLedger`), lims (at-most-once replay in a durable
  table)
- **Forms + exact values / units** — lims (unit algebra, detection limits,
  versioned analyses, cross-field rules); bookmarks and bank exercise the
  ordinary path
- **Background jobs and polling** — bookmarks (the metadata-fetch worker as an
  internal client, `GetChangesSince`)
- **Remote transport and its limits** — all three, through the rig's `Socket`
  mode and the scenario corpora under `scripts/scenario/scenarios/`

## Recurring framework questions

Each is answered once, at the rung named, and reused afterwards:

1. **Background jobs** (bookmarks) — work triggered by an action that
   completes later and mutates a model outside any client request. The answer
   is the internal-client pattern with no new framework seam: a worker
   dispatches through `SimulatedRemoteBackend` into the same `RemoteServer` a
   socket client reaches, under a service principal with its own signed
   token, so its calls are authorized and journaled like a user's.
   Bookmarks' README records the design.
2. **Event polling** (bookmarks) — a changes-since action substitutes for
   server push. Its cursor pairs a timestamp with a same-instant id
   tie-break, because a timestamp alone drops a write landing in the same
   millisecond. Every client wait is bounded by
   `Bridge::setExecuteDeadline`, and a rate-limited frame is answered, not
   dropped.
3. **Exactly-once delivery** — the wire `Envelope` carries no idempotency key,
   so a reply lost after commit means a retry double-applies. The answer is
   an op-id inside the action payload plus a server-side applied-ops table,
   promoted as `morph::offline::IReplayLedger`
   ([`docs/spec/offline/offline.md`](../docs/spec/offline/offline.md)).
4. **Journal payload evolution** (lims) — replay decodes stored payloads with
   the *current* action structs, so renaming a field would silently drop
   recorded data. Replay refuses on a payload-shape fingerprint mismatch, and
   lims' README records the migration story.

## Journal honesty

`morph::journal` is an **audit trail** whose replay is exact only for pure,
deterministic, single-instance, in-memory models — not an event-sourcing
engine. `undoLast()` returns a *detached* holder (no API installs it into a
live server registry, so in-place undo of a shared instance is not possible);
cascaded mutations get no causal link to their trigger; there are no
cross-model transactions or correlated entries. The ladder-wide position: the
journal is used for audit and history rendering; undo on a database-backed or
shared model is a compensating domain action, never a raw journal undo; and
framework growth (replay-mode signalling, causal parent ids, per-principal
undo, indexed reads) is proposed as a finding rather than assumed.

## Conventions

- **License hygiene (binding).** morph is Apache-2.0; several anchors are
  AGPL/GPL (SENAITE among them). Anchors are studied for *requirements,
  data-model shapes and behaviour only* — no source code, comments or
  substantial expressive structure is ported from a copyleft project. Where a
  README says "model on", it means the observable API surface and semantics,
  never the code.
- **Security opt-ins.** Everything in `docs/spec/security.md` defaults
  fail-open, so each rung's README says which hardening it turns on and which
  it leaves at the default, and a rung that relies on the default says so
  rather than implying a hardened server.
- **Observability.** A rung's server installs a logging
  `morph::observe::MetricSink`.
- **Persistence & migrations.** All app persistence goes through the
  Lightweight ORM per [`IMPLEMENTATION.md`](IMPLEMENTATION.md); schema is owned
  by `LIGHTWEIGHT_SQL_MIGRATION` definitions.
- **Docs tax.** A framework change a rung provokes lands in `include/morph`
  and pays the full spec + Doxygen (`WARN_AS_ERROR`) cost. Example code is
  exempt.

## Known limits the ladder is designed to hit

- **No server-initiated push.** Mitigated by the event-polling pattern.
  Sub-second collaborative editing (operational-transform class) is out of
  scope for the whole ladder: it is the one workload that genuinely needs
  push.
- **`Completion<T>` is not composable.** Long-running operations need a
  submit → job id → poll idiom; nested execute-and-wait orchestration can
  deadlock the worker pool.
- **Compiled C++ action types vs. runtime-defined entities.** How far served
  JSON-Schema forms stretch without runtime type creation is measured by the
  extension-bag spike above and by lims' versioned analyses.
- **Authorization is per execute, attachments are ownerless.** Revoking a
  principal does not detach it from shared instances unless the authorizer
  distinguishes them, and a token expiring between authorize and authenticate
  dispatches with an **empty principal**, which a regulatory model (lims) must
  refuse.
- **WASM is not desktop.** The WebAssembly client is single-threaded:
  `NetworkMonitor` (a probe thread) and `SqliteOfflineQueue` (the filesystem)
  do not run in the browser, so offline capture is desktop-first.
