# The application ladder

A sequence of stateful applications of gradually increasing complexity, each
anchored to existing open source software, designed to stress-test every
morph subsystem and find the framework's limits. Persistence is SQLite via
the Lightweight ORM throughout; clients are Qt (desktop + WASM), as in
[`bank`](bank).

**Program scope (round-7 holistic review):** the committed build is
**rung 0 through rung 4** plus the no-app spikes below — that is where the
unproven seams live (first WASM-remote, first shared-over-socket, offline
replay, exactly-once, SQLite contention) and where reviews locate peak
findings-per-week. **Rungs 5–8 are a design annex**: their READMEs are
finished deliverables (requirements studies whose sharpest content the
spikes convert into CI at a fraction of construction cost); building any of
them is a separate decision taken *after* rung 4 with the
[finding pipeline](FINDINGS.md) scoreboard in hand. Ledger (rung 5) is the
strongest candidate to build — the only annex rung with a genuinely
app-shaped core; forge's framework content ships as its load script against
synthetic models, and crm's as the extension-bag spike. The program's
product is **findings fixed, not apps shipped** — see
[`FINDINGS.md`](FINDINGS.md) for what counts, triage, the fix budget, exit
criteria, and the demotion policy.

**The no-app spikes** (start immediately, in parallel with rungs 0–1; each
files findings, none builds an app):

1. **Forms conformance suite** — the round-5 D1–D8 test constructions
   (retag-vs-round, clamped-wire, nested enforcement, render-old/validate-new
   skew, locale, stale Choice, auto-fire, rules parity); needs no socket.
2. **Rational property/fuzz harness** at ledger-realistic magnitudes
   (intermediate overflow, checked-arithmetic case).
3. **Journal payload-evolution spike** — replay across a renamed/retyped
   action field; the versioning/migration design input for the annex.
4. **Extension-bag spike (7b)** — one model with a runtime custom field
   through schema, forms, validation, journal; answers the crm endgame
   without the CRM.
5. **Forge load script** — synthetic notification/poll models, 500–2,000
   sockets, hardened configuration, epoch resync across restart.

**Audience decision:** the primary audience of every rung is morph's own
regression suite and finding ledger. The single polished showcase is
**kanban** (mid-ladder, every subsystem load-bearing, visually legible);
every other rung takes rule 2's zero-styling literally, no guilt.

Each rung's folder contains a README describing what to implement, the open
source reference implementations to study, and the framework limits the rung
is expected to hit. Two binding companion documents:
[`IMPLEMENTATION.md`](IMPLEMENTATION.md) — how the apps are written
(models are the application; minimal schema-driven GUIs; strong types only
in DTOs, `std::string` the sole plain type; persistence exclusively through
the Lightweight ORM; models 100% unit tested) — and
[`TESTING.md`](TESTING.md) — how they are tested: every rung's GUI is
presenter-shaped and unit tested in **both deployment modes** (in-process
`LocalBackend`, and `QtWebSocketBackend` against an in-test `RemoteServer`
with N clients) plus a WASM-shaped single-thread mode, via the shared
`examples/common/testkit`.

Discipline rule: each rung names explicit **design questions**; they must be
resolved *in writing* (in that rung's README) before the next rung starts —
later rungs consume earlier answers (5 reuses 4's cascade-journaling answer,
7 reuses 4's board pieces, 8 reuses 2's job pattern and 3's event pattern).

| # | App | Anchor project(s) | New subsystems under stress |
|---|-----|-------------------|-----------------------------|
| 1 | [`pastebin`](pastebin) | [MicroBin](https://github.com/szabodanika/microbin) | Full loop smoke test; journal semantics of state-mutating reads and expiry |
| 2 | [`bookmarks`](bookmarks) | [linkding](https://github.com/sissbruecker/linkding) | Multi-entity CRUD, bulk actions, sessions/authz, background jobs |
| 3 | [`polls`](polls) | [Rallly](https://github.com/lukevella/rallly) | Shared instances, anonymous principals, undo, event polling |
| 4 | [`kanban`](kanban) | [Kanboard](https://github.com/kanboard/kanboard) | Strand ordering under concurrency, RBAC, offline queue + replay, action cascades |
| 5* | [`ledger`](ledger) | [Firefly III](https://github.com/firefly-iii/firefly-iii), [Actual Budget](https://github.com/actualbudget/actual) | Exact `Rational` arithmetic under invariants, multi-currency, sync-philosophy benchmark |
| 6* | [`lims`](lims) | [SENAITE](https://github.com/senaite/senaite.core), [InvenTree](https://github.com/inventree/InvenTree), [ODK Central](https://github.com/getodk/central) | Unit algebra, versioned schema-driven forms, offline entities with conflict detection |
| 7* | [`crm`](crm) | [EspoCRM](https://github.com/espocrm/espocrm), [Tryton](https://github.com/tryton/tryton), [Frappe](https://github.com/frappe/frappe) | Metadata-driven forms, dynamic logic, per-field authz; **7b** (gated): runtime custom fields |
| 8* | [`forge`](forge) | [Gogs](https://github.com/gogs/gogs), [Gitea/Forgejo](https://github.com/go-gitea/gitea), GitLab architecture | Everything at once: orgs/permissions, notifications at scale, webhooks, out-of-protocol sidecars |

\* = design annex: README is the deliverable; construction is a post-rung-4
decision (ledger first in line; forge → load script; crm → 7b spike).

## Cross-cutting stress map

Every subsystem is hit by at least two rungs:

- **Per-model strands** — 4 (concurrent board moves), 7 (multi-model lead conversion)
- **Shared instances** — 3, 4, 6, 8 (rung 3 is also the framework's *first
  ever* `AllowShared`-over-WebSocket coverage — a scope-heavy rung, like 1)
- **Journal / undo / audit** — 1, 3, 4, 5, 6 (payload evolution), 7
- **Offline queue + replay** — 4, 5, 6, 7
- **Forms + exact values / units** — 5, 6, 7
- **Sessions / authorization** — 2, 3, 4, 5–6 (empty-principal refusal), 7, 8
- **Application version skew** (old client binary vs. new server, via
  `MORPH_CLIENT_ONLY`) — 6 (owner), re-run at 8 across its own releases
- **Remote transport and its limits** — all

## The six recurring strains

These needs recur across the researched projects and deserve one
framework-level answer each, introduced at a specific rung and reused
afterwards:

1. **Background jobs** (rung 2) — work triggered by an action but completing
   later, mutating the model outside any client request. **Correction from
   verification: a typed in-process path exists today** —
   `SimulatedRemoteBackend` is a shipped public backend that routes through
   the complete server pipeline (authorizer, journal log provider,
   per-instance strand), so a server-side worker *can* be built as an
   internal client with a service principal. The genuine gap is narrower
   but real: no *sanctioned* seam, no defined service-principal convention,
   the simulated path is connection-unscoped (`ConnectionId` 0), and
   `handleInline` rejects `execute`. Rung 2's design discussion starts from
   the internal-client option and decides whether a first-class framework
   seam is still warranted; rungs 4, 5, and 8 consume the answer.
   **Time-*scheduled* jobs are a distinct shape with their own owner —
   rung 5** (recurring transactions): who ticks, on what thread, under what
   principal, journaled how. Forge's webhook retry loop assumes that answer
   exists.
2. **Event polling** (rung 3; rung 2's DoD includes a minimal
   changes-since poll as its preview) — the Zulip-style
   `getEventsSince(lastEventId)` action that substitutes for server push
   everywhere. See
   [Zulip's events system](https://zulip.readthedocs.io/en/stable/subsystems/events-system.html).
   Two hard requirements from review: event sequences must survive
   instance destruction (shared instances die *immediately* at refcount
   zero — persist events or issue epoch tokens forcing full resync), and
   the client polling helper must bound every call (when this was written a
   rate-limited server dropped frames silently and morph had no client-side
   execute deadline, so the completion hung forever; both have since changed —
   `Bridge::setExecuteDeadline` bounds the wait, and the transport now answers
   a rate-limited frame instead of dropping it).
3. **File/blob attachments** (rungs 4 and 8) — payloads that should not travel
   the JSON action protocol; side channel must share the authorizer's token
   discipline.
4. **Document generation** (rung 5) — reports/invoices/statements as
   long-running submit-then-poll jobs with defined snapshot semantics.
5. **Exactly-once delivery** (rung 4, re-tested with money in rung 5) — the
   wire `Envelope` has **no idempotency-key field** and the server cannot
   recognize a replayed operation; a reply frame lost after commit means a
   retry double-applies. The answer (an op-id inside action payloads plus a
   server-side applied-ops ledger in the model) is established in rung 4
   and reused everywhere writes are retried.
6. **Journal payload evolution** (rung 6, bites rungs 5 and 7 too) — replay
   decodes stored payloads with the *current* action structs; renaming a
   field silently drops recorded data. Versioned catalogs need per-entry
   schema pinning and a migration story.

## Journal honesty (decided at rung 1, in writing)

Review verdict: the later rungs' claims oversell `morph::journal`, which is
an **audit trail** whose replay is exact only for pure, deterministic,
single-instance, in-memory models — not an event-sourcing engine. Known
hard limits: `undoLast()` returns a *detached* holder (no API installs it
into a live server registry, so in-place undo of a shared instance is not
possible today) and pops the newest entry *regardless of principal*;
cascaded mutations get no causal link to their trigger; there are no
cross-model transactions or correlated entries, so multi-model actions
(`ConvertLead`) cannot be replayed consistently; `entries()` re-reads the
whole file. Rung 1 must write the ladder-wide position: what the journal is
used for (audit, history rendering), what it is not (undo on shared
instances — use compensating actions; cross-model replay), and which
framework growth (replay-mode signaling, causal parent ids, per-principal
undo, indexed reads) the ladder should propose instead of assuming.

## Rung 0, scope, and sequencing (from delivery review)

Verification found rung 1 had accreted ~twelve deliverables under a "smoke
test" label. The infrastructure is now split out as **rung 0**: the testkit
subset (`pump.hpp`, `backend_rig.hpp`, Qt-owning test `main`), the shared
presenter architecture (`examples/common/gui`), the `ladder-tests` CI job
with path-filtered `MORPH_LADDER_RUNGS`, and the **WASM-remote spike** (with
a written fallback if it bounces off framework work). Rung 1 is then the
pastebin app plus its own tests and design records.

Honest effort accounting (baseline: one "bank" = `examples/bank`, ≈9k LOC):
the full eight rungs would sum to **~19–25 bank-equivalents plus the
framework prerequisites** — a multi-year solo effort, which is why the
committed scope is rungs 0–4 (+ spikes): ~8–10 bank-equivalents, a
6-month-scale solo horizon, and where adversarial review expects peak
findings-per-week. Deferral decisions recorded in the rung READMEs: kanban
defers automation rules and attachments to a "later" section (ledger needs
only the cascade *decision*, writable from a spike); the annex rungs keep
their internal gates (7a/7b, forge phase 3 per-item) for whenever they are
green-lit. The **fault-injection wire proxy and the strand interleaver are
pulled forward to rung 0–1** (round-7: they outperform whole rungs on
finding yield; scheduling them at rung 4 delayed the program's
highest-value instruments behind three rungs of CRUD).

Parallelization: hard sequence **0 → 1 → 2 → 3 → 4**; after rung 4's
written answers, **5, 6, and 7a are mutually independent** (three
contributors can run them concurrently), and **8 phase 1 needs only 2's job
answer and 3's event pattern** so it can start alongside 4. The coupling
point is `examples/common` — it needs an owner and an **additive-only API
discipline** after rung 3.

**License hygiene (binding):** morph is Apache-2.0; several anchors are
AGPL/GPL (Rallly, Firefly III, EspoCRM, Tryton, SENAITE). Anchors are
studied for *requirements, data-model shapes, and behavior only* — no
source code, comments, or substantial expressive structure is ported from
copyleft projects; all ladder implementation is original. Where a README
says "model on"/"transliterate", it means the observable API surface and
semantics, never the code.

## Framework prerequisites (schedule as issues now, not rung discoveries)

Adversarial review found four items that invalidated rung definitions-of-done
as written. **All four have since shipped**; they are recorded here because
they explain why the rungs were sequenced as they were, not because anything
is still blocked on them. Each is listed with where it landed, so a reader can
check rather than take this section's word for it:

1. **Async shared/keyed attach for WASM** (before rung 3's WASM story) —
   `registerModelShared`/`attachModel` were synchronous and nested an event
   loop, which aborts the page on the WASM main thread, and
   `registerModelAsync` covered only the plain path.
   **Shipped:** `IBackend::registerModelSharedAsync` and
   `IBackend::attachModelAsync` (`include/morph/core/backend.hpp`), consumed by
   `Bridge::ensureBoundAsync`/`attachHandlerAsync` and implemented by
   `QtWebSocketBackend`.
2. **Client-side execute deadline** (before rung 3's polling helper) — no
   timeout existed on a `Completion`, so a black-holed server hung the client
   forever. **Shipped:** `Bridge::setExecuteDeadline`
   (`include/morph/core/bridge.hpp`), specified in
   [`docs/spec/core/completion.md`](../docs/spec/core/completion.md). (This
   item also named rate-limiter drops as a cause; that half no longer applies
   either — the transport now answers a refused frame instead of dropping it.)
3. **Injectable time source usable by remotely-constructed models** (before
   rung 1's expiry semantics) — `LogEntry` timestamps were hard-wired to the
   system clock and registry-constructed models are default-constructed, so
   tests needed a process-global now-provider convention.
   **Shipped:** `setNowOverride` / `ScopedNowOverride`
   (`include/morph/util/datetime.hpp`) — exactly that convention.
4. **The fault-injection wire proxy** (rung 0–1, pulled forward by the
   round-7 review) — scriptable drop/delay/duplicate/kill between client
   and server; without it the exactly-once, dead-letter, and
   reconnect-mid-replay scenarios are demos, not CI tests.
   **Shipped:** `examples/common/testkit/fault_proxy.hpp`, with the
   deterministic `strand_interleaver.hpp` alongside it as promised, both with
   their own tests. See [TESTING.md](TESTING.md).

Also queued deliberately, with their status as of this writing: the offline
queue's **depth bound has since shipped** — `IOfflineQueue::maxDepth()` and
`OfflineQueueFullError` (`include/morph/offline/offline_queue.hpp`) enforce a
reject-newest policy (note the linear-scan/quadratic enqueue applies to
`FileOfflineQueue` only — `SqliteOfflineQueue`'s key dedup is index-backed);
the **SyncWorker's 5-attempt cap is still hard-coded** and still dead-letters
legitimate writes after five flaky reconnects, though the surfacing hook rung 4
needs now exists (`SyncWorker`'s `DeadLetterSink` constructor parameter), so
"surface dead-letters in the UI, not logs" is buildable rather than blocked;
and **`SQLITE_BUSY` waits occupy pool threads** (K writing models on a 2–4-thread pool can starve every strand,
fire `executeTimeout`, and still commit — the timeout-then-committed
double-apply is rung 4's sharpest data-corruption test).

**Forms-subsystem gaps** (from the round-5 deep review; owners in the
lims/crm/ledger READMEs): no sum types in the forms palette (the
`quantity | belowLOD | aboveUDL` result is a *multi-field encoding* glued by
`x-rules`, by design); rule vocabulary is closed single-node conditions (no
`and`/`or`/`not` — EspoCRM-class logic maps onto it or becomes a framework
proposal); schemas-as-data render old versions but **validation always runs
against the current compiled struct**; no per-caller schema shaping; nested
aggregates get schemas but **no enforcement recursion and no child-table
renderer**; no pre-decode wire validation seam (clamped `Rational`s reach
`validate()` as plausible numbers).

Explicit-submit mode is **closed** (it was the last entry on this list): an
action declaring `static constexpr bool explicitSubmit = true` makes
`schemaJson<A>()` emit the top-level `x-submitMode: "explicit"` key, and the
shipped `DynamicForm.qml` renders its own Submit button — gated on the form's
`ready` state — instead of auto-firing on validity
(`docs/spec/forms/forms.md`, "Explicit submit mode"). Adoption is per rung and
only `polls` has migrated so far, on all three of its schema-driven actions;
`bookmarks`, `lims` and `pastebin` still pair `controller: null` with a
hand-written Button at thirteen call sites between them.

## Operations and security (binding conventions)

- **Security opt-in matrix** (everything in `docs/spec/security.md`
  defaults fail-open): rung 1 deliberately tests the *unhardened* default
  (a test asserts the fail-open delta) and owns the `hello`
  version-negotiation test; rung 2 must exercise `authorizeRegister` +
  `authorizeInstance` (not just `SigningAuthorizer`); rung 3 runs its
  harness with the rate limiter ON (the polling helper's timeout is
  untested otherwise); rung 4's HTTP side channel reuses `TokenVerifier`
  and joins the fuzz corpus; rungs 5–6 get a CI leg with
  `MORPH_REQUIRE_VETTED_HMAC=ON`; **rung 8 is the hardened-configuration
  demonstration** — TLS, vetted HMAC, register/instance authorization,
  full `LimitPolicy` and server bounds, negotiation — and its load script
  runs against that config (its README's non-goal is public *exposure*,
  not hardened configuration).
- **Observability**: every rung's server installs a logging
  `morph::observe::MetricSink`; rung 4 asserts `queueDepth`/reconnect
  metrics in its offline tests; rung 8's load script consumes
  `executeLatencyMs`/`executeInFlight` and drives the drain via
  `RemoteServer::health()`/`beginShutdown()`.
- **Persistence & migrations**: all app persistence goes through the
  Lightweight ORM per [`IMPLEMENTATION.md`](IMPLEMENTATION.md) — schema is
  owned by `LIGHTWEIGHT_SQL_MIGRATION` definitions (bank's pattern), which
  is the migration story lims's replay-across-migration DoD presupposes.
  No rung writes database code itself.
- **Demo seeding**: every rung ships a `--seed` path implemented on the
  testkit's `action_driver` generators (deterministic demos, screenshots,
  Playwright).
- **Docs tax**: framework prerequisites land in `include/morph` and pay the
  full spec + Doxygen (`WARN_AS_ERROR`) + pinned-facts cost — budget
  +30–50% over code cost per item. Example code is exempt.

## Known limits the ladder is designed to hit

- **No server-initiated push.** Mitigated by the event-polling pattern
  (precedented: Zulip is long-poll only; Gitea's own UI polls; EspoCRM polls).
  Rung 8's many-clients-polling is the scale test — at **500–2,000
  concurrent sockets at ~1 poll/s** (the single Qt receive/reply thread is
  the ceiling, not the worker pool), including during a graceful drain.
  Sub-second collaborative text editing (Etherpad-class OT) is explicitly
  *out of scope* for the whole ladder — it is the one workload that
  genuinely requires push.
- **`Completion<T>` is not composable** — long-running operations (merge,
  report generation) need a submit → job-id → poll-status idiom; nested
  execute-and-wait orchestration can deadlock the worker pool (rung 7 tests
  this deliberately).
- **Compiled C++ action types vs. runtime-defined entities** — rung 7's
  endgame (Salesforce-style custom fields) decides how far served JSON-Schema
  forms can stretch without runtime type creation.
- **Authorization is per-execute, attachments are ownerless** — revoking a
  principal does not detach it from shared instances or cut off reads unless
  the authorizer distinguishes them (rungs 4 and 8 test revocation
  mid-session); a token expiring between authorize and authenticate
  dispatches with an **empty principal**, which regulatory rungs (5, 6) must
  refuse at the model.
- **WASM ≠ desktop.** The shipped WASM pattern is single-threaded and
  local-only: `NetworkMonitor` (probe thread) and `SqliteOfflineQueue`
  (filesystem) do not run in the browser, and a WASM client over
  `QtWebSocketBackend` has never been exercised. Rung 1 proves WASM-remote;
  rung 4 scopes offline to desktop or builds browser-native equivalents
  (IndexedDB queue, online/offline events).
